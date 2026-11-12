//
//  vgpuFramebuffer.cpp — an IOFramebuffer over the VMware SVGA II device's own VRAM.
//
//  Stage 2 of the B0 gate. Stage 1 proved a framebuffer we wrote ourselves can be published
//  through IOFramebuffer at all; this is the step that makes it visible, by putting the
//  system aperture on the exact memory the adapter is already scanning out.
//
//  The design decision worth understanding is that this kext does *not* set a video mode.
//  The device is already enabled and programmed by the time we run -- firmware and the
//  boot-time NDRV driver got there first -- and SVGA_REG_FB_OFFSET plus SVGA_REG_BYTES_PER_LINE
//  say precisely which bytes are on screen. So we read those and inherit them. Inheriting is
//  both less code and far less dangerous than a modeset: nothing we do can change what the
//  display controller is doing, so the worst case is a picture drawn in the wrong place rather
//  than a device left in a state nobody can program.
//
//  Measured on this guest when this was written:
//
//      svga id 0x90000002 enable 1
//      svga mode 1024x768 depth 24 bpp 32 pitch 4096
//      svga max 6688x5016
//      svga fb offset 0x0 fb size 0x300000
//      svga vram 0x8000000 fb start 0xf0000000 caps 0xfdff83e2
//
//  So the visible framebuffer is VRAM[0 .. 0x300000) pitched at 4096. See
//  docs/vmware-svga-capabilities.md for what this device can and cannot accelerate, and
//  notes on the Metal translation path that eventually has to sit on top of it.
//
//  Modelled on refs/MacHyperVSupport/MacHyperVFramebuffer for the IOFramebuffer surface.
//

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>

#define VGPU_FB_TAG "vgpu-fb"

// VRAM is BAR1 -- 128 MB at 0xf0000000 on this guest, and the memory the adapter scans out.
static const UInt32 kVgpuVramBarIndex = 1;

// A kernel collection has no separate Info.plist, so identity travels inside the binary
// as `_kmod_info`. See docs/kext-loading.md, gate 2.
extern "C" kern_return_t vgpuFramebufferKmodStart(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t vgpuFramebufferKmodStop(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

extern "C" {
KMOD_EXPLICIT_DECL(com.vgpu.framebuffer, "1.0.0", vgpuFramebufferKmodStart, vgpuFramebufferKmodStop)
}

//
// ---------------------------------------------------------------------------
// VMware SVGA II register access
// ---------------------------------------------------------------------------
//
// The registers are not memory mapped. BAR0 is a 16 byte I/O port block: write a register
// index to SVGA_INDEX_PORT, then read or write the value at SVGA_VALUE_PORT. That is what the
// boot-time NDRV driver does, and it is the only way to find out what the device is doing.
//
// Only reads are issued. Writing SVGA_REG_ID renegotiates the device protocol version and
// drops the device back to legacy mode if the host does not recognise the value, so the
// version is left alone. Every register read below is meaningful in every revision.
//
//
// Every index below was read off this host's own driver (vm3dmp.sys 9.17.11.0003) or off a
// live probe of the register window -- NOT inferred from the upstream svga_reg.h. The header
// this tree was originally written against has a different layout: it skips a block of indices
// after BITS_PER_PIXEL, so assuming 0,1,2,3... puts everything above 7 at the wrong port.
// docs/svga-register-map.md is the authority; §40 and §41 of cb-findings record how it was
// established. The two mistakes that cost the most were:
//
//   - 0x14 is SVGA_REG_RESET, not CONFIG_DONE. Writing 0 to it resets the device, which drops
//     every mode the host had established (including the legacy auto-scanout). The register was
//     labelled CONFIG_DONE here for eight rounds and written on every stage>=1 boot (§42).
//   - 0x16 is the SYNC status the host polls, not SYNC itself; SYNC is write-only at 0x15.
//
enum {
    kSvgaRegId           = 0x00,
    kSvgaRegEnable       = 0x01,
    kSvgaRegWidth        = 0x02,
    kSvgaRegHeight       = 0x03,
    kSvgaRegMaxWidth     = 0x04,
    kSvgaRegMaxHeight    = 0x05,
    kSvgaRegDepth        = 0x06,
    kSvgaRegBitsPerPixel = 0x07,
    kSvgaRegBytesPerLine = 0x0C,
    kSvgaRegFbStart      = 0x0D,
    kSvgaRegFbOffset     = 0x0E,
    kSvgaRegVramSize     = 0x0F,
    kSvgaRegFbSize       = 0x10,
    kSvgaRegCapabilities = 0x11,
    kSvgaRegMemStart     = 0x12,
    kSvgaRegMemSize      = 0x13,

    // Registers 20..64, checked line by line against upstream svga_reg.h
    // (drivers/gpu/drm/vmwgfx/device_include/svga_reg.h -- note the device_include/ directory;
    // the file is no longer at the driver root, which is why earlier attempts to fetch it 404'd
    // and why this block spent several rounds wrong):
    //
    //     SVGA_REG_CONFIG_DONE  = 20   <-- 0x14
    //     SVGA_REG_SYNC         = 21   <-- 0x15
    //     SVGA_REG_BUSY         = 22   <-- 0x16
    //     SVGA_REG_MEM_REGS     = 30   <-- 0x1E
    //     SVGA_REG_COMMAND_LOW  = 48   <-- 0x30
    //     SVGA_REG_COMMAND_HIGH = 49   <-- 0x31
    //     SVGA_REG_DEV_CAP      = 52   <-- 0x34
    //     SVGA_REG_CMD_PREPEND_LOW/HIGH = 53/54
    //     SVGA_REG_GUEST_DRIVER_ID      = 61, VERSION1..3 = 62..64
    //     SVGA_REG_FIFO_CAPS    = 68   <-- capabilities are a register, not FIFO memory
    //     SVGA_REG_FENCE        = 69
    //
    // Three separate errors lived in this block and each one alone was enough to make the
    // device ignore every command we sent:
    //
    //   1. CONFIG_DONE was written to 25. 25 is SVGA_REG_CURSOR_X. The host was therefore
    //      never told the FIFO had been configured, and a host that does not believe a FIFO
    //      exists does not drain it -- which is exactly what §44 measured: NEXT_CMD advanced
    //      and SYNC pinged, STOP never moved, FENCE never written.
    //   2. 20 (0x14) was labelled RESET and the code wrote 0 to it "to clear the flag". There
    //      is no SVGA_REG_RESET in the device definition at all; 0x14 *is* CONFIG_DONE, so the
    //      write-0-then-write-1 pair was correct all along and had been "fixed" into 0x19.
    //   3. Command submission used 53/54, which are the PREPEND registers. Command low/high are
    //      48/49. (The original 48/49 was right and was changed away in §40.)
    kSvgaRegConfigDone   = 0x14,   // 20: guest sets 1 once the FIFO words are coherent
    kSvgaRegSync         = 0x15,   // 21: write 1 = flush, 2 = flush and wait
    kSvgaRegBusy         = 0x16,   // 22: host clears this when the flush completes
    kSvgaRegMemRegs      = 0x1E,   // 30: number of FIFO register dwords the host defines
    kSvgaRegCommandLow   = 0x30,   // 48: command PA low 32 bits, OR context id in the low 6
    kSvgaRegCommandHigh  = 0x31,   // 49: command PA high 32 bits
    kSvgaRegDevCap       = 0x34,   // 52: write a SVGA3D_DEVCAP index, read its value
    kSvgaRegCmdPrependLow  = 0x35, // 53: preempted command buffer PA, low
    kSvgaRegCmdPrependHigh = 0x36, // 54: preempted command buffer PA, high
    kSvgaRegGuestDriverId  = 61,
    kSvgaRegFifoCaps     = 68,     // the capability word the host actually publishes
    kSvgaRegFence        = 69,     // the fence value register

    // Three registers the ScreenTarget work needed and had never even been read (§45).
    //
    //   SVGA_REG_BLANK_SCREEN_TARGETS = 58 -- write 1 to put every screen target into the
    //        "blank" (uncomposited) state, 0 to bring them back. A screen target that is
    //        blank never samples its attached surface, which would explain a ScreenTarget
    //        that accepts GB_SCREEN_DMA and dispenses zeroes.
    //   SVGA_REG_CAP2 = 59 -- the second capability word. SVGA_CAP2_SCREENDMA_REG (0x10)
    //        says the SVGA_REG_SCREENDMA control register exists at all.
    //   SVGA_REG_SCREENDMA = 75 -- controls the hardware screen-DMA path:
    //        UNDEFINED 0 / NOT_PRESENT 1 / PRESENT 2. Under PRESENT the host performs the
    //        screen-to-memory copy in hardware; NOT_PRESENT means the guest must not expect
    //        GB_SCREEN_DMA to return anything. That distinction decides whether §45's
    //        "0/524288 words" measurement was a real result or a measurement of a disabled
    //        feature -- read it before drawing any conclusion from the DMA probe.
    kSvgaRegBlankScreenTargets = 58,
    kSvgaRegCap2         = 59,
    kSvgaRegScreenDma    = 75,

    // The four limits that decide whether the screen target we define is even legal here.
    // Every one of these was unread until §45; a 1024x512 target rejected by the host for
    // exceeding SCREENTARGET_MAX_* looks exactly like a target that accepts commands and
    // displays nothing, which is the symptom we have been chasing for three rounds.
    kSvgaRegScreenTargetMaxWidth  = 55,
    kSvgaRegScreenTargetMaxHeight = 56,
    kSvgaRegMobMaxSize   = 57,     // bytes; the real ceiling for our page-table MOBs
    kSvgaRegGbObjectMemSizeKb = 76, // total GB-object memory the host will dole out
};

// svga_reg.h: SVGA_SCREENDMA_REG_{UNDEFINED,NOT_PRESENT,PRESENT}
enum { kSvgaScreenDmaUndefined = 0, kSvgaScreenDmaNotPresent = 1, kSvgaScreenDmaPresent = 2 };
// svga_reg.h: SVGA_CAP2_SCREENDMA_REG
static const UInt32 kSvgaCap2ScreenDmaReg = 0x00000010;

//
// FIFO registers are dwords inside the device memory region that SVGA_REG_MEM_START points
// at. The first four (MIN/MAX/NEXT_CMD/STOP) are BYTE offsets into that region; every other
// index is a dword index. The register block occupies the first SVGA_FIFO_MIN bytes, so the
// guest chooses how many registers exist by where it starts the command data.
//
// MEASURED, not inferred (kexts/probe _dumpFifoHeader, §43.5 in cb-findings): after the host
// configured this FIFO it had written exactly six dwords, and they were at:
//
//   [0] 0x480 MIN   [1] 0x40000 MAX   [2] 0x4a8 NEXT_CMD   [3] 0x480 STOP
//   [288] 0x45b   <- SVGA3D_HWVERSION (protocol 1115)
//   [289] 0x18    <- the companion version word
//
// So 3D_HWVERSION lives at 288, not at 7 as the upstream header has it, and the capability
// block is NOT populated by this host at all (only 6 of 291 dwords were non-zero). Both of
// those are consistent with §40: this host's register layout is not the one in svga_reg.h.
//
// The consequence used to be fatal: with MIN = 1152 the command area began at dword 288,
// exactly on top of the host's 3D version word, so the first command we pushed overwrote it.
// MIN must therefore clear MEM_REGS (291), not 288.
enum {
    kSvgaFifoMin              = 0,      // byte offset: start of the command data area
    kSvgaFifoMax              = 1,      // byte offset: end of the FIFO

    // NEXT_CMD and STOP were documented the other way round for most of this project, and
    // that inversion is what made every submission look like it "should" work while the
    // host ignored it. The authority is vmw_local_fifo_commit() in vmwgfx_cmd.c:
    //
    //     next_cmd += bytes;  ...  vmw_fifo_mem_write(dev_priv, SVGA_FIFO_NEXT_CMD, next_cmd);
    //
    // The GUEST owns NEXT_CMD and advances it. The HOST owns STOP: it advances STOP as it
    // consumes, which is why vmw_fifo_is_full() computes free space as
    // (MAX - NEXT_CMD) + (STOP - MIN) and why vmw_fifo_wait() sleeps until STOP moves.
    kSvgaFifoNextCmd          = 2,      // byte offset: GUEST write position (guest advances)
    kSvgaFifoStop             = 3,      // byte offset: HOST read position (host advances)
    kSvgaFifoCapabilities     = 4,      // dword index: SVGA_FIFO_CAP_* bits (reads 0 here)
    kSvgaFifoFlags            = 5,
    kSvgaFifoFence            = 6,      // dword index: last completed fence value
    kSvgaFifoReserved         = 14,     // dword index: bytes past NEXT_CMD being written
    kSvgaFifo3dCaps           = 32,     // dword index: start of the 3D capability block
    kSvgaFifo3dCapsCount      = 256,    // through SVGA_FIFO_3D_CAPS_LAST
    kSvgaFifo3dHwVersion      = 288,    // dword index: host's SVGA3D protocol version -- MEASURED
    kSvgaFifo3dHwVersion2     = 289,    // dword index: companion word, MEASURED
    kSvgaFifoBusy             = 290,

    // Where command data starts. The host defines registers through index 290 (MEM_REGS =
    // 291), and it writes 3D_HWVERSION into 288, so a MIN of 288 dwords (1152 bytes) put the
    // first command on top of it. 292 dwords clears the block with one dword of slack.
    kSvgaFifoExtendedMandatoryRegs = 292,
};

// SVGA_FIFO_CAPABILITIES bits, from the SVGA II definitions.
enum {
    kSvgaFifoCapFence   = 1 << 0,
    kSvgaFifoCapReserve = 1 << 6,
};

// Legacy FIFO commands. Each is a raw dword stream: the command id, then its parameters
// (no SVGA3dCmdHeader -- that wrapper belongs to the 3D command set). SVGA_CMD_FENCE
// carries one parameter, and the host writes the value into SVGA_FIFO_FENCE once every
// command before it has been processed: the simplest end-to-end proof that the command
// stream is live.
enum {
    kSvgaCmdFence = 30,
    kSvgaCmdUpdate = 1,   // [x][y][width][height]: framebuffer rect changed, rescan it
    kSvgaCmdDefineScreen = 34,
    kSvgaCmdDefineGmrfb = 36,
    kSvgaCmdBlitGmrfbToScreen = 37,
};

// SVGA_GMR_FRAMEBUFFER: a SVGAGuestPtr gmrId that addresses the legacy framebuffer (BAR1)
// directly; the ptr offset is a byte offset into the BAR. This lets the screen-object
// present path read the console framebuffer without any guest-side copy.
static const UInt32 kSvgaGmrFramebuffer = 0xFFFFFFFE;

// SVGAGMRImageFormat: bitsPerPixel | colorDepth << 8. Our inherited mode is bpp 32, depth 24
// (32-bit BGRX) -- one of the formats the GMRFB blits support.
static const UInt32 kSvgaGmrfbFormatBgrx32 = 32 | (24 << 8);

// SVGA_SCREEN flags.
static const UInt32 kSvgaScreenIsPrimary = 1 << 1;

//
// ---------------------------------------------------------------------------
// Guest-backed objects (the vGPU10 / GBOBJECTS path)
// ---------------------------------------------------------------------------
//
// 3D commands carry an SVGA3dCmdHeader {id, size} before their bodies. The object model is
// table-driven: the guest allocates object tables (OTables) in guest memory, points the
// host at them with SVGA_3D_CMD_SET_OTABLE_BASE64, and then fills entries as it defines
// objects. Definitions come in pairs -- the guest writes its half of the state (the OTable
// entry) AND sends the definition command; the host validates both against each other.
//
// A MOB (memory object) is described by a page table of PPN64s. With SVGA3D_MOBFMT_PT64_0
// the MOB's "base" PPN points at one page-table page holding up to 512 PPN64 entries, which
// is exactly one 4 KiB table page describing up to 2 MiB of guest memory. That form works
// wherever the guest pages happen to live, contiguous or not.
//
// Command ids and layouts are from the SVGA3D definitions used by Linux's vmwgfx
// (drivers/gpu/drm/vmwgfx/device_include/svga3d_cmd.h, svga3d_types.h) at the protocol
// generation Workstation 17 implements.
enum {
    kSvga3dCmdSetOtableBase64 = 1115,
    kSvga3dCmdDefineGbMob64   = 1135,
    kSvga3dCmdUpdateGbMobMapping = 1096,  // {mobid}: tell the host to re-read the PT
    kSvga3dCmdDefineGbSurface = 1097,
    kSvga3dCmdBindGbSurface      = 1099,   // SVGA3dCmdBindGBSurface {sid, mobid}
    kSvga3dCmdUpdateGbSurface    = 1102,   // SVGA3dCmdUpdateGBSurface {sid}: re-read the MOB
    kSvga3dCmdUpdateGbImage      = 1101,   // {sid, box}: re-read a sub-box of the MOB
    kSvga3dCmdGbScreenDma        = 1131,   // {screenId, dead, destMobID, destPitch, mapMob}
    kSvga3dCmdDefineGbContext    = 1107,
    kSvga3dCmdBindGbContext      = 1109,
    kSvga3dCmdReadbackGbImage    = 1103,
    kSvga3dCmdSetRenderTarget    = 1050,
    kSvga3dCmdSetViewport        = 1055,
    kSvga3dCmdClearRt            = 1057,
    kSvga3dCmdDxDefineContext = 1143,
    kSvga3dCmdDefineGbScreenTarget = 1124,
    kSvga3dCmdDestroyGbScreenTarget = 1125,
    kSvga3dCmdBindGbScreenTarget   = 1126,
    kSvga3dCmdUpdateGbScreenTarget = 1127,
    kSvga3dCmdSurfaceDestroy       = 1041,
    kSvga3dCmdDestroyGbMob         = 1094,
    kSvga3dCmdDxBindContext        = 1145,
    // 10-29 21:5x: this used to be 1277. Per svga3d_cmd.h the neighbours are
    //   SVGA_3D_CMD_DX_SET_SHADER_IFACE  = 1274
    //   SVGA_3D_CMD_DX_BIND_STREAMOUTPUT = 1275
    //   SVGA_3D_CMD_SURFACE_STRETCHBLT_NON_MS_TO_MS = 1276
    //   SVGA_3D_CMD_DX_BIND_SHADER_IFACE = 1277   <-- what we were actually sending
    // so our STATUS 3 on "SET_SHADER_IFACE" was simply the wrong command: BIND expects
    // {cid, shaderType, shaderId}, we sent {cid, stateMobId, 0x2000}. Keep the correct
    // id here; the send site is still a runner (not a gate) because 3D caps are 0.
    kSvga3dCmdDxSetShaderIface     = 1274,   // 0x4fa: {cid, ifaceMobid, prependOffset}
    kSvga3dCmdDxSetCotable         = 1207,
    kSvga3dCmdDxDefineRenderTargetView = 1187,
    kSvga3dCmdDxSetRenderTargets   = 1161,
    kSvga3dCmdDxSetViewports       = 1174,
    kSvga3dCmdDxSetScissorRects    = 1175,
    kSvga3dCmdDxClearRenderTargetView = 1176,
    kSvga3dCmdReadbackGbSurface    = 1104,
    kSvga3dCmdDxReadbackSubResource = 1183,
    kSvga3dResourceTexture2D       = 3,
};

enum {
    kSvgaOtableMob          = 0,
    kSvgaOtableSurface      = 1,
    kSvgaOtableScreenTarget = 4,
    kSvgaOtableDxContext    = 5,
};

enum {
    kSvga3dMobFmtPt64_0 = 4,    // no PT level: the base page IS the (single) data page
    kSvga3dMobFmtPt64_1 = 5,    // one PT level: base page holds PPN64s of the data pages
    kSvga3dMobFmtPt64_2 = 6,    // two PT levels: base -> L1 PT pages -> data pages
};

// SVGA_STFLAG_PRIMARY.
static const UInt32 kSvgaStflagPrimary = 1 << 0;

// SVGA3D_DEVCAP indices, from svga3d_devcaps.h.
enum {
    kSvgaDevcap3d        = 0,
    kSvgaDevcapDxContext = 95,
    kSvgaDevcapSm41      = 244,
    kSvgaDevcapGl43      = 261,
};

enum {
    kSvga3dSurfaceFormatX8R8G8B8 = 1,
};

enum {
    kSvga3dSurfaceHintDynamic = 1ULL << 2,
    kSvga3dSurfaceHintTexture = 1ULL << 5,
    kSvga3dSurfaceScreentarget = 1ULL << 16,   // required for STDU-bound surfaces
    kSvga3dSurfaceHintRenderTarget = 1ULL << 6,
    kSvga3dSurfaceBindRenderTarget = 1ULL << 24,
};

// kFbStage gates the FIFO bring-up itself:
//   0 = read-only probe: register reads and dumps only, no FIFO writes, no fence
//   2 = FIFO init only (MIN/MAX/CONFIG_DONE=1 at 0x19), no guest commands at all
//   3 = init + one fence command
//   4 = init + fence + full-screen UPDATE probe
//   5 = init + fence + legacy screen-object present path (DEFINE_SCREEN, DEFINE_GMRFB over
//       the framebuffer, BLIT_GMRFB_TO_SCREEN) + the periodic present timer
//
// HISTORY THAT NO LONGER APPLIES -- read §41/§42 in cb-findings before trusting the old
// notes that used to live here. The 2026-10-06 bisect concluded "the moment the guest
// writes CONFIG_DONE, the MKS switches to the screen-target model and presentation
// freezes". That experiment could not have shown it: the register it called CONFIG_DONE
// was 0x14, which is RESET, so those boots reset the device instead of configuring the
// FIFO -- and separately, every command was submitted to 48/49 rather than 0x35/0x36, so
// "commands accepted but nothing rendered" is equally explained by the host never seeing
// a command at all.
//
// Consequence: stage 2 is an UNTESTED configuration. It is the first boot in this
// project's history that raises CONFIG_DONE without resetting the device.
// §47: 3 -> 5, and this is the configuration that ships. Stage 5 is the legacy
// screen-object present path (DEFINE_SCREEN + DEFINE_GMRFB + BLIT_GMRFB_TO_SCREEN, re-blit
// every kPresentIntervalMs), and it is the only path on this host that has ever put our own
// pixels on the screen: the first boot with it rendered the macOS lock screen live, and the
// desktop after logging in through VNC. It costs one FIFO command per frame and no guest
// memory at all, because the GMRFB points straight at the framebuffer in BAR1.
//
// Everything the STDU path spent §36-§47 chasing -- MOBs, page tables, object-table
// entries, screen-target binds, surface updates -- turns out to be unnecessary for a
// working display on this host. See kGbStage below.
static const UInt32 kFbStage = 5;

// kGbStage gates the GB object stack (see gbBringUp); 5 adds the STDU present path.
//
// §47: 5 -> 0. The STDU path never produced a lit pixel here, and the control arm finally
// said why in one line: with the STDU bound and updated at 30 Hz, painting MAGENTA into
// VRAM and the red/green pattern into the surface produced a capture that was black --
// neither source was being presented. Once the guest takes the FIFO, this host presents
// only what the driver blits to a screen object; sampling a bound screen target is not
// something it does for us. That makes every STDU experiment of the last three weeks a
// measurement of a path this host does not implement, and it makes kFbStage=5 the answer.
//
// The STDU code stays: it is the vGPU10 present path a real driver would use, the object
// model is proven (DEFINE/BIND/UPDATE all fence-verified, page tables resolve), and the day
// the reason for the missing sample-and-composite turns up, the way back is one number.
// Note kDxTest must stay 0 while this is 5 -- see below.
static const UInt32 kGbStage = 0;

// kDxTest runs the DX clear/readback round-trip experiment after the present path is up.
// Measured 2026-10-06: with the test present, the host aborts FIFO processing at
// DEFINE_GB_SCREENTARGET (deterministically, next/stop frozen mid-stream) even though the
// identical byte stream passed in earlier boots and the test itself runs only afterwards.
// Root cause unknown -- bisect with this switch next session.
//
// 10-29: switched OFF. DEFINE_RTV is dead (§35), so what was left of this test was the
// SURFACE_DMA round trip -- and that path is already proven by M6's live display. Running
// it alongside STDU also re-arms the exact hang described above. Turn it back on only to
// bisect that hang, never together with kGbStage = 5 on a boot we care about.
static const UInt32 kDxTest = 0;

// kStduPaint overlays a known pattern (red border, green top half, blue bottom half) on
// the surface every present tick, ON TOP of the VRAM->surface copy.
//
// Purpose (§36.5): the STDU path is up and the FIFO advances 300 ticks without wedging,
// yet the screen is pure black. That is consistent with two OPPOSITE conclusions -- the
// present path is broken, or the present path is fine and VRAM is simply black because
// nothing on this guest is drawing into it. Our own pixels settle it in one boot.
// Switch it off once there is real content to show; it would otherwise cover it up.
static const UInt32 kStduPaint = 1;

// kStduReadback asks the host to hand surface 2 back to us (§36.8 step 1).
//
// §36.7 proved the buffer we DMA from holds the right pattern and the screen is
// still black, so the fault lies between "host consumed the command" and "a pixel
// appears". This test splits that interval in two: once, after the first painted
// frame, submit SURFACE_DMA with transfer = SVGA3D_READBACK (0) into a SEPARATE
// buffer and compare it against the pattern.
//
//   match    -> the DMA target really is our surface/MOB, so the break is on the
//               screen-target/display side (P3, fresh-MKS topology).
//   mismatch -> the host's notion of what sid 2 / mobid 2 points at differs from
//               ours, i.e. the mapping is the problem.
//
// Read-back goes into its own allocation on purpose: reading back into _stduVirt
// would let a broken DMA "succeed" by simply leaving the pattern we already
// wrote there.
//
// §38e: off. §37's "SURFACE_DMA is a no-op" verdict turned out to be a measurement
// artifact (§38g: the transfer field was 0, which is not a valid SVGA3dTransferType --
// READ_HOST_VRAM is 2), so the present loop is put back to the shape that has a defined
// transfer value, and the read-back probe is re-enabled to settle it in one boot.
static const UInt32 kStduReadback = 1;

// kStduPt1 picks the MOB page-table depth for the STDU backing store (§38).
//
// §45: back to 0. This is the M6 shape (§b89c619), the only configuration on this project
// that ever put pixels on the screen, and it says so in its own commit message:
//
//   "3. The 3 MB backing MOB needs PT64_2 (768 pages -> 2 L1 PT pages -> root),
//       not PT64_1 -- level count from vmwgfx's vmw_mob_pt_setup."
//
// PT64_1 was adopted afterwards purely to make 512 pages fit one level, which required
// shrinking the surface from 1024x768 to 1024x512 -- i.e. it changed the display mode to
// suit the page-table shape, which is backwards. Both the two-level walk and the 3-page PT
// allocation are still present below; only this switch was flipped.
//
// 1 = PT64_1: ONE level, one 4 KiB page holding 512 entries, covering 512 * 4 KiB = 2 MiB.
// 2 = PT64_2: TWO levels, a root page naming the second-level pages, 768 entries over
//             2 second-level pages.
static const UInt32 kStduPt1 = 0;

// kStduTopology restores the SVGA_REG_DISPLAY_* block (registers 35..40) before the screen
// target is defined (§45).
//
// This is the single most important line in M6's commit message and it was deleted by
// accident in 0e80bab:
//
//   "1. Display topology: SVGA_REG_DISPLAY_ID/IS_PRIMARY/POSITION_X/Y/WIDTH/HEIGHT
//       (registers 35-40) must be programmed -- without a declared display the MKS renders
//       nothing regardless of screen targets."
//
// How it got deleted is worth recording, because the mistake is a decimal/hex mix-up and
// not a judgement call. 0e80bab's comment says:
//
//   "That set is exactly ... 0x35/0x36 CMD_PREPEND_LOW/HIGH ... It contains NO
//    display-topology write (those are 35..40 = 0x23..0x28 ...)"
//
// 0x35/0x36 (hex) is decimal 53/54 = PREPEND, which is correct. 35..40 (decimal) is
// 0x23..0x28 = DISPLAY_*, which is also correct. Both statements are true and they are
// about different registers -- but the block that was actually removed was the decimal
// 35..40 one, i.e. the display topology, on the strength of an argument about 0x35/0x36.
//
// There is also a real difference between M6's version and the one 0e80bab removed: M6
// wrote the six values and stopped, while the later version appended
// `SVGA_REG_DISPLAY_ID = 0xFFFFFFFF` as a "deselect". Deselecting the display right after
// declaring it is the opposite of declaring it, and 2026-10-07's "these writes wedge the
// FIFO" matrix was collected with that deselect in place. So: declare, do not deselect.
static const UInt32 kStduTopology = 1;

// Height actually used by the STDU surface. 512 when kStduPt1 == 1 so that the page
// count fits a single-level table; otherwise the full inherited mode. The literals are
// spelled out because kStduWidth/kStduHeight are declared further down (§352 vs §525);
// keep them in sync with those.
//
// §45: 768 is back, because kStduPt1 is back to 0 (M6's PT64_2 shape). 1024x768 * 4 =
// 3 MiB = 768 pages, which needs exactly the two second-level pages the PT allocation
// already provides. This is also the mode the host is actually scanning out (SVGA_REG_WIDTH
///HEIGHT read 1024x768 at load), so a 1024x512 surface was always a partial frame.
// §47: the host's Screen 0 is 640x480, so make the surface 640x480 too.
//
// This switch exists because of a log line that had never been produced before this boot.
// Once SVGA_REG_ENABLE carries HIDE, the host answers with
//
//     SVGA hiding SVGA
//     SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1024, 768) flags=0x2
//     SWBScreen: Screen 0 Defined:   xywh(0, 0,  640, 480) flags=0x3
//
// i.e. it tears down the legacy screen, stands up a *guest* screen (Screen 0, flags 0x3)
// and hands the display to us -- the one behaviour every previous boot lacked. But it
// builds that screen at 640x480, not at the 1024x768 it just destroyed, and not at the
// 1024x768 the DISPLAY_* topology registers read back. The screen target we bind is
// 1024x768, so the host is compositing a 640x480 screen from a surface that describes
// itself as 1024x768 -- a mismatch that is a perfectly good explanation for "bound,
// updated, and still black", and a cheap one to remove.
//
// So: 640x480 everywhere. Same code path, same commands, same order; only the geometry
// moves. If the painted pattern appears, the present path is complete and the remaining
// work is to make the host adopt *our* mode instead of us adopting the host's. If the
// screen stays black at a matched geometry, then geometry is exonerated and the fault is
// in how the host reads the MOB.
//
// Pages: 640*480*4 = 1228800 bytes = 300 pages, which still fits one second-level table
// (512 entries), so the PT shape below is unchanged.
static const UInt32 kStduMatchHost = 0;

static const UInt32 kStduSurfWidth  = kStduMatchHost ? 640 : 1024;
static const UInt32 kStduSurfHeight = kStduMatchHost ? 480
                                   : (kStduPt1 == 1) ? 512 : 768;
static const UInt32 kStduSurfPages  = kStduSurfWidth * kStduSurfHeight * 4 / 4096;

// kStduDma = 0 removes SURFACE_DMA from the present loop (§37).
//
// §37.3 proved the DMA moves nothing: a READBACK into a zeroed 3 MB scratch buffer
// returned 0/768 sampled words nonzero, i.e. the engine never touched the memory.
// M6 (b89c619) shipped a live display with UPDATE_GB_IMAGE as the documented content
// path, so the DMA is not required -- and an unanswered DMA may be worse than useless,
// if it leaves the host's surface marked busy and it skips the proxy update too.
// This switch makes the loop M6-shaped so the two can be told apart.
//
// §38e: now off. §37 already answered the question (§37.3 measured the engine moving
// nothing), and while it was on, the extra 21-dword DMA per frame was the only thing
// pushing ~90 bytes/tick of traffic to a rate that walked the ring into the FIFO_MAX
// wrap window every ~4 minutes. M6's shape is what we actually want to ship.
//
// §38g: back on. §37.3 was measuring transfer == 0, an undefined SVGA3dTransferType
// (READ_HOST_VRAM is 2), so "the engine moves nothing" was never established. Keeping
// the upload in the loop also gives the read-back probe something to read: without a
// WRITE_HOST_VRAM first, the host's copy of the surface is whatever it was at BIND.
// The §38d FIFO wrap deadlock that this switch used to expose is fixed independently.
static const UInt32 kStduDma = 0;

// kStduImage puts UPDATE_GB_IMAGE (1101) back in the per-frame present loop (§45).
//
// §38.8 removed it on the grounds that upstream's STDU CPU path sends only
// UPDATE_GB_SCREENTARGET. That was true of the code being read and false about this
// host, because the measurement it rested on was taken while every surface we created
// had a mis-filled OTable entry (surfaceFlags written as 64 bits into a 32-bit field --
// see surfaceEntryFill). Under that defect the host's surface described width=0/height=1,
// so no upload path could have worked, and "the DMA already handles it" was never a
// finding, only an assumption.
//
// M6 (b89c619) shipped a live display and names 1101 as the command that did it. It is a
// separate switch so one boot can attribute the result to this command alone.
static const UInt32 kStduImage = 1;

// kStduScreenDma runs GB_SCREEN_DMA once on the normal (non-flat) present arm (§45b).
// It is the only command that reports what the host is actually displaying, so it is what
// tells "our upload never landed" apart from "the host never samples the surface".
// SVGA_REG_BLANK_SCREEN_TARGETS is write-1-blank / write-0-unblank. Upstream only
// touches it on suspend/resume, but nothing says a guest that defines its own target may
// assume the "unblank" default -- and a blanked target explains every measurement we have
// with no other defect required. Its own switch because a write to an unimplemented
// register on this device class is not guaranteed harmless.
static const UInt32 kStduUnblank = 1;
static const UInt32 kStduScreenDma = 1;

// kStduEnableHide writes SVGA_REG_ENABLE = ENABLE_ENABLE | ENABLE_HIDE (3) before
// CONFIG_DONE (§47).
//
// This is the one step in upstream's bring-up that we never copied. vmwgfx_drv.c
// vmw_device_init() is unambiguous about the order (line 423-434):
//
//     dev_priv->enable_state = vmw_read(dev_priv, SVGA_REG_ENABLE);
//     ...
//     vmw_write(dev_priv, SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE |
//                                           SVGA_REG_ENABLE_HIDE);
//     vmw_write(dev_priv, SVGA_REG_TRACES, uses_fb_traces);
//     dev_priv->fifo = vmw_fifo_create(dev_priv);      <-- CONFIG_DONE happens in here
//
// So on a real driver the HIDE bit is set BEFORE the FIFO exists, and vmw_svga_enable()
// (line 1330) is the only place that clears it back to ENABLE -- and that call is about
// "let me use VRAM as a memory pool", not about displaying anything.
//
// We measured this host's boot value and it is 1: the device comes up with ENABLE set and
// HIDE clear, i.e. "nobody has taken over the display, keep scanning the legacy
// framebuffer out". That is exactly the state in which the desktop is visible (stage 0
// boot). Then we write CONFIG_DONE, the host's ScreenMgr switches to ScreenTarget, its
// scanout source disappears -- and the HIDE bit that would have told it "a guest driver
// owns the display now, composite the screen targets instead" was never set.
//
// That reading explains every black-screen boot in the matrix with one missing write,
// and it explains why nothing about the surface, the MOB, the page table or the update
// rectangle ever mattered: the host was never asked to composite.
static const UInt32 kStduEnableHide = 0;

// Dump every CB header+payload to the log (bisect instrumentation).
static const UInt32 kCbDump = 1;

// 2026-10-25: CB header flag probe. The header validator
// (vmware-vmx.exe!0x140087c10) runs, in this order:
//     if (header.length == 0) -> COMPLETED          <-- returns BEFORE the flags test
//     page-crossing check on ptr.pa
//     call 0x140607760
//     test header.flags, 0xFFFFFFFE -> CB_HEADER_ERROR(4)
// So a length==0 CB with flags=2 answers the standing question: do DX-flag CBs
// traverse THIS validator (the only gate we have ever seen write status 4), or do
// they take a different submission path? COMPLETED = this is the only gate;
// status 4 = flags=2 never reaches this code, i.e. another path exists.
// Device queue (0x3f) only: device-queue errors never wedge the queue.
static const UInt32 kFlagsProbe = 1;

// 2026-10-25: packed-CB variant of the RTV test ({BIND,RTV} etc). It failed while
// the host still rejected flags bit1 and costs 3 queue-0 error slots, so it is off
// by default now that the real blocker (DX manager never created) is known.
static const UInt32 kPackedTest = 0;

// 2026-10-27: run the vm3dmp create-context template instead of the old
// DEFINE/BIND/COTABLE scanner (see the kDxVm3dmpTemplate block in
// dxReadbackTest). The scanner is kept for reference but is skipped while
// this is 1; the template ends the boot with exactly ONE DX render command.
static const UInt32 kDxVm3dmpTemplate = 1;
// 10-28 23:28: the template DID run, but only after the legacy GB/DX chain ahead of it
// had already wedged the command engine: cb[63] (DEFINE_RTV q1 dx1 flags2) hit the 1.3 s
// timeout with status still 0, and cb[64]/cb[65] never completed either -- the queue
// stops answering after the FIRST DX error (documented in Â§22), so every command the
// template sent was already dead on arrival. This switch skips the whole legacy chain so
// the template runs on a pristine engine. One boot = one clean experiment.
static const UInt32 kDxTemplateOnly = 1;

// 2026-10-25: submit command buffers in MOB form (SVGA_CB_FLAG_MOB). vmwgfx's
// primary path is MOB-based (vmw_cmdbuf.c:892-896): the CB header itself keeps a
// normal DMA address and is still submitted through SVGA_REG_COMMAND_LOW/HIGH,
// but ptr becomes {u32 mobid, u32 mobOffset} pointing at a GB MOB and flags gets
// bit2. The host's DX manager object (0x1414930A0) -- whose null check rejects
// every render command -- is only created on that form.
// 10-29 20:17: MOB form + dxContext=1 + flags(=NO_IRQ|MOB after stripping DX_CONTEXT)
// came back STATUS 4, errorOffset 0. That is the *header* validator, docs §18.4:
//
//     test dword header.flags, 0xFFFFFFFE  -> CB_HEADER_ERROR(4)
//
// i.e. bit0 is the only bit that may be set, and MOB is bit2. MOB form cannot be
// expressed on this host at all -- not because of DX, because of the validator. The
// earlier "vm3dmp uses the MOB form" note in the comment above was an inference from
// vmw_cmdbuf.c (Linux), not from this host's behaviour; vm3dmp on THIS host must be
// using the PA form, since it renders fine and this validator is the only one there is.
// So: PA form, dxContext in the header, flags = NO_IRQ. One clean variable.
static const UInt32 kCbMobMode = 0;
static const UInt32 kCbMobId = 6;      // GB MOB id holding CB payloads

// SVGA_CB_FLAG_DX_CONTEXT. Present in the UAPI, rejected by the validator we talk to
// (docs §15). Declared here so cbSubmitCtx can strip the bit centrally instead of
// trusting every caller to remember not to set it -- the 10-29 template did set it,
// and the host answered without ever looking at the payload.
static const UInt32 kCbFlagDxContext = 2;

// 10-29 20:29: which branch of the host's CB handler a DX batch takes is decided by
// bit1 alone (0x14050a04e test r8b,2). With the bit set the host runs
// lookup-or-create(dxContext) and -- if that succeeds -- creates the DX manager object
// at 0x1414930A0 that every render command null-checks. With the bit clear it takes the
// plain command-stream branch, where DX command ids have no handler.
//
// Both settings have now been observed to fail, but for different reasons that the log
// distinguishes: bit set → STATUS 4 at the header layer; bit clear → the payload reaches
// the command parser and fails per-command. Keep it as a named switch so each boot says
// which of the two it is testing.
static const UInt32 kCbKeepDxContext = 0;

// Delay before the DX test runs (seconds after kext bring-up). Gives SSH/logd time
// to be fully live so results survive an MKS wedge.
static const UInt32 kDxTestDelayS = 90;

// Periodic DX probe: every kProbeEveryS seconds re-submit the packed DEFINE+BIND
// on the device queue (errors there do not wedge). Used while a host-side debugger
// is attached to catch the CB status write. 0 = off.
static const UInt32 kProbeEveryS = 0;

// Register the PREPEND buffer (regs 53/54). 10-08 evening correlation: with PREPEND
// registered BIND_CONTEXT was refused; the 14:04 boot without it had BIND accepted.
// 0 = skip registration entirely (single-variable test).
static const UInt32 kCbPrepend = 1;

// GUEST_DRIVER_ID (regs 61-64) + DXCONTEXT otable pre-write were BOTH added in
// 80ba752 (19:43) -- exactly the boundary where BIND_CONTEXT flipped from accepted
// (14:04 boots: neither present, verified against the persisted guest log) to
// refused (every boot since). 0 = skip both, restoring the proven 14:04 config.
//
// Now 0 on evidence, not just on that correlation: enumerating every register index
// vm3dmp.sys writes (tools/rev/reg-indices.py) shows it never touches 61-64. The comment
// that used to sit on the call site -- "vm3dmp writes exactly this at init" -- was an
// assumption inherited from the same wrong header, i.e. the same class of mistake as 0x14.
// Turning it off also removes 4 writes to registers this host has no reason to implement.
static const UInt32 kGuestDriverId = 0;

// GB-context render commands transport: 1 = plain CB (vmwgfx-style on GBOBJECTS
// devices), 0 = legacy FIFO (accepted-but-inert, see 10-10 findings).
static const UInt32 kGbcViaCb = 1;
static const UInt32 kOtablePrewrite = 0;

// STDU object teardown before the defines (see the bisect note in stduBringUp).
static const UInt32 kStduTeardown = 0;

// kStduVramControl -- the isolation experiment for §38h.
//
// The STDU path is provably correct on the guest side: the page table resolves every
// entry (pt: VERDICT), the screen target is defined and bound, the host's own log shows
// "Screen type changed to ScreenTarget" and a 1024x512 topology, and the FIFO drains
// every command with no host-side error. The one thing we cannot see from inside the
// guest is whether the host ever reads the memory we named.
//
// So stop asking the GB path and ask the *display* path instead: write the identical
// pattern straight into the legacy framebuffer aperture (BAR1, which the host has always
// been able to read -- M6 presented out of it) on every tick. Two outcomes, and both are
// informative:
//
//   VNC shows the pattern -> the display/VNC encoding chain is fine, and the fault is
//                            isolated to the host reading our GB surface.
//   VNC stays black      -> nothing we write reaches the screen through the screen
//                            target at all; look at the screen-target present step
//                            itself (UPDATE_GB_SCREENTARGET semantics / stid binding).
//
// This is a diagnostic, not a present path: it corrupts the framebuffer the guest's own
// console would otherwise use, so it goes back to 0 once it has answered. (It answered in
// §38.9: with ScreenTarget active the legacy UPDATE is ignored, so writing BAR1 changes
// nothing on screen -- keep it off.)
//
// §46: the whole round that had this at 1 was built on a false premise, so it is off again
// and the correction is recorded here rather than in a commit message nobody re-reads.
//
// The premise was that the host never leaves RegisterMode, on the strength of exactly one
// line in vmware.log:
//
//     SVGA-ScreenMgr: Screen type changed to RegisterMode
//
// That line is real but it is only the LAST transition of many. Grepping the whole file
// instead of the tail gives the actual sequence for the boot we were reasoning about:
//
//     11:36:24.805Z  Screen type changed to RegisterMode
//     11:56:09.389Z  SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1024, 768)
//     11:56:09.389Z  Screen type changed to ScreenTarget
//     11:56:09.389Z  SWBScreen: Screen 1 Defined: xywh(0, 0, 1024, 768)
//
// and again at 11:56:54.521Z, half a second after stduBringUp ran. So the host DOES enter
// ScreenTarget, it does so on our behalf, and three minutes later its encoder reports the
// pitch we chose:
//
//     MVNCEncode: screen: 0 BoundingBox: (1024x768) @ (0,0) bytesPerLine: 4096
//
// Every conclusion drawn from "the host is still in RegisterMode" -- including the idea
// that BAR1 is what the scanout reads -- is therefore void, and the arm this switch
// guarded could never have produced a meaningful reading either way. The earlier §38.9
// reading ("legacy UPDATE is ignored") is void for the same reason: it was taken while
// ScreenTarget was live, which is exactly when UPDATE is supposed to be ignored.
//
// Nothing about the present path changes because of this; it only removes a wrong
// explanation. The fault is still where §45 left it: the host is in ScreenTarget mode,
// bound to our surface, sampling at our pitch, and still shows black.
static const UInt32 kStduVramControl = 0;

// ---------------------------------------------------------------------------
// kStduFlat -- the §38.12 isolation experiment.
//
// Everything we know about the STDU path points at exactly one remaining place: the host
// never sees the content of the surface we bind to the screen target. Two very different
// faults produce that symptom and NOTHING tried so far separates them, because the current
// path exercises both at once:
//
//   (1) the host cannot resolve our MOB page table (PT64_1 / PPN64 entries / the memory
//       those PPNs name), so it reads zeros forever, or
//   (2) the page table is fine but the present chain never actually samples the surface
//       (wrong command to refresh the proxy, wrong stid, wrong rect...).
//
// kStduFlat removes (1) from the picture completely. A MOB whose whole payload fits in ONE
// 4 KiB page is defined as PT64_0, where the MOB's "page table" *is* the data page -- the
// host dereferences no page table at all, it takes basePpn straight from the otable entry.
// mksSandbox shows the host accepts a PT64_0 MOB (the DX-context MOB is one), so this half
// costs nothing to prove.
//
// With (1) gone, the content the host reads is whatever bytes we put in that page. If the
// screen still does not change, the fault is (2) and it is no longer hidden behind an
// unverifiable page table.
//
// A single page cannot describe a display, so the experiment presents a *tile*: the
// 1024x512 panel shows 32 columns of a 32-pixel-wide stripe pattern, and the 4096 bytes of
// MOB payload are exactly one row-pair (1024 px * 4 = 4096 = pitch 4096 * 1 row). Colour
// depends on the tick, so any working present path shows a flickering colour field rather
// than a still image.
// §44.7: the experiment is DONE and it answered its question.
//
// With the FIFO actually configured (§44), the flat path bound a PT64_0 MOB to the screen
// target and the host still read zeros back. That is the answer: the MOB page table is not
// the fault, the present chain is. Continuing to run the flat path now only costs us the
// display -- it binds a 1024x1 surface to the screen target, which is why the screen went
// black at 1024x512.
//
// So it is off. The normal path below builds a real kStduWidth x kStduSurfHeight surface
// (512 pages, PT64_1) which is the one that can actually fill a panel. Its earlier failures
// happened while the host was not reading the FIFO at all, so they say nothing about it.
static const UInt32 kStduFlat = 0;

// §38.13: the scratch MOB the flat readback writes into. Same numbering habit as the
// older probe (mobid 2 is the surface, +100 keeps the diagnostics out of the way).
static const UInt32 kFlatRbMobId = 102;

// §38.14: GB_SCREEN_DMA needs a full 1024x512 destination, i.e. 512 pages -- this is the
// one experiment here that genuinely needs a multi-page MOB, so it keeps PT64_1 and a
// second, independently allocated backing store. Its own mobid, its own pages: nothing it
// finds can be confused with the single-page present arm.
static const UInt32 kFlatSdMobId = 103;

// MOB id of the DX context state block.
static const UInt32 kCtxMobId = 3;

// MOB id of the DX render-target-view COTABLE (one 4 KiB page, PT64_0).
static const UInt32 kCotableMobId = 5;

// SVGA3D_INVALID_ID as seen on the wire: an "unbind this COTable" request is
// expressed as mobid = SVGA3D_INVALID_ID together with validSizeInBytes = 0
// (exactly what vmwgfx's vmw_cotable_scrub() does).
static const UInt32 SVGA3D_INVALID_ID_U32 = 0xFFFFFFFFu;

// ---- Command buffer (CB) submission --------------------------------------
// On vGPU10 hosts the DX command set is accepted but NOT executed when
// submitted through the legacy FIFO; vmwgfx submits every DX command through
// the command-buffer mechanism (SVGA_REG_COMMAND_LOW/HIGH). The CB is a
// 64-byte SVGACBHeader describing a command payload, submitted by writing
// its physical address into the COMMAND registers.
static const UInt32 kCbDataBytes = 64 * 1024;

// Periodic full-screen re-present interval, in milliseconds. The GMRFB points straight at
// the framebuffer in BAR1, so each present is a host-side DMA with no guest copy; the cost
// is bounded and dirty-tracking can replace this later.
static const UInt32 kPresentIntervalMs = 33;

//
// kEnableRpcHeartbeat -- §38.19. Keep the host's Tools heartbeat alive from the kext.
//
// Why this exists: the host expires its heartbeat timer 20-25 s after the last GuestRpc
// it saw, and on expiry it declares the guest display dead, destroys the legacy
// RegisterMode screen and falls back to a 1024x512 screen target. Our kext loads *after*
// that switch (measured: switch at 04:04:35.9, kext start at 04:04:37.3), so we inherit a
// dead target and the screen stays black no matter how correct our 3D commands are.
//
// The heartbeat is one zero-length BDOOR_CMD_MESSAGE per kRpcHeartbeatTicks present ticks
// (~5 s at 33 ms/tick, against a 20 s host timeout -- 4x margin for a stalled timer).
//
// Two things this does NOT do: it does not make the host switch back to RegisterMode
// (that is measured, see §38.19), and it does not install any Tools component. It only
// removes the host's reason to give up on us in the first place -- so it must be running
// *before* the first timeout, which means the real fix is load timing (see §38.19).
static const UInt32 kEnableRpcHeartbeat  = 1;
// Absolute-time cadence, not a tick count. Measured: 150 ticks looked like "5 s" on paper but
// the heartbeat log showed #1 at 04:14:25 and #2 at 04:15:00 -- 35 s apart, because
// presentTick does not actually fire every 33 ms on this host (the present path itself costs
// time). The host expires at 20 s, so a tick-derived interval blew straight through it.
// 2 s against a 20 s timeout is a 10x margin and still costs nothing.
static const UInt64 kRpcHeartbeatNs      = 2ULL * 1000000000ULL;

// One 4 KiB page of PPN64 entries describes up to 512 pages = 2 MiB.
static const IOByteCount kGbPageBytes        = 4096;
static const UInt32     kGbPageShift         = 12;
static const UInt32     kGbDataPages         = 4;    // 16 KiB data MOB -> needs PT64_1
static const UInt32     kGbMobTableEntries   = 1024; // 16 KiB
static const UInt32     kGbSurfaceTableEnts  = 64;   // 64 * 72 = 4608 bytes

// Sizes derived from the SVGAOTable*Entry structures (both `#pragma pack(push, 1)`).
//
// §45: kGbSurfaceEntryBytes used to say 72 in this comment while the value said 64, and the
// 72 was right -- SVGAOTableSurfaceEntry is
//     format(4) surface1Flags(4) numMipLevels(4) multisampleCount(4) autogenFilter(4)
//     size{width,height,depth}(12) mobid(4) arraySize(4) mobPitch(4) surface2Flags(4)
//     multisamplePattern(1) qualityLevel(1) bufferByteStride(2) minLOD(4) pad0[2](8)
//   = 64
// The struct really is 64 bytes; what was wrong was how it was being FILLED (see
// surfaceEntryFill), not its size. Keep the size, fix the fill.
static const UInt32 kGbMobEntryBytes         = 16;   // {ptDepth(4) sizeInBytes(4) base(8)}
static const UInt32 kGbSurfaceEntryBytes     = 64;   // SVGAOTableSurfaceEntry, packed

// Offsets inside the single contiguous working buffer the kext allocates.
static const IOByteCount kGbOffPtOtable      = 0x0000;
static const IOByteCount kGbOffPtSurface     = 0x1000;
static const IOByteCount kGbOffPtData        = 0x2000;
static const IOByteCount kGbOffMobTable      = 0x3000;
static const IOByteCount kGbOffSurfaceTable  = 0x7000;
static const IOByteCount kGbOffData          = 0x8000;
static const IOByteCount kGbOffPtDxCtx       = 0xC000;
static const IOByteCount kGbOffDxCtxTable    = 0xD000;
static const IOByteCount kGbOffPtSt          = 0xE000;
static const IOByteCount kGbOffStTable       = 0xF000;
static const IOByteCount kGbWorkingBytes     = 0x10000;

// DX context object table: SVGAOTableDXContextEntry {uint32 cid; SVGAMobId mobid;} = 8 bytes.
static const UInt32     kGbDxCtxTableEnts    = 256;  // 2 KiB at 8B/entry (host otables[5].count)
static const UInt32     kGbDxCtxEntryBytes   = 8;   // {u32 cid; u32 mobid}; host keeps count = size/8

//
// Screen target (STDU) present path. The screen target's content is a GB surface of the
// full display size; its data lives in a dedicated 3 MB MOB (PT64_1: one root PT page
// pointing at two second-level PT pages covering 768 pages). The present loop copies the
// framebuffer (VRAM) into that memory and issues UPDATE_GB_SCREENTARGET -- the host reads
// the surface data straight out of guest RAM.
//
static const UInt32     kStduWidth           = kStduMatchHost ? 640 : 1024;
// The inherited display is 1024x768, but the STDU surface height is kStduSurfHeight
// (declared near the top of the file): 512 under kStduPt1 so 512 pages fit one PT level.
// kStduHeight stays as the *framebuffer* height for reference only -- do NOT use it to
// size or describe the surface; that was the §38d bug (surface rect 768 rows on a
// 512-row surface, and a 3 MiB bzero into a 2 MiB allocation).
static const UInt32     kStduHeight          = 768;
// §38d: everything in the STDU path is sized by kStduSurfHeight (512 when kStduPt1 == 1,
// 768 otherwise), which is declared up top so the page count can depend on it. These two
// used to be derived from kStduHeight instead -- so with kStduPt1 == 1 kStduPages said 768
// while the MOB and its page table only ever held 512, and stduReadbackProbe() bzero'd
// 3 MiB into a 2 MiB allocation (a heap overwrite past the scratch buffer).
static const IOByteCount kStduBytes          = (IOByteCount)kStduWidth * kStduSurfHeight * 4;
static const UInt32     kStduPages           = (UInt32)(kStduBytes / kGbPageBytes); // kStduSurfPages
static const UInt32     kStduL1Pages         = (kStduPages + 511) / 512;            // 2
// §47: there were two widths all along -- kStduSurfWidth up top and kStduWidth here --
// and they only agreed because both were the literal 1024. kStduSurfPages is derived from
// one and kStduBytes from the other, so changing just one would size the MOB and its page
// table differently, which is the §38d bug in a new costume. Make the compiler hold them
// together instead of a comment asking for it.
static_assert(kStduWidth == kStduSurfWidth, "kStduWidth and kStduSurfWidth must agree");
static_assert(kStduSurfPages == kStduPages, "surface page count disagrees with MOB size");
static const UInt32     kGbStEntryBytes      = 64;   // SVGAOTableScreenTargetEntry
static const UInt32     kGbStTableEnts       = 64;   // 64 * 64 = 4096 bytes

// SVGA3dHardwareVersion, from svga3d_devcaps.h:
//   SVGA3D_MAKE_HWVERSION(major, minor) = (major << 16) | minor
//   SVGA3D_HWVERSION_CURRENT = SVGA3D_HWVERSION_WS8_B1 = (2, 1)
// The handshake is guest-first: the guest announces its version in
// SVGA_FIFO_GUEST_3D_HWVERSION, and only then does the host publish its own (clamped)
// version in SVGA_FIFO_3D_HWVERSION and the 3D capability block.
static const UInt32 kSvga3dHwVersionCurrent = 0x00020001;

static const UInt16 kSvgaIndexPort = 0x00;   // offset within BAR0
static const UInt16 kSvgaValuePort = 0x01;

// x86 port I/O. A kext may issue these directly once the provider has enabled the I/O
// aperture in its command register, which firmware and the NDRV driver already have
// (measured: pci command 0x7).
static inline UInt32 vgpuIn32(UInt16 port) {
    UInt32 value;
    __asm__ volatile ("inl %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline void vgpuOut32(UInt16 port, UInt32 value) {
    __asm__ volatile ("outl %0, %1" : : "a" (value), "Nd" (port));
}

//
// GuestRpc backdoor -- §38.19.
//
// This VM has never had VMware Tools installed, and that single fact is what has been
// keeping the screen black. Here is the chain, read straight out of vmware.log:
//
//   20:04:09.773  Tools: Adding Tools inactivity timer.
//   20:04:10.190  Tools: Running status rpc handler: 0 => 1      <- firmware sends one RPC
//   20:04:10.190  Tools: Removing Tools inactivity timer.
//   20:04:35.136  Tools: Tools heartbeat timeout.                <- 25 s with no RPC
//   20:04:35.136  Tools: Running status rpc handler: 1 => 0
//   20:04:35.756  Guest display topology changed: numDisplays 0
//   20:04:35.902  Screen type changed to ScreenTarget            <- host gives up on legacy
//
// The host treats "no Tools heartbeat" as "this guest has no live display" and tears
// down the legacy RegisterMode screen, falling back to a 1024x512 screen target of its
// own making. Everything we then draw lands on an abandoned target and never reaches
// the encoder -- which is exactly the black screen §37/§38 spent so long chasing.
//
// The fix does not need VMware Tools at all. `Tools: Running status` is driven by
// GuestRpc, and GuestRpc is a plain I/O-port hypercall: the host only needs to *see*
// RPC traffic from the guest on a regular cadence to keep the display alive. We already
// own the I/O aperture for the SVGA registers, so we can send that traffic ourselves.
//
// The low-bandwidth backdoor ABI (backdoor_def.h / asm/vmware.h):
//   eax = BDOOR_MAGIC 0x564D5868 ("VMXh")
//   ecx = command      (BDOOR_CMD_MESSAGE = 30)
//   ebx = arg1         (RPCI message length -- 0 for a bare heartbeat)
//   edx = port         0x5658
//   `inl (%dx), %eax` performs the call; eax comes back with the status.
//
// A zero-length message is the cheapest possible "I am alive" and is what we want: we
// are not trying to talk RPCI properly (that needs a shared page and a real message
// protocol), only to keep the host's heartbeat timer from expiring.
//
static const UInt32 kBdoorMagic  = 0x564D5868;   // "VMXh"
static const UInt16 kBdoorPort   = 0x5658;
static const UInt32 kBdoorCmdMsg = 30;           // BDOOR_CMD_MESSAGE

// Returns the host's status word (eax). A non-zero value means the host accepted the call;
// on a non-VMware host the `inl` would fault, so callers gate this on kEnableRpcHeartbeat.
static inline UInt32 vmwareBackdoor(UInt32 cmd, UInt32 arg1, UInt32 arg2) {
    UInt32 eax = kBdoorMagic;
    UInt32 ebx = arg1;
    UInt32 ecx = cmd;
    UInt32 edx = (UInt32)kBdoorPort;
    __asm__ volatile ("inl %%dx, %%eax"
                      : "+a" (eax), "+b" (ebx), "+c" (ecx), "+d" (edx)
                      :
                      : "memory");
    return eax;
}

static UInt32 svgaReadRegister(UInt16 portBase, UInt32 index) {
    vgpuOut32((UInt16)(portBase + kSvgaIndexPort), index);
    return vgpuIn32((UInt16)(portBase + kSvgaValuePort));
}

static void svgaWriteRegister(UInt16 portBase, UInt32 index, UInt32 value) {
    vgpuOut32((UInt16)(portBase + kSvgaIndexPort), index);
    vgpuOut32((UInt16)(portBase + kSvgaValuePort), value);
}

//
// Tell the host that the FIFO advanced.
//
// This is the single most-missed step in the whole submission path, and it is the reason
// the fence test appeared to "hang": we moved NEXT_CMD and then sat in a spin loop, but the
// host was never told to look. vmw_local_fifo_commit() ends with
//
//     vmw_fifo_ping_host(dev_priv, SVGA_SYNC_GENERIC);      // SVGA_REG_SYNC <- 1
//
// and the host is otherwise free to wait for exactly that doorbell. 0x15 is one of the eight
// registers vm3dmp.sys actually writes (tools/rev/reg-indices.py), so the index is not a
// guess. Value 1 = "flush", value 2 = "flush and wait for BUSY to clear"; we use 1 and do our
// own polling with our own timeout, because value 2 blocks with the host's timeout, not ours.
//
static void svgaPingHost(UInt16 portBase) {
    svgaWriteRegister(portBase, kSvgaRegSync, 1);
}

//
// =====================================================================
// §45: the two object-table / command payloads that were being mis-filled.
//
// Everything here is transcribed field by field from
//   drivers/gpu/drm/vmwgfx/device_include/svga3d_cmd.h
//   drivers/gpu/drm/vmwgfx/device_include/svga3d_types.h
// at the protocol generation this host speaks. The previous hand-rolled offsets assumed
// `surfaceFlags` was 64 bits wide, which shifted EVERY following field by 4 bytes.
//
// The consequence was not subtle, it was just invisible: the host read
//   numMipLevels  = 0            (mips are inherited from the next field)
//   size.width    = 0
//   size.height   = 1
//   mobid         = 2 in the width slot ... i.e. the surface described nothing
// yet DEFINE itself was "accepted", because the command body's size was also wrong
// (40 instead of the authoritative 36), so the host's parser walked 4 bytes into the
// NEXT command. Accepting a command is not the same as understanding it.
//
// Why this hid for so long: the DX arm of this driver had ALREADY been fixed (it writes
// sflags32 at +4 and 36-byte bodies -- see dxReadbackTest), and the DX DMA round trip
// demonstrably works on this host. Only the STDU/display arm still had the 64-bit layout.
// That A/B is the strongest possible evidence that the 32-bit layout is the correct one.
// =====================================================================

// SVGA3dSurface1Flags is `typedef uint32` (svga3d_types.h:392). The 64-bit type is
// SVGA3dSurfaceAllFlags and is only used by the _v3/_v4 DEFINE variants.
static const UInt32 kSvgaSurfaceFlagsMask32 = 0xFFFFFFFFu;

// Fill one SVGAOTableSurfaceEntry (64 bytes, #pragma pack(1)):
//    +0  SVGA3dSurfaceFormat   format              4
//    +4  SVGA3dSurface1Flags   surface1Flags       4    <-- 32-bit, NOT 64
//    +8  uint32                numMipLevels        4
//   +12  uint32                multisampleCount    4
//   +16  SVGA3dTextureFilter   autogenFilter       4
//   +20  SVGA3dSize            size{w,h,d}        12
//   +32  SVGAMobId             mobid               4
//   +36  uint32                arraySize           4
//   +40  uint32                mobPitch            4
//   +44  SVGA3dSurface2Flags   surface2Flags       4
//   +48  uint8                 multisamplePattern  1
//   +49  uint8                 qualityLevel        1
//   +50  uint16                bufferByteStride    2
//   +52  float                 minLOD              4
//   +56  uint32                pad0[2]             8
static void surfaceEntryFill(volatile UInt8 *entry, UInt32 format, UInt64 flags64,
                             UInt32 width, UInt32 height, UInt32 mobId, UInt32 pitch,
                             UInt32 surface2Flags) {
    const UInt32 flags1 = (UInt32)(flags64 & kSvgaSurfaceFlagsMask32);
    const UInt32 surface2 = surface2Flags | (UInt32)((flags64 >> 32) & kSvgaSurfaceFlagsMask32);
    UInt32 one = 1, filter = 0, depth = 1;
    UInt8  msPattern = 0, quality = 0;
    UInt16 byteStride = 0;
    float  minLOD = 0.0f;

    bzero((void *)entry, kGbSurfaceEntryBytes);
    memcpy((void *)(entry +  0), &format,     4);
    memcpy((void *)(entry +  4), &flags1,     4);   // <-- the field that used to be 8 bytes
    memcpy((void *)(entry +  8), &one,        4);   // numMipLevels
    memcpy((void *)(entry + 12), &one,        4);   // multisampleCount
    memcpy((void *)(entry + 16), &filter,     4);   // autogenFilter
    memcpy((void *)(entry + 20), &width,      4);
    memcpy((void *)(entry + 24), &height,     4);
    memcpy((void *)(entry + 28), &depth,      4);
    memcpy((void *)(entry + 32), &mobId,      4);
    memcpy((void *)(entry + 36), &one,        4);   // arraySize
    memcpy((void *)(entry + 40), &pitch,      4);
    memcpy((void *)(entry + 44), &surface2,   4);
    memcpy((void *)(entry + 48), &msPattern,  1);
    memcpy((void *)(entry + 49), &quality,    1);
    memcpy((void *)(entry + 50), &byteStride, 2);
    memcpy((void *)(entry + 52), &minLOD,     4);
    __asm__ volatile ("" ::: "memory");
}

// SVGA3dCmdDefineGBSurface (V1), body size 36 bytes = 9 dwords:
//    +0  uint32                   sid                4
//    +4  SVGA3dSurface1Flags      surfaceFlags       4    <-- 32-bit in V1
//    +8  SVGA3dSurfaceFormat      format             4
//   +12  uint32                   numMipLevels       4
//   +16  uint32                   multisampleCount   4
//   +20  SVGA3dTextureFilter      autogenFilter      4
//   +24  SVGA3dSize               size{w,h,d}       12
// Writes 10 dwords: {cmd, size, sid, flags, format, mips, ms, filter, w, h, d} is 11 --
// see below; the helper writes the full framed command.
static const UInt32 kSvgaDefineGbSurfaceBodyBytes = 36;

static void defineGbSurfaceFrame(UInt32 *out, UInt32 sid, UInt64 flags64, UInt32 format,
                                 UInt32 width, UInt32 height) {
    const UInt32 flags1 = (UInt32)(flags64 & kSvgaSurfaceFlagsMask32);
    out[0]  = kSvga3dCmdDefineGbSurface;
    out[1]  = kSvgaDefineGbSurfaceBodyBytes;   // 36, NOT 40: the body has no depth field
    out[2]  = sid;
    out[3]  = flags1;
    out[4]  = format;
    out[5]  = 1;                                // numMipLevels
    out[6]  = 1;                                // multisampleCount
    out[7]  = 0;                                // autogenFilter = SVGA3D_TEX_FILTER_NONE
    out[8]  = width;
    out[9]  = height;
    out[10] = 1;                                // size.depth -- inside SVGA3dSize, still legal
}
static const UInt32 kSvgaDefineGbSurfaceWords = 11;

class vgpuFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(vgpuFramebuffer)
    typedef IOFramebuffer super;

private:
    static const UInt32 kDepth = 32;

    // The device's own geometry, read out of the SVGA registers in start(). The aperture and
    // every mode answer derive from these rather than from a table, because the device has
    // already been programmed and is scanning out this exact region.
    bool     _deviceModeValid = false;
    UInt32   _deviceWidth     = 0;
    UInt32   _deviceHeight    = 0;
    UInt32   _devicePitch     = 0;
    UInt32   _deviceFbOffset  = 0;

    IOPhysicalAddress         _apertureBase   = 0;
    IOByteCount               _apertureLength = 0;
    IODeviceMemory           *_aperture       = nullptr;

    IODisplayModeID           _currentMode = 1;
    UInt16                    _svgaPortBase = 0;

    bool readDeviceMode(IOPCIDevice *pci, UInt16 *portOut);
    void probeFifo(IOPCIDevice *pci);
    void fifoDump(volatile UInt32 *fifo, const char *when);
    UInt32 devcapRead(UInt32 index);
    void fenceTest(volatile UInt32 *fifo);

    bool fifoSubmitWords(volatile UInt32 *fifo, const UInt32 *words, UInt32 wordCount);
    bool fenceAck(volatile UInt32 *fifo, UInt32 *seq);
    void gbBringUp(volatile UInt32 *fifo);
    void stduBringUp(volatile UInt32 *fifo);
    void stduFlatBringUp(volatile UInt32 *fifo);   // §38.12
    void stduFlatReadback(volatile UInt32 *fifo);  // §38.13
    void stduScreenDma(volatile UInt32 *fifo);     // §38.14
    bool dxVm3dmpTemplate(volatile UInt32 *fifo, UInt32 surfId, UInt32 dxW, UInt32 dxH);
    bool dxTemplateSetupSurface(volatile UInt32 *fifo, UInt32 surfId, UInt32 mobId, UInt32 dxW, UInt32 dxH);
    void dxReadbackTest(volatile UInt32 *fifo);
    void stduReadbackProbe(void);

    void presentSetup(volatile UInt32 *fifo);
    void presentTick(void);
    static void presentTickC(thread_call_param_t param0, thread_call_param_t param1);
    void schedulePresent(void);
    thread_call_t _presentCall = nullptr;
    UInt32 _presentTicks = 0;
    // §38.19 GuestRpc heartbeat: its own thread_call, independent of the present timer.
    bool         startRpcHeartbeat(void);
    static void  rpcTickC(thread_call_param_t param0, thread_call_param_t param1);
    thread_call_t _rpcCall = nullptr;
    UInt32 _rpcSent = 0;
    UInt64 _rpcLastSentAbs = 0;     // kept for diagnostics only
    // Delayed DX test: runs on its own thread_call N seconds after bring-up so the
    // guest is fully booted (SSH + logd flushing) before anything can wedge the MKS.
    static void dxTestC(thread_call_param_t param0, thread_call_param_t param1);
    static void probeC(thread_call_param_t param0, thread_call_param_t param1);
    static void probeC2(thread_call_param_t param0, thread_call_param_t param1);
    void probeRun(void);
    void armProbe(thread_call_t call);
    volatile UInt32 _dxBusy = 0;          // presentTick skips while the DX test owns the FIFO
    thread_call_t _dxCall = nullptr;
    thread_call_t _probeCall = nullptr;   // periodic probe (ping-pong pair: a thread
    thread_call_t _probeCall2 = nullptr;  // call cannot re-enter itself)
    volatile UInt32 *_dxFifo = nullptr;
    volatile UInt32 *_presentFifo = nullptr;
    IOMemoryMap *_fifoMap = nullptr;

    // STDU present state.
    bool _stduActive = false;
    UInt8 _paintLogged = 0;         // kStduPaint logs once, on the first painted frame
    UInt8 _vramLogged = 0;          // kStduVramControl logs once, same reason
    IOBufferMemoryDescriptor *_stduMem = nullptr;   // 3 MB surface backing store
    void                    *_stduVirt = nullptr;
    UInt64                   _stduPhys = 0;
    bool                     _stduPrepared = false;  // prepare() succeeded -> must complete()
    IOBufferMemoryDescriptor *_stduPtMem = nullptr; // 16 KiB contiguous: root + 2 L1 PT pages
    void                    *_stduPtVirt = nullptr;
    UInt64                   _stduPtPhys = 0;
    bool                     _stduPtPrepared = false;
    IOMemoryMap             *_vramMap = nullptr;     // kernel mapping of the framebuffer
    void                    *_vramVirt = nullptr;
    UInt32                   _surfaceId = 1;
    UInt32                   _stduMobId = 2;
    UInt32                   _stduStid = 0;
    // §38.12 kStduFlat: the whole surface payload is ONE 4 KiB page, so the MOB is PT64_0
    // and the host dereferences no page table to find it.
    UInt32                   _flatRbMobId = kFlatRbMobId;
    // §38.14: SCREEN_DMA destination -- a real 1024x512 PT64_1 MOB, kept separate from the
    // single-page present arm so nothing depends on the flat surface geometry.
    IOBufferMemoryDescriptor *_sdMem = nullptr;
    void                    *_sdVirt = nullptr;
    bool                     _sdPrepared = false;
    IOBufferMemoryDescriptor *_sdPtMem = nullptr;
    void                    *_sdPtVirt = nullptr;
    bool                     _sdPtPrepared = false;
    UInt32                   _flatSdMobId = kFlatSdMobId;
    // §38.13: the readback target for the flat arm -- its own PT64_0 MOB, so a zero there
    // means the host genuinely wrote nothing, not that we read our own page back.
    IOBufferMemoryDescriptor *_flatRbMem = nullptr;
    void                    *_flatRbVirt = nullptr;
    bool                     _flatRbPrepared = false;
    IOBufferMemoryDescriptor *_flatMem = nullptr;
    void                    *_flatVirt = nullptr;
    bool                     _flatPrepared = false;
    IOBufferMemoryDescriptor *_ctxPage = nullptr;   // DX context state MOB (64 KiB)
    UInt32                   _ctxMobId = 3;
    IOBufferMemoryDescriptor *_dxMem = nullptr;     // DX test surface memory
    void                    *_dxVirt = nullptr;
    IOBufferMemoryDescriptor *_dxPtMem = nullptr;   // context MOB 3 PT page (gbBringUp, must stay alive)
    IOBufferMemoryDescriptor *_surfPtMem = nullptr; // DX test surface PT page (dedicated)
    // Command buffer submission state.
    IOPCIDevice              *_pci = nullptr;          // for register access in cbSubmit
    IOBufferMemoryDescriptor *_cotableMem = nullptr;   // DX COTABLE storage (4 KiB)
    IOBufferMemoryDescriptor *_cotablePtMem = nullptr; // unused (PT64_0 needs no PT page)
    IOBufferMemoryDescriptor *_cbHeaderMem = nullptr;  // 64 B, contiguous
    volatile UInt8           *_cbHeaderVirt = nullptr;
    UInt64                    _cbHeaderPA = 0;
    IOBufferMemoryDescriptor *_cbDataMem = nullptr;    // 64 KiB, contiguous
    volatile UInt8           *_cbDataVirt = nullptr;
    UInt64                    _cbDataPA = 0;
    UInt64                    _cbId = 1;
    IOBufferMemoryDescriptor *_cbMobMem = nullptr;     // CB payload MOB (1 page, PT64_0)
    volatile UInt8           *_cbMobVirt = nullptr;
    bool                      _cbMobReady = false;
    IOBufferMemoryDescriptor *_cbPrependMem = nullptr; // 4 KiB PREPEND buffer (regs 53/54)
    IOBufferMemoryDescriptor *_ctxStateMem = nullptr;  // exact-size DX state MOB (12288 B)
    IOBufferMemoryDescriptor *_ctxStatePtMem = nullptr; // its PT page

    bool cbInit(void);
    bool cbMobSetup(volatile UInt32 *fifo);   // MOB-form CB payload MOB
    bool cbSubmitCtx(const UInt32 *cmds, UInt32 wordCount, UInt32 cbContext, UInt32 dxContext,
                     UInt32 flags = 1);
    bool cbSubmit(const UInt32 *cmds, UInt32 wordCount);
    bool cbSubmitDx(const UInt32 *cmds, UInt32 wordCount, UInt32 dxCtxId);

    // The GB working buffer stays resident for the life of the kext: the host reads the
    // object tables out of it whenever it validates later commands.
    IOBufferMemoryDescriptor *_gbMem  = nullptr;
    IOByteCount              _gbLen   = 0;
    void                    *_gbVirt  = nullptr;
    UInt64                   _gbPhys  = 0;
    UInt32                   _fenceSeq = 0;

public:
    //
    // IOService.
    //
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    //
    // IOFramebuffer.
    //
    IOReturn enableController() APPLE_KEXT_OVERRIDE;
    bool isConsoleDevice() APPLE_KEXT_OVERRIDE;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    const char *getPixelFormats() APPLE_KEXT_OVERRIDE;
    IOItemCount getDisplayModeCount() APPLE_KEXT_OVERRIDE;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *pixelInfo) APPLE_KEXT_OVERRIDE;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
    IOReturn setCursorImage(void *cursorImage) APPLE_KEXT_OVERRIDE;
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) APPLE_KEXT_OVERRIDE;
    void flushCursor() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(vgpuFramebuffer, IOFramebuffer)

//
// Log every BAR the provider exposes, then read the SVGA registers. The BAR dump is what
// showed that BAR1 is the 128 MB VRAM; the register dump is what makes inheriting the mode
// possible at all.
//
bool vgpuFramebuffer::readDeviceMode(IOPCIDevice *pci, UInt16 *portOut) {
    for (UInt32 index = 0; index < 6; index++) {
        IODeviceMemory *bar = pci->getDeviceMemoryWithIndex(index);
        if (bar != nullptr) {
            IOLog(VGPU_FB_TAG ": BAR%u phys 0x%llx length %llu\n",
                  index,
                  (unsigned long long)bar->getPhysicalAddress(),
                  (unsigned long long)bar->getLength());
        }
    }

    IODeviceMemory *io = pci->getDeviceMemoryWithIndex(0);
    if (io == nullptr) {
        IOLog(VGPU_FB_TAG ": no BAR0, cannot reach the SVGA registers\n");
        return false;
    }
    UInt16 portBase = (UInt16)io->getPhysicalAddress();
    *portOut = portBase;
    _svgaPortBase = portBase;

    UInt32 id     = svgaReadRegister(portBase, kSvgaRegId);
    UInt32 enable = svgaReadRegister(portBase, kSvgaRegEnable);
    UInt32 width  = svgaReadRegister(portBase, kSvgaRegWidth);
    UInt32 height = svgaReadRegister(portBase, kSvgaRegHeight);
    UInt32 depth  = svgaReadRegister(portBase, kSvgaRegDepth);
    UInt32 bpp    = svgaReadRegister(portBase, kSvgaRegBitsPerPixel);
    UInt32 pitch  = svgaReadRegister(portBase, kSvgaRegBytesPerLine);
    UInt32 offset = svgaReadRegister(portBase, kSvgaRegFbOffset);

    IOLog(VGPU_FB_TAG ": svga ports 0x%x/0x%x, pci command 0x%x\n",
          (unsigned)(portBase + kSvgaIndexPort), (unsigned)(portBase + kSvgaValuePort),
          pci->configRead16(kIOPCIConfigCommand));
    IOLog(VGPU_FB_TAG ": svga id 0x%x enable %u\n", id, enable);
    IOLog(VGPU_FB_TAG ": svga mode %ux%u depth %u bpp %u pitch %u\n",
          width, height, depth, bpp, pitch);
    IOLog(VGPU_FB_TAG ": svga max %ux%u\n",
          svgaReadRegister(portBase, kSvgaRegMaxWidth),
          svgaReadRegister(portBase, kSvgaRegMaxHeight));
    IOLog(VGPU_FB_TAG ": svga fb offset 0x%x fb size 0x%x\n",
          offset, svgaReadRegister(portBase, kSvgaRegFbSize));
    IOLog(VGPU_FB_TAG ": svga vram 0x%x fb start 0x%x caps 0x%x\n",
          svgaReadRegister(portBase, kSvgaRegVramSize),
          svgaReadRegister(portBase, kSvgaRegFbStart),
          svgaReadRegister(portBase, kSvgaRegCapabilities));

    // Plausibility gate. A device that is disabled, or that reports geometry we cannot
    // believe, is worse to inherit than to refuse: the aperture would be a window onto
    // memory nobody is scanning out.
    if (enable == 0 || width == 0 || height == 0 || pitch < width * (kDepth / 8)) {
        IOLog(VGPU_FB_TAG ": refusing to inherit implausible geometry\n");
        return false;
    }

    _deviceWidth    = width;
    _deviceHeight   = height;
    _devicePitch    = pitch;
    _deviceFbOffset = offset;
    _deviceModeValid = true;
    return true;
}

//
// FIFO bring-up.
//
// The FIFO lives in the device memory region SVGA_REG_MEM_START points at. On this guest it
// is BAR2 (8 MB) and SVGA_REG_MEM_SIZE publishes 256 KB of it to the guest. Nobody on a
// macOS guest ever configures it: VMware's own driver is 2D-only, so MIN/MAX read zero and
// the host publishes no SVGA3D state. Since the 3D capability became reachable (see
// docs/vmware-svga-capabilities.md), the missing step is exactly this one: a guest driver
// partitioning the FIFO between registers and command data and handing it to the host with
// SVGA_REG_CONFIG_DONE.
//
// The sequence, from the SVGA II definitions shared by vmwgfx and Xorg's vmware driver:
//
//   1. SVGA_FIFO_MIN      = 288 regs * 4 bytes. This is what makes the extended register
//                         set -- 3D_HWVERSION at index 7, the 3D capability block at 32,
//                         GUEST_3D_HWVERSION at 288 -- exist at all.
//   2. SVGA_FIFO_MAX      = usable byte size (MEM_SIZE, clamped to what we mapped).
//   3. NEXT_CMD and STOP  = MIN, i.e. an empty command stream.
//   4. SVGA_REG_CONFIG_DONE = 1. The host validates and takes ownership from here.
//
// Everything is logged before and after, because the after-state is the measurement this
// stage exists to take: a non-zero FIFO_3D_HWVERSION is the host's own acknowledgement that
// SVGA3D is live from inside the guest.
//
void vgpuFramebuffer::probeFifo(IOPCIDevice *pci) {
    _pci = pci;
    UInt32 memStart = svgaReadRegister(_svgaPortBase, kSvgaRegMemStart);
    UInt32 memSize  = svgaReadRegister(_svgaPortBase, kSvgaRegMemSize);
    UInt32 memRegs  = svgaReadRegister(_svgaPortBase, kSvgaRegMemRegs);
    IOLog(VGPU_FB_TAG ": svga mem start 0x%x size 0x%x regs %u\n", memStart, memSize, memRegs);

    // ScreenTarget control state, read before the FIFO is touched (§45). These three
    // registers are the only published way to ask the device "is the screen-DMA path you
    // are about to be handed commands for actually switched on", and §45 sent
    // GB_SCREEN_DMA without ever asking. A GB_SCREEN_DMA that returns zeroes is only
    // evidence of a host bug if the feature reports itself PRESENT and unblanked, so the
    // reading has to come first and be logged unconditionally.
    {
        const UInt32 cap2     = svgaReadRegister(_svgaPortBase, kSvgaRegCap2);
        const UInt32 screenDm = svgaReadRegister(_svgaPortBase, kSvgaRegScreenDma);
        const UInt32 blank    = svgaReadRegister(_svgaPortBase, kSvgaRegBlankScreenTargets);
        const UInt32 stMaxW   = svgaReadRegister(_svgaPortBase, kSvgaRegScreenTargetMaxWidth);
        const UInt32 stMaxH   = svgaReadRegister(_svgaPortBase, kSvgaRegScreenTargetMaxHeight);
        const UInt32 mobMax   = svgaReadRegister(_svgaPortBase, kSvgaRegMobMaxSize);
        const UInt32 gbMemKb  = svgaReadRegister(_svgaPortBase, kSvgaRegGbObjectMemSizeKb);
        const char *sdName = (screenDm == kSvgaScreenDmaPresent)    ? "PRESENT"
                           : (screenDm == kSvgaScreenDmaNotPresent) ? "NOT_PRESENT"
                           : (screenDm == kSvgaScreenDmaUndefined)  ? "UNDEFINED"
                           : "out-of-range";
        IOLog(VGPU_FB_TAG ": screendma: CAP2 0x%08x (SCREENDMA_REG %s) SCREENDMA %u (%s) "
              "BLANK_SCREEN_TARGETS %u\n",
              cap2, (cap2 & kSvgaCap2ScreenDmaReg) ? "yes" : "no", screenDm, sdName, blank);
        IOLog(VGPU_FB_TAG ": screendma: limits -> ST max %ux%u, MOB max %u bytes, "
              "GB object mem %u KB\n", stMaxW, stMaxH, mobMax, gbMemKb);
        if (stMaxW != 0 && kStduWidth > stMaxW) {
            IOLog(VGPU_FB_TAG ": screendma: *** our target is %u wide, the host caps at %u ***\n",
                  kStduWidth, stMaxW);
        }
        if (stMaxH != 0 && kStduSurfHeight > stMaxH) {
            IOLog(VGPU_FB_TAG ": screendma: *** our target is %u tall, the host caps at %u ***\n",
                  kStduSurfHeight, stMaxH);
        }
        if (mobMax != 0 && (UInt32)kStduBytes > mobMax) {
            IOLog(VGPU_FB_TAG ": screendma: *** our MOB is %u bytes, the host caps at %u ***\n",
                  (UInt32)kStduBytes, mobMax);
        }
        if (screenDm == kSvgaScreenDmaNotPresent) {
            IOLog(VGPU_FB_TAG ": screendma: *** the host says the DMA path does not exist here; "
                  "GB_SCREEN_DMA returning zeroes says nothing about the surface ***\n");
        }
        if (blank != 0) {
            IOLog(VGPU_FB_TAG ": screendma: *** screen targets are BLANKED; a blanked target "
                  "does not sample its surface ***\n");
        }

        // SVGA_REG_BLANK_SCREEN_TARGETS is write-1-blank / write-0-unblank. Upstream only
        // touches it on suspend/resume, but nothing says a guest that defines its own
        // target may assume the "unblank" default -- and a blanked target explains every
        // measurement we have (commands accepted, DMA returns zeroes, screen black) with
        // no other defect required. Writing 0 is idempotent and reversible, so it is the
        // cheapest possible test of that explanation.
        if (kStduUnblank) {
            svgaWriteRegister(_svgaPortBase, kSvgaRegBlankScreenTargets, 0);
            const UInt32 after = svgaReadRegister(_svgaPortBase, kSvgaRegBlankScreenTargets);
            IOLog(VGPU_FB_TAG ": screendma: unblank written, reads back %u\n", after);
        }
    }
    if (memStart == 0 || memSize == 0) {
        IOLog(VGPU_FB_TAG ": no device memory region, FIFO not reachable\n");
        return;
    }

    IODeviceMemory *region = nullptr;
    for (UInt32 index = 0; index < 6 && region == nullptr; index++) {
        IODeviceMemory *bar = pci->getDeviceMemoryWithIndex(index);
        if (bar != nullptr && bar->getPhysicalAddress() == memStart) {
            region = bar;
        }
    }
    if (region == nullptr) {
        IOLog(VGPU_FB_TAG ": mem region 0x%x is not one of the BARs\n", memStart);
        return;
    }

    IOMemoryMap *map = region->map();
    if (map == nullptr) {
        IOLog(VGPU_FB_TAG ": could not map the FIFO region\n");
        return;
    }
    volatile UInt32 *fifo = (volatile UInt32 *)map->getVirtualAddress();

    fifoDump(fifo, "before");

    // §47: SVGA_REG_ENABLE first, and outside the kFbStage gate.
    //
    // ENABLE is *device* state, not guest state: it survives a guest reboot. One boot with
    // ENABLE_HIDE set therefore leaves the device hidden for every boot after it, including
    // the read-only one -- which is precisely what happened today. A two-boot sequence
    // (hide, then rebuild without it) produced a black screen with no explanation in any
    // guest log, because the write that caused it was two boots in the past.
    //
    // So the write happens on both arms and before every other decision: with the switch on
    // it hides, with it off it un-hides. That also makes kFbStage=0 mean what it says --
    // "no FIFO writes" -- while still being able to undo an earlier experiment.
    if (kStduEnableHide) {
        UInt32 enableBefore = svgaReadRegister(_svgaPortBase, kSvgaRegEnable);
        svgaWriteRegister(_svgaPortBase, kSvgaRegEnable, 3);   // ENABLE | HIDE
        IOLog(VGPU_FB_TAG ": enable: %u -> ENABLE|HIDE before CONFIG_DONE, reads back %u\n",
              enableBefore, svgaReadRegister(_svgaPortBase, kSvgaRegEnable));
    } else {
        UInt32 enableBefore = svgaReadRegister(_svgaPortBase, kSvgaRegEnable);
        svgaWriteRegister(_svgaPortBase, kSvgaRegEnable, 1);   // ENABLE only
        IOLog(VGPU_FB_TAG ": enable: %u -> ENABLE only (un-hide) before CONFIG_DONE, "
              "reads back %u\n",
              enableBefore, svgaReadRegister(_svgaPortBase, kSvgaRegEnable));
    }

    if (kFbStage == 0) {
        // Read-only bisect mode: no FIFO writes at all.
        IOLog(VGPU_FB_TAG ": probeFifo: read-only (kFbStage=0)\n");
        map->release();
        return;
    }
    // MIN is where guest command data starts, and it has to clear every register the host
    // defines -- which on this host is 291 dwords, not the 288 the upstream header implies.
    //
    // This is the bug that explains the whole 3D saga. A probe of the FIFO block (see
    // _dumpFifoHeader in kexts/probe) with this driver unloaded showed the host had written
    // exactly six dwords, one of them at index 288: SVGA_FIFO_3D_HWVERSION = 0x45b, i.e.
    // SVGA3D protocol 1115. With MIN = 1152 (288 dwords) the command area began *exactly on
    // top of that dword*, so the first command we pushed overwrote the host's own 3D version
    // word -- and a host that reads its version word back and finds a command header there
    // has every reason to drop the queue. That is why commands "fenced" in guest memory
    // while nothing was ever consumed, why the FIFO capability dword stayed 0, and why
    // DEFINE_RTV never once succeeded (§35, §39, §43).
    //
    // The reservation is the larger of the host's MEM_REGS and our own table, plus one dword
    // of slack so the last host-defined index can never be the first command byte -- and then
    // rounded up to a page, because that is what the working driver does. vmw_fifo_create()
    // in vmwgfx_cmd.c is explicit about the last step:
    //
    //     min = vmw_read(dev_priv, SVGA_REG_MEM_REGS);   // 291 here
    //     min <<= 2;                                     // 1164 bytes
    //     if (min < PAGE_SIZE) min = PAGE_SIZE;          // -> 4096
    //
    // We had been using 1168. That is legal on paper -- command data may start anywhere past
    // the last register -- but it is 4 KiB-misaligned in exactly the way a host that indexes
    // the ring by page may not tolerate, and it is the last structural difference between our
    // FIFO and the one Linux configures on this exact device. Commands sat in memory with
    // NEXT_CMD advanced, SYNC pinged, and the host still neither consumed them nor published
    // a single capability bit (§44). Aligning costs nothing and removes the difference.
    UInt32 mandatoryRegs = kSvgaFifoExtendedMandatoryRegs;
    if (memRegs != 0 && memRegs < 4096 && memRegs + 1 > mandatoryRegs) {
        mandatoryRegs = memRegs + 1;
    }
    UInt32 minBytes = mandatoryRegs * (UInt32)sizeof (UInt32);
    if (minBytes < 4096) {                      // vmw_fifo_create: "if (min < PAGE_SIZE)"
        minBytes = 4096;
    }
    IOLog(VGPU_FB_TAG ": fifo: reserving %u dwords (MEM_REGS %u) -> MIN %u bytes%s\n",
          mandatoryRegs, memRegs, minBytes,
          (minBytes == mandatoryRegs * 4) ? "" : " (page-aligned)");

    // A non-zero MIN means a FIFO was already configured. On this guest that is always us,
    // on a previous boot -- the device is not reset by a reboot, only by a VM power cycle.
    // (The old check rejected anything that was not exactly our own size, which over the
    // course of these fixes meant a boot could refuse to configure simply because the
    // previous boot used a different MIN. The size is ours to choose; a leftover value is
    // never a reason to skip.)
    if (fifo[kSvgaFifoMin] != 0 && fifo[kSvgaFifoMin] < minBytes) {
        IOLog(VGPU_FB_TAG ": fifo: previous MIN %u is below the %u this build needs, "
              "reconfiguring\n", fifo[kSvgaFifoMin], minBytes);
    } else if (fifo[kSvgaFifoMin] != 0 && fifo[kSvgaFifoMin] != minBytes) {
        IOLog(VGPU_FB_TAG ": fifo: previous MIN %u differs from this build's %u, "
              "reconfiguring\n", fifo[kSvgaFifoMin], minBytes);
    }

    // The stream is NOT necessarily empty. A boot that wrote a command and was then shut down
    // before the host consumed it leaves NEXT_CMD > STOP, which is exactly what the stage-3
    // boot after the stage-2 run showed (next_cmd 1192, stop 1152). Rewriting both to MIN
    // discards that command; that is correct, because it describes a frame belonging to a
    // previous boot, but it is worth a log line -- "the FIFO was left dirty" is information
    // about how the previous session ended.
    if (fifo[kSvgaFifoMin] != 0 && fifo[kSvgaFifoNextCmd] != fifo[kSvgaFifoStop]) {
        IOLog(VGPU_FB_TAG ": FIFO left dirty (next_cmd %u, stop %u) -- discarding\n",
              fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop]);
    }

    {
        UInt32 regBytes  = minBytes;
        UInt32 fifoBytes = (memSize != 0 && memSize <= (UInt32)map->getLength()) ? memSize
                                                                                 : (UInt32)map->getLength();
        if (fifoBytes < regBytes + 10 * 1024) {
            IOLog(VGPU_FB_TAG ": FIFO area %u bytes too small for the extended registers\n", fifoBytes);
            map->release();
            return;
        }

        // Clear the configured flag while the FIFO words are being replaced, then set it once
        // the stream is coherent. This used to write 0x14 -- which is RESET, not CONFIG_DONE --
        // so every stage>=1 boot reset the device here and threw away the host's display state.
        // That is what §38 was actually measuring; see §42 in cb-findings.
        svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 0);
        fifo[kSvgaFifoMin]     = regBytes;
        fifo[kSvgaFifoMax]     = fifoBytes;
        fifo[kSvgaFifoNextCmd] = regBytes;
        fifo[kSvgaFifoStop]    = regBytes;
        // vmw_fifo_create() also zeroes SVGA_FIFO_BUSY before setting CONFIG_DONE. A stale
        // non-zero BUSY would have the host believe a flush is still in flight and never start
        // draining the ring -- which is precisely the symptom we were chasing.
        fifo[kSvgaFifoBusy]    = 0;
        __asm__ volatile ("" ::: "memory");

        // Display topology and the guest driver id are NOT written here any more.
        //
        // Enumerating every index vm3dmp.sys ever writes (tools/rev/reg-indices.py) yields
        // exactly eight, and now that svga_reg.h has been read correctly they all decode:
        //
        //   0x01 ENABLE      0x02 WIDTH       0x14 CONFIG_DONE   0x15 SYNC
        //   0x16 BUSY        0x21 IRQMASK     0x35/0x36 CMD_PREPEND_LOW/HIGH
        //
        // That set is exactly "configure the FIFO, ask for interrupts, register a prepend
        // buffer" -- which is what a Windows miniport does. It contains NO display-topology
        // write (those are 35..40 = 0x23..0x28, where the driver writes nothing) and NO
        // guest driver id write (61..64). Both blocks were inventions of the old, wrong
        // register map and are gone.
        //
        // The 0x35/0x36 entry is the one that has been misread twice: first as display
        // topology, then as the command doorbell. They are SVGA_REG_CMD_PREPEND_LOW/HIGH.
        // The command doorbell is 48/49 and neither this host's driver nor vmwgfx uses it --
        // both submit through the FIFO. Therefore writing 0xFFFFFFFF to 0x35/0x36 as
        // "SVGA_ID_INVALID: deselect" was not corrupting a command queue at all; it was
        // publishing a bogus prepend buffer address. Removed.
        //
        // SVGA_REG_GUEST_DRIVER_ID (61) and VERSION1..3 (62..64) do exist in the device
        // definition, so the old comment claiming they were invented was wrong too. What is
        // true is narrower: this host's driver never writes them, so there is no evidence this
        // host requires them, and they are not part of the FIFO bring-up. Left off the boot
        // path; kGuestDriverId still gates the copy in the command-buffer path.
        // Display topology (registers 34..40) is declared HERE, before CONFIG_DONE, and not
        // in stduBringUp next to the screen-target define (§45).
        //
        // Measured this boot: with the block placed in stduBringUp, DEFINE_GB_SCREENTARGET
        // was refused -- the FIFO aborted at 1124, exactly the failure mode the 2026-10-07
        // matrix recorded for "topology writes while the MKS is processing our FIFO". The
        // registers are host-side display state, not FIFO state; writing them while a live
        // stream is being drained makes the MKS rebuild its output while it is mid-command.
        // Before CONFIG_DONE the host is not yet draining anything, so the same six writes
        // are free -- which is also why M6's first success recorded them as step 1 of the
        // bring-up and not as part of the present path.
        //
        // 35..40 decimal = 0x23..0x28 hex. NOT 0x35/0x36, which is CMD_PREPEND_LOW/HIGH;
        // conflating those two is what deleted this block in 0e80bab.
        // §47: ENABLE is written above, before the kFbStage gate, because it is device
        // state that outlives a reboot. Nothing about CONFIG_DONE is left here but the
        // topology block and the flag itself.
        if (kStduTopology) {
            svgaWriteRegister(_svgaPortBase, 34, 1);           // SVGA_REG_NUM_GUEST_DISPLAYS
            svgaWriteRegister(_svgaPortBase, 35, 0);           // SVGA_REG_DISPLAY_ID (stid 0)
            svgaWriteRegister(_svgaPortBase, 36, 1);           // SVGA_REG_DISPLAY_IS_PRIMARY
            svgaWriteRegister(_svgaPortBase, 37, 0);           // SVGA_REG_DISPLAY_POSITION_X
            svgaWriteRegister(_svgaPortBase, 38, 0);           // SVGA_REG_DISPLAY_POSITION_Y
            svgaWriteRegister(_svgaPortBase, 39, kStduWidth);  // SVGA_REG_DISPLAY_WIDTH
            svgaWriteRegister(_svgaPortBase, 40, kStduSurfHeight); // SVGA_REG_DISPLAY_HEIGHT
            IOLog(VGPU_FB_TAG ": topo: declared stid 0, primary, 0,0 %ux%u before CONFIG_DONE; "
                  "reads back id %u primary %u pos %u,%u size %ux%u\n",
                  kStduWidth, kStduSurfHeight,
                  svgaReadRegister(_svgaPortBase, 35), svgaReadRegister(_svgaPortBase, 36),
                  svgaReadRegister(_svgaPortBase, 37), svgaReadRegister(_svgaPortBase, 38),
                  svgaReadRegister(_svgaPortBase, 39), svgaReadRegister(_svgaPortBase, 40));
        }

        svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 1);
        IOLog(VGPU_FB_TAG ": FIFO configured: regs 0..%u, data %u..%u, CONFIG_DONE=1\n",
              regBytes, regBytes, fifoBytes);
        fifoDump(fifo, "after ");
    }

    // Announce the guest protocol version. On a vGPU10 (GBOBJECTS) device the legacy
    // SVGA_FIFO_3D_HWVERSION register stays zero -- the authoritative 3D check there is
    // the DEV_CAP backdoor below, exactly as Linux's vmwgfx does in vmw_fifo_have_3d().
    //
    // NOTE: the guest-version word is NOT written here any more. A probe run with this kext
    // unloaded (§43.5) found the host had put its own SVGA3D protocol number (0x45b = 1115)
    // at index 288, and this line was overwriting it with 0x20001. Whatever the host does
    // with a version word it wrote and then finds changed, overwriting it is not something
    // a guest that has not negotiated anything should do.
    IOLog(VGPU_FB_TAG ": host 3D protocol at fifo[%u] = 0x%x (read only, not overwritten)\n",
          kSvgaFifo3dHwVersion, fifo[kSvgaFifo3dHwVersion]);

    // The device-capability backdoor: write a SVGA3D_DEVCAP index into SVGA_REG_DEV_CAP,
    // read the value back from the same register. This is the authoritative per-feature
    // table (vGPU10-era), independent of the FIFO capability block.
    fifoDump(fifo, "final ");
    IOLog(VGPU_FB_TAG ": devcaps\n");
    for (UInt32 i = 0; i < 32; i += 8) {
        IOLog(VGPU_FB_TAG ": devcap[%2u..%2u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
              i, i + 7,
              devcapRead(i),     devcapRead(i + 1), devcapRead(i + 2), devcapRead(i + 3),
              devcapRead(i + 4), devcapRead(i + 5), devcapRead(i + 6), devcapRead(i + 7));
    }

    // The verdict, using the same check as vmwgfx's vmw_fifo_have_3d() for a GBOBJECTS
    // device: 3D exists if and only if DEV_CAP[SVGA3D_DEVCAP_3D] reads non-zero.
    UInt32 devcap3d = devcapRead(0);   // SVGA3D_DEVCAP_3D
    IOLog(VGPU_FB_TAG ": SVGA3D %s (DEVCAP_3D=%u)\n",
          devcap3d != 0 ? "LIVE" : "absent", devcap3d);

    // End-to-end proof of the command stream: submit SVGA_CMD_FENCE and wait for the host
    // to acknowledge it by writing the value back into SVGA_FIFO_FENCE.
    if (kFbStage >= 3) {
        fenceTest(fifo);

        // Presentation probe: after CONFIG_DONE the host no longer auto-traces framebuffer
        // changes, so nothing new is presented (the boot screen freezes). SVGA_CMD_UPDATE is
        // the legacy "this rect of the framebuffer changed, scan it out again" notification.
        // Submitting one full-screen UPDATE is the minimal fix candidate.
        if (kFbStage >= 4 && _deviceModeValid) {
            UInt32 upd[5] = { kSvgaCmdUpdate, 0, 0, _deviceWidth, _deviceHeight };
            if (fifoSubmitWords(fifo, upd, 5) && fenceAck(fifo, &_fenceSeq)) {
                IOLog(VGPU_FB_TAG ": full-screen UPDATE submitted and acked\n");
            } else {
                IOLog(VGPU_FB_TAG ": full-screen UPDATE not acked\n");
            }
        }
    }

    // Guest-backed object infrastructure: object tables, a data MOB, and a GB surface.
    gbBringUp(fifo);

    if (kDxTest) {
        // Run the DX test from a delayed thread_call (kDxTestDelayS after bring-up):
        // by then the guest is fully up (SSH reachable, logd persisting) so the test
        // results survive even if the MKS wedges. presentTick skips while we run.
        _dxFifo = fifo;
        _dxCall = thread_call_allocate(&vgpuFramebuffer::dxTestC, (thread_call_param_t)this);
        _probeCall = thread_call_allocate(&vgpuFramebuffer::probeC, (thread_call_param_t)this);
        _probeCall2 = thread_call_allocate(&vgpuFramebuffer::probeC2, (thread_call_param_t)this);
        if (_dxCall != nullptr) {
            UInt64 now = 0, delta = 0;
            clock_get_uptime(&now);
            nanoseconds_to_absolutetime((UInt64)kDxTestDelayS * 1000000000ULL, &delta);
            thread_call_enter_delayed(_dxCall, now + delta);
            IOLog(VGPU_FB_TAG ": dx: test scheduled in %u s\n", kDxTestDelayS);
        } else {
            IOLog(VGPU_FB_TAG ": dx: could not allocate dx thread_call\n");
        }
    }

    // The present path: STDU (GB screen targets) when the GB stack is up, else the legacy
    // screen-object blits. Both arm the periodic re-present timer.
    if (kGbStage >= 5 && kFbStage >= 2) {
        map->retain();
        _fifoMap = map;
        presentSetup(fifo);
    } else if (kFbStage >= 5) {
        map->retain();
        _fifoMap = map;
        presentSetup(fifo);
    }

    map->release();
}

//
// Screen-object present path.
//
// Once the guest owns the FIFO (CONFIG_DONE), the MKS stops auto-presenting the framebuffer
// and expects the driver to drive screen objects. Three commands set that up:
//
//   DEFINE_SCREEN         -- screen 0, primary, the inherited mode, at (0,0);
//   DEFINE_GMRFB          -- the GMRFB is the blit *source*: it points straight at the
//                            framebuffer in BAR1 via SVGA_GMR_FRAMEBUFFER, so presents need
//                            no guest-side copy at all;
//   BLIT_GMRFB_TO_SCREEN  -- one full-screen blit from the GMRFB to screen 0.
//
// A periodic timer re-blits the whole screen at kPresentIntervalMs. It is a pure host-side
// DMA (the GMRFB lives in BAR1), so the guest cost is just the FIFO dwords; dirty tracking
// can shrink it later. Single-producer (the timer) after start() completes, so no lock is
// needed beyond the reserve protocol itself.
//
// ---------------------------------------------------------------------------
// stduReadbackProbe -- §36.8 step 1: does the host agree with us about what
// surface 2 / mobid 2 contain?
//
// Runs exactly once, on present tick 4, so the pattern painted on tick 1..3 has
// been DMA'd to the host several times before we ask for it back. The read-back
// goes into its own scratch allocation; reading into _stduVirt would be
// self-confirming, because a DMA the host silently dropped would leave our own
// pattern sitting there and look like a pass.
//
// Verdicts, both logged with pixel evidence:
//   MATCH    -> the DMA target is our surface; look at the screen-target path.
//   MISMATCH -> surface 2 / mobid 2 on the host is not what we think it is.
// ---------------------------------------------------------------------------
void vgpuFramebuffer::stduReadbackProbe(void) {
    if (_presentFifo == nullptr) {
        IOLog(VGPU_FB_TAG ": rb: no FIFO\n");
        return;
    }
    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kStduBytes, 0xFFFULL);
    if (mem == nullptr) {
        IOLog(VGPU_FB_TAG ": rb: allocation failed\n");
        return;
    }
    if (mem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog(VGPU_FB_TAG ": rb: prepare failed\n");
        mem->release();
        return;
    }
    UInt8 *virt = (UInt8 *)mem->getBytesNoCopy();
    bzero(virt, kStduBytes);        // any surviving zero = the host never wrote here

    // Physical pages of the scratch buffer become its own MOB, so the read-back
    // has a place to land that is unambiguously not the source MOB.
    //
    // Page tables get their own allocation: 768 pages need a 6 KiB L1 table, and the
    // GB working buffer only has 4 KiB pages to spare next to the tables already in
    // use. ROOT = 1 page, L1 = 2 pages (768 * 8 = 6144 B).
    //
    // §38: the two L1 pages are NOT one contiguous run. The device walks them through
    // root[0] and root[1], so entry[i] lives at L1page[i / 512][i % 512]. This probe had
    // the same flat-layout bug as the STDU table and would have measured a bogus MOB.
    const UInt32 rbEntsPerPage = (UInt32)(kGbPageBytes / 8);   // 512
    // §38b: the scratch MOB follows the same depth switch as the source MOB, so a
    // one-level (PT64_1) run keeps ROOT itself as the leaf table and 512 pages fit.
    const UInt32 rbPtPages = (kStduPt1 == 1)
                             ? 1
                             : ((kStduPages + rbEntsPerPage - 1) / rbEntsPerPage + 1);
    const UInt32 rbMobId = _stduMobId + 100;
    IOBufferMemoryDescriptor *ptMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 3 * kGbPageBytes, 0xFFFULL);
    if (ptMem == nullptr) {
        IOLog(VGPU_FB_TAG ": rb: PT allocation failed\n");
        mem->complete(kIODirectionInOut);
        mem->release();
        return;
    }
    volatile UInt8 *ptVirt = (volatile UInt8 *)ptMem->map()->getVirtualAddress();
    bzero((void *)ptVirt, 3 * kGbPageBytes);
    UInt64 ptPpn[3] = { 0, 0, 0 };
    bool ptOk = true;
    for (UInt32 p = 0; p < 3; p++) {
        IOByteCount len = 0;
        IOVirtualAddress addr = ptMem->getPhysicalSegment((IOByteCount)p * kGbPageBytes, &len);
        if (addr == 0 || len < kGbPageBytes) {
            ptOk = false;
            break;
        }
        ptPpn[p] = (UInt64)addr >> kGbPageShift;
    }
    if (ptOk) {
        volatile UInt64 *root = (volatile UInt64 *)ptVirt;
        root[0] = ptPpn[1];
        root[1] = ptPpn[2];
        for (UInt32 i = 0; i < kStduPages; i++) {
            IOByteCount len = 0;
            IOVirtualAddress addr = mem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &len);
            if (addr == 0 || len < kGbPageBytes) {
                ptOk = false;
                break;
            }
            const UInt32 lvl = i / rbEntsPerPage;
            const UInt32 idx = i % rbEntsPerPage;
            volatile UInt64 *l1 = (volatile UInt64 *)(ptVirt +
                                                    (IOByteCount)(lvl + 1) * kGbPageBytes);
            l1[idx] = (UInt64)addr >> kGbPageShift;
        }
    }
    __asm__ volatile ("" ::: "memory");
    if (!ptOk) {
        IOLog(VGPU_FB_TAG ": rb: page-table pages not resolvable\n");
        ptMem->release();
        mem->complete(kIODirectionInOut);
        mem->release();
        return;
    }
    const UInt64 rbRootPpn = ptPpn[0];

    // MOB otable entry must be written BEFORE the DEFINE: the host snapshots it.
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    const UInt32 rbEntryOff = rbMobId * kGbMobEntryBytes;
    const UInt32 mobDepth = (kStduPt1 == 1) ? kSvga3dMobFmtPt64_1
                                          : kSvga3dMobFmtPt64_2;
    const UInt32 rbBytes = (UInt32)((IOByteCount)kStduWidth * kStduSurfHeight * 4);
    memcpy((void *)(mobTable + rbEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + rbEntryOff + 4), &rbBytes, 4);
    memcpy((void *)(mobTable + rbEntryOff + 8), &rbRootPpn, 8);
    __asm__ volatile ("" ::: "memory");

    UInt32 def[7];
    def[0] = kSvga3dCmdDefineGbMob64;
    def[1] = 20;
    def[2] = rbMobId;
    def[3] = mobDepth;
    UInt64 rootCopy = rbRootPpn;
    memcpy(&def[4], &rootCopy, 8);
    def[6] = rbBytes;
    fifoSubmitWords(_presentFifo, def, 7);
    if (!fenceAck(_presentFifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": rb: DEFINE_GB_MOB64 for mob %u refused\n", rbMobId);
        mem->complete(kIODirectionInOut);
        mem->release();
        ptMem->release();
        return;
    }

    // The DMA is bracketed by fences -- fenceAck() submits its own FENCE pair -- so the
    // compare below runs strictly after the host has finished writing our scratch buffer.
    //
    // §38g: the transfer field was 0 here, which is NOT a defined value. svga3d_types.h:
    //
    //     typedef enum { SVGA3D_WRITE_HOST_VRAM = 1,
    //                    SVGA3D_READ_HOST_VRAM  = 2 } SVGA3dTransferType;
    //
    // so §37 ("READBACK into a zeroed buffer left every sampled word zero, therefore
    // SURFACE_DMA is a no-op on this host") was measuring a malformed command, not a
    // missing capability. The host's own verifier keys off exactly this field --
    // vmwgfx_execbuf.c:vmw_cmd_dma does
    //     dirty = (transfer == SVGA3D_WRITE_HOST_VRAM) ? VMW_RES_DIRTY_SET : 0;
    // and the xorg driver does use SURFACE_DMA against screen targets on this same
    // device class. This run answers the question §37 thought it had answered: if the
    // scratch buffer comes back with our pixels, the host resolves mobid 2 to our memory
    // and the fault is purely in the update/present step; if it comes back zero, the MOB
    // mapping really is not landing on our pages.
    UInt32 dma[21];
    dma[0] = 1044;                  // SVGA_3D_CMD_SURFACE_DMA
    dma[1] = 28 + 36 + 12;
    dma[2] = rbMobId;               // guest.ptr.gmrId -- scratch MOB, not the source
    dma[3] = 0;                     // guest.ptr.offset
    dma[4] = kStduWidth * 4;        // guest.pitch
    dma[5] = _surfaceId;            // host.sid = 2, the surface the screen target shows
    dma[6] = 0;                     // host.face
    dma[7] = 0;                     // host.mipmap
    dma[8] = 2;                     // transfer = SVGA3D_READ_HOST_VRAM
    dma[9] = 0;  dma[10] = 0;  dma[11] = 0;
    dma[12] = kStduWidth;  dma[13] = kStduSurfHeight;  dma[14] = 1;
    dma[15] = 0; dma[16] = 0; dma[17] = 0;
    dma[18] = 12;
    dma[19] = (UInt32)((IOByteCount)kStduWidth * kStduSurfHeight * 4);
    dma[20] = 0;
    fifoSubmitWords(_presentFifo, dma, 21);

    if (!fenceAck(_presentFifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": rb: post-fence timed out -- host never finished the readback\n");
        mem->complete(kIODirectionInOut);
        mem->release();
        ptMem->release();
        return;
    }

    // Compare against the pattern the present loop paints, sampled at the same
    // coordinates the paint logger reports: a border pixel and a bottom-half pixel.
    const UInt32 *src = (const UInt32 *)_stduVirt;
    const UInt32 *dst = (const UInt32 *)virt;
    const UInt32 w = kStduWidth, h = kStduSurfHeight;
    const UInt32 probeIdx[4] = {
        0,                                  // red border corner
        (UInt32)(h / 4) * w + w / 2,        // green top half
        (UInt32)(h / 2 + h / 4) * w + w / 2, // blue bottom half
        (UInt32)(h - 1) * w + (w - 1),      // red border far corner
    };
    UInt32 same = 0;
    for (UInt32 i = 0; i < 4; i++) {
        if (dst[probeIdx[i]] == src[probeIdx[i]]) {
            same++;
        }
        IOLog(VGPU_FB_TAG ": rb: px[%u] src 0x%08x rb 0x%08x %s\n", i,
              src[probeIdx[i]], dst[probeIdx[i]],
              dst[probeIdx[i]] == src[probeIdx[i]] ? "same" : "DIFF");
    }
    // Also count how many of the sampled pixels the host wrote at all: an all-zero
    // scratch buffer means the read-back never touched memory, which is a different
    // failure from "wrote the wrong thing".
    UInt32 nonZero = 0;
    for (UInt32 y = 0; y < h; y += 32) {
        const UInt32 *row = dst + (IOByteCount)y * w;
        for (UInt32 x = 0; x < w; x += 32) {
            if (row[x] != 0) {
                nonZero++;
            }
        }
    }
    IOLog(VGPU_FB_TAG ": rb: VERDICT %s (4/4 sampled px identical, %u/768 nonzero)\n",
          (same == 4) ? "*** MATCH: DMA target is our surface ***"
                      : "*** MISMATCH: host's surface 2 is not our buffer ***",
          nonZero);

    mem->complete(kIODirectionInOut);
    mem->release();
    ptMem->release();
}

void vgpuFramebuffer::presentSetup(volatile UInt32 *fifo) {
    if (!_deviceModeValid) {
        return;
    }
    _presentFifo = fifo;
    UInt32 words[16];

    if (kGbStage >= 5) {
        // STDU mode: stduBringUp() has already run (inside gbBringUp) and set
        // _stduActive; it defines the screen target and binds the surface. Arm the
        // periodic copy+update loop. (The thread_call must be allocated HERE -- the
        // legacy branch below never runs in this mode, and without the allocation
        // schedulePresent() silently no-ops: no ticks, black display.)
        if (_stduActive) {
            _presentCall = thread_call_allocate(&vgpuFramebuffer::presentTickC,
                                                (thread_call_param_t)this);
            if (_presentCall == nullptr) {
                IOLog(VGPU_FB_TAG ": present: could not allocate the present thread_call\n");
                return;
            }
            schedulePresent();
            IOLog(VGPU_FB_TAG ": present: STDU re-present started (%u ms)\n",
                  kPresentIntervalMs);
        }
        return;
    }

    // §47: fill the framebuffer with magenta ONCE, before the first blit.
    //
    // This is a draw detector, not decoration. It runs at kext start, before WindowServer
    // has drawn anything, so by the time a capture is taken one of three things is true:
    // the desktop is on screen (the blit works and the window server draws), magenta is on
    // screen (the blit works and nothing redraws -- so the framebuffer driver is not where
    // WindowServer draws), or the screen is black (the blit does not work at all). Without
    // it, "black" cannot be told apart from "the guest put nothing there".
    if (_vramMap == nullptr) {
        _vramMap = _aperture->map();
    }
    if (_vramMap != nullptr && kStduVramControl) {
        volatile UInt32 *vram = (volatile UInt32 *)_vramMap->getVirtualAddress();
        const UInt32 vw = (UInt32)(_devicePitch / 4);
        for (UInt32 y = 0; y < _deviceHeight; y++) {
            volatile UInt32 *row = vram + (IOByteCount)y * vw;
            for (UInt32 x = 0; x < vw; x++) {
                row[x] = 0xFFFF00FF;
            }
        }
        __asm__ volatile ("" ::: "memory");
        IOLog(VGPU_FB_TAG ": present: legacy arm filled VRAM with magenta (draw detector) "
              "first=0x%08x\n", vram[0]);
    }

    // DEFINE_SCREEN: SVGAScreenObject v1 -- structSize=28 (no backingStore field; it is
    // optional with SVGA_FIFO_CAP_SCREEN_OBJECT, which this host advertises).
    words[0] = kSvgaCmdDefineScreen;
    words[1] = 28;                  // sizeof(SVGAScreenObject) without backingStore
    words[2] = 28;                  // structSize
    words[3] = 0;                   // screenId
    words[4] = kSvgaScreenIsPrimary;
    words[5] = _deviceWidth;
    words[6] = _deviceHeight;
    words[7] = 0;                   // root.x
    words[8] = 0;                   // root.y
    words[9] = 0;                   // cloneCount
    if (!fifoSubmitWords(fifo, words, 10) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": present: DEFINE_SCREEN refused\n");
        return;
    }

    // DEFINE_GMRFB: {ptr{gmrId, offset}, bytesPerLine, format} -- 16 bytes.
    words[0] = kSvgaCmdDefineGmrfb;
    words[1] = 16;
    words[2] = kSvgaGmrFramebuffer;   // gmrId: the legacy framebuffer (BAR1)
    words[3] = _deviceFbOffset;       // ptr.offset: byte offset into BAR1
    words[4] = _devicePitch;
    words[5] = kSvgaGmrfbFormatBgrx32;
    if (!fifoSubmitWords(fifo, words, 6) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": present: DEFINE_GMRFB refused\n");
        return;
    }

    // BLIT_GMRFB_TO_SCREEN: {srcOrigin{x,y}, destRect{l,t,r,b}, destScreenId} -- 28 bytes.
    words[0] = kSvgaCmdBlitGmrfbToScreen;
    words[1] = 28;
    words[2] = 0;                   // srcOrigin.x
    words[3] = 0;                   // srcOrigin.y
    words[4] = 0;                   // destRect.left
    words[5] = 0;                   // destRect.top
    words[6] = _deviceWidth;        // destRect.right
    words[7] = _deviceHeight;       // destRect.bottom
    words[8] = 0;                   // destScreenId
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": present: BLIT_GMRFB_TO_SCREEN refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": present: screen 0 defined (%ux%u), GMRFB = BAR1+%u, blit acked -- "
          "screen-object present path is UP\n", _deviceWidth, _deviceHeight, _deviceFbOffset);

    // Keep presenting. The first tick fires after one interval; each tick reschedules.
    _presentCall = thread_call_allocate(&vgpuFramebuffer::presentTickC, (thread_call_param_t)this);
    if (_presentCall == nullptr) {
        IOLog(VGPU_FB_TAG ": present: could not allocate the present thread_call\n");
        return;
    }
    schedulePresent();
    IOLog(VGPU_FB_TAG ": present: periodic re-present started (%u ms)\n", kPresentIntervalMs);
}

// thread_call callback: fire one present and reschedule.
void vgpuFramebuffer::presentTickC(thread_call_param_t param0, thread_call_param_t param1) {
    (void)param1;
    vgpuFramebuffer *self = (vgpuFramebuffer *)param0;
    if (self != nullptr) {
        self->presentTick();
        self->schedulePresent();
    }
}

//
// §38.19: the heartbeat gets its own thread_call, deliberately NOT piggy-backed on the
// present timer. Measured reason: the present timer stalls. Its log showed
//
//   04:25:05.722  heartbeat #1  tick 1
//   04:25:07.745  heartbeat #2  tick 60     <- 2 s, correct
//   04:25:38.662  heartbeat #3  tick 61     <- 31 s later, tick advanced by ONE
//
// so presentTick() can go half a minute without being entered at all (thread_call_enter_delayed
// returns FALSE when the call is already pending, and the present body itself is far from
// free). A heartbeat that rides on it inherits that stall and blows the host's 20 s timeout.
// One thread_call for the heartbeat, re-armed from inside its own callback, is independent of
// whatever the present path is doing.
//
void vgpuFramebuffer::rpcTickC(thread_call_param_t param0, thread_call_param_t param1) {
    (void)param1;
    vgpuFramebuffer *self = (vgpuFramebuffer *)param0;
    if (self == nullptr) {
        return;
    }
    self->_rpcSent++;
    const UInt32 status = vmwareBackdoor(kBdoorCmdMsg, 0, 0);
    if (self->_rpcSent <= 3 || self->_rpcSent % 30 == 0) {
        IOLog(VGPU_FB_TAG ": rpc: heartbeat #%u -> status 0x%x\n", self->_rpcSent, status);
    }
    // Re-arm from the callback: one live thread_call at a time, no queue build-up.
    if (self->_rpcCall != nullptr) {
        UInt64 now = 0, delta = 0;
        clock_get_uptime(&now);
        nanoseconds_to_absolutetime(kRpcHeartbeatNs, &delta);
        thread_call_enter_delayed(self->_rpcCall, now + delta);
    }
}

// Start the heartbeat thread_call. Called as early as we can in start(), independently of
// whether the present path ever comes up -- the host must not be left to expire while we are
// still deciding how to draw.
bool vgpuFramebuffer::startRpcHeartbeat(void) {
    if (!kEnableRpcHeartbeat) {
        return false;
    }
    if (_rpcCall != nullptr) {
        return true;                    // already running
    }
    _rpcCall = thread_call_allocate(&vgpuFramebuffer::rpcTickC, (thread_call_param_t)this);
    if (_rpcCall == nullptr) {
        IOLog(VGPU_FB_TAG ": rpc: could not allocate the heartbeat thread_call\n");
        return false;
    }
    UInt64 now = 0, delta = 0;
    clock_get_uptime(&now);
    nanoseconds_to_absolutetime(kRpcHeartbeatNs, &delta);
    thread_call_enter_delayed(_rpcCall, now + delta);
    IOLog(VGPU_FB_TAG ": rpc: heartbeat armed (every %llu ms, host timeout is ~20 s)\n",
          (unsigned long long)(kRpcHeartbeatNs / 1000000ULL));
    return true;
}

// Schedule the next present tick. thread_call_enter_delayed takes an absolute mach
// absolute-time deadline; compute now + interval.
void vgpuFramebuffer::schedulePresent(void) {
    if (_presentCall == nullptr) {
        return;
    }
    UInt64 now = 0, delta = 0;
    clock_get_uptime(&now);
    nanoseconds_to_absolutetime((UInt64)kPresentIntervalMs * 1000000ULL, &delta);
    thread_call_enter_delayed(_presentCall, now + delta);
}

// Delayed DX test entry: runs kDxTestDelayS seconds after bring-up.
void vgpuFramebuffer::dxTestC(thread_call_param_t param0, thread_call_param_t param1) {
    (void)param1;
    vgpuFramebuffer *self = (vgpuFramebuffer *)param0;
    if (self == nullptr) {
        return;
    }
    self->_dxBusy = 1;
    __asm__ volatile ("" ::: "memory");
    IOLog(VGPU_FB_TAG ": dx: delayed test starting (fifo=%p)\n", (void *)self->_dxFifo);
    self->dxReadbackTest(self->_dxFifo);
    __asm__ volatile ("" ::: "memory");
    self->_dxBusy = 0;

    // Arm the periodic debug probe (ping-pong pair keeps it re-arming).
    if (kProbeEveryS != 0) {
        self->armProbe(self->_probeCall);
    }
}

// Periodic probe: same packed DEFINE+BIND as the DX chain, on the device queue --
// this is the submission a host-side debugger watches (status write on the CB header).
//
// NOTE: a thread_call CANNOT re-enter itself, so thread_call_enter_delayed() from
// inside the running call silently does nothing -- the original self-rearming
// probe fired exactly once per boot. Two calls ping-pong instead: probeC arms
// _probeCall2 and probeC2 arms _probeCall.
void vgpuFramebuffer::probeRun(void) {
    if (_cbHeaderVirt == nullptr || _cbDataVirt == nullptr) {
        return;
    }
    _dxBusy = 1;
    __asm__ volatile ("" ::: "memory");
    UInt32 packed[8] = {
        kSvga3dCmdDxDefineContext, 4, 1,
        kSvga3dCmdDxBindContext, 12, 1, kCtxMobId, 0,
    };
    // FLAGS MUST BE 1 (NO_IRQ only): the VMX header validator does
    //   test flags, 0xFFFFFFFE ; jne -> CB_HEADER_ERROR  (0x140087c90)
    // so no DX_CONTEXT bit here. The cid travels inside the command body.
    bool ok = cbSubmitCtx(packed, 8, 0x3F, 0);
    IOLog(VGPU_FB_TAG ": probe: DEFINE+BIND on device queue -> %s\n",
          ok ? "ACCEPTED" : "refused");
    // Same CB on the regular queue 0 (ctx-0): with flags=1 the header validator
    // passes; this tells us whether the DX command handlers accept the pair there.
    // (An error on ctx-0 wedges that queue only -- the device queue keeps probing.)
    bool ok0 = cbSubmitCtx(packed, 8, 0, 0);
    IOLog(VGPU_FB_TAG ": probe: DEFINE+BIND on queue 0 -> %s\n",
          ok0 ? "ACCEPTED" : "refused");
    _dxBusy = 0;
}

void vgpuFramebuffer::armProbe(thread_call_t call) {
    if (call == nullptr) {
        return;
    }
    AbsoluteTime when = 0;
    uint64_t delta = 0;
    clock_get_uptime(&when);
    nanoseconds_to_absolutetime((UInt64)kProbeEveryS * 1000000000ULL, &delta);
    thread_call_enter_delayed(call, when + delta);
}

void vgpuFramebuffer::probeC(thread_call_param_t param0, thread_call_param_t param1) {
    (void)param1;
    vgpuFramebuffer *self = (vgpuFramebuffer *)param0;
    if (self == nullptr) {
        return;
    }
    self->probeRun();
    self->armProbe(self->_probeCall2);
}

void vgpuFramebuffer::probeC2(thread_call_param_t param0, thread_call_param_t param1) {
    (void)param1;
    vgpuFramebuffer *self = (vgpuFramebuffer *)param0;
    if (self == nullptr) {
        return;
    }
    self->probeRun();
    self->armProbe(self->_probeCall);
}

void vgpuFramebuffer::presentTick(void) {
    //
    // §38.19: the heartbeat goes FIRST, before every early return below. The whole point is
    // to keep the host from declaring us dead; a tick that bails out early on a missing FIFO
    // or an unset mode is exactly the situation where we most need the host to keep the
    // display alive. _presentTicks is bumped here so the cadence does not depend on whether
    // the present path below is reachable.
    //
    _presentTicks++;
    // The heartbeat is NOT here: see startRpcHeartbeat(). It rides its own thread_call
    // because presentTick can stall for tens of seconds (measured, §38.19).
    if (_dxBusy) {
        return;                     // DX test owns the FIFO right now
    }
    if (_presentFifo == nullptr || !_deviceModeValid) {
        return;
    }
    if (_presentTicks <= 3 || _presentTicks % 300 == 0) {
        IOLog(VGPU_FB_TAG ": present tick %u (next %u stop %u)\n", _presentTicks,
              _presentFifo[kSvgaFifoNextCmd], _presentFifo[kSvgaFifoStop]);
    }
    if (_stduActive && kDxTest && _presentTicks == 600) {
        IOLog(VGPU_FB_TAG ": present tick 600: running the DX round-trip test\n");
        dxReadbackTest(_presentFifo);
    }
    if (kStduPaint && kStduReadback && kStduDma && !kStduFlat && _stduActive &&
        _presentTicks == 4) {
        stduReadbackProbe();    // §36.8: once, after the paint has been DMA'd a few times
    }
    // §45b: ask the host what it is CURRENTLY DISPLAYING for this screen.
    //
    // This is the one measurement that separates the two remaining explanations, and it
    // has to be run on the normal arm -- until now it only ever ran on the flat arm,
    // which is why the normal path has never had an answer to "is the surface wrong, or
    // is the display pipeline not consuming it?".
    //
    //   GB_SCREEN_DMA returns pixels  -> the host IS displaying something and can push it
    //                                    into guest memory. If those pixels are not our
    //                                    pattern, the surface we bound never reached the
    //                                    display, and the fault is in the upload/bind.
    //   GB_SCREEN_DMA returns zeros   -> the host's display for stid 0 is genuinely blank,
    //                                    i.e. it never sampled the surface at all; the
    //                                    fault is upstream of the encoder and UPDATE_*
    //                                    volume is irrelevant until that is fixed.
    //
    // Placed after the readback probe so the two verdicts land in the log together.
    if (_stduActive && !kStduFlat && kStduScreenDma && _presentTicks == 8) {
        stduScreenDma(_presentFifo);
    }
    if (_stduActive && kStduFlat) {
        // §38.12 flat present: the surface has exactly one data page, and it IS the MOB.
        // Nothing to copy, nothing to DMA -- recolour the page, tell the host both that
        // the surface contents changed (UPDATE_GB_IMAGE, which the M6 commit credits for
        // making the host re-read the MOB) and that the screen target should be refreshed.
        //
        // The colour is a sawtooth over the tick counter, so consecutive frames differ:
        // if anything on the host side is diffing pixels, this gives it something new on
        // every single tick. The white band stays for the length of the page so a fixed
        // feature is visible even if only one frame ever gets picked up.
        volatile UInt32 *px = (volatile UInt32 *)_flatVirt;
        if (px != nullptr) {
            const UInt32 phase = (UInt32)(_presentTicks & 7);
            const UInt32 bar = 0xFFFFFFFFu;
            UInt32 base = 0xFF000000u;
            switch (phase) {
                case 0: base |= 0x00FF0000u; break;   // red
                case 1: base |= 0x0000FF00u; break;   // green
                case 2: base |= 0x000000FFu; break;   // blue
                case 3: base |= 0x00FFFF00u; break;   // yellow
                case 4: base |= 0x00FF00FFu; break;   // magenta
                case 5: base |= 0x0000FFFFu; break;   // cyan
                case 6: base |= 0x00FF8000u; break;   // orange
                default: base = bar; break;           // white
            }
            const UInt32 n = (UInt32)(kGbPageBytes / 4);
            const UInt32 bandStart = (UInt32)((_presentTicks * 16) % (n - 128));
            for (UInt32 i = 0; i < n; i++) {
                px[i] = ((i >= bandStart) && (i < bandStart + 128)) ? bar : base;
            }
            __asm__ volatile ("" ::: "memory");
            (void)_flatMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);

            // §45: this used to send 52 bytes / 17 dwords with three made-up trailing
            // fields (box.srcx/srcY/srcZ and box.dstx/dstY/dstZ) that SVGA3dBox does not
            // have. The authoritative command (svga3d_cmd.h:1253) is
            //     SVGA3dCmdUpdateGBImage { SVGA3dSurfaceImageId image; SVGA3dBox box; }
            //   image = {sid, face, mipmap}   12 B
            //   box   = {x, y, z, w, h, d}    24 B   <-- 6 dwords, six fields, no src/dst
            //   body  = 36 B = 9 dwords, framed = 11 dwords.
            // With 52 the host read the next command header 16 bytes into our padding,
            // which is why every update after the first was swallowed. Same class of bug
            // as the DEFINE body being 40 instead of 36.
            //
            // The box names the whole first row because the flat MOB is a single 4 KiB
            // page (1024x1): w = 1024 pixels, h = 1 row, d = 1 slice.
            UInt32 img[11];
            img[0] = kSvga3dCmdUpdateGbImage;
            img[1] = 36;                                        // body bytes
            img[2] = _surfaceId;                                // image.sid
            img[3] = 0;                                         // image.face
            img[4] = 0;                                         // image.mipmap
            img[5] = 0;                                         // box.x
            img[6] = 0;                                         // box.y
            img[7] = 0;                                         // box.z
            img[8] = kStduWidth;                                // box.w
            img[9] = 1;                                         // box.h
            img[10] = 1;                                        // box.d
            fifoSubmitWords(_presentFifo, img, 11);

            UInt32 upd[7];
            upd[0] = kSvga3dCmdUpdateGbScreenTarget;
            upd[1] = 20;
            upd[2] = _stduStid;
            upd[3] = 0;
            upd[4] = 0;
            upd[5] = kStduWidth;
            upd[6] = kStduSurfHeight;
            fifoSubmitWords(_presentFifo, upd, 7);

            if (_paintLogged == 0) {
                _paintLogged = 1;
                IOLog(VGPU_FB_TAG ": flat: present armed (%ux%u panel, 4 KiB MOB, "
                      "colour 0x%08x) first word 0x%08x\n",
                      kStduWidth, kStduSurfHeight, base, px[0]);
            }
        }
        // §38.13: once, after the present loop has been running long enough for the host
        // to be well past its first update.
        if (_presentTicks == 20) {
            stduFlatReadback(_presentFifo);
        }
        // §38.14: a couple of seconds later, ask the host for what it is displaying.
        if (_presentTicks == 60) {
            stduScreenDma(_presentFifo);
        }
        return;
    }
    if (_stduActive) {
        // STDU present, two steps per tick:
        //
        //   1. copy the framebuffer (VRAM, uncached) into the surface's source region
        //      (guest RAM), then SVGA_3D_CMD_SURFACE_DMA guest->host into surface sid 2 --
        //      the host does NOT read the MOB directly; surface content must arrive
        //      through the DMA engine (this is what vmwgfx_stdu.c does);
        //   2. UPDATE_GB_SCREENTARGET with the full rect so the host presents the
        //      refreshed surface.
        //
        // No fences -- FIFO order guarantees the UPDATE sees the completed DMA, and the
        // next tick's DMA supersedes anything stale.
        if (_vramVirt != nullptr && _stduVirt != nullptr) {
            memcpy(_stduVirt, _vramVirt, (IOByteCount)kStduWidth * kStduSurfHeight * 4);
        }

        // 10-29 22:5x (§36.5 verification): paint a known pattern over the copy.
        //
        // The STDU path is up and its FIFO never wedges, yet the screen is pure black --
        // 12288 sampled points, one distinct colour. That leaves two very different
        // explanations, and no amount of staring at the command sequence separates them:
        //
        //   (a) the present path is broken -- our pixels never reach the screen, or
        //   (b) the present path is fine, but VRAM itself is black, because no window
        //       server is drawing into the framebuffer on this guest.
        //
        // (b) would mean the black screen is CORRECT, and we have been treating a working
        // path as a broken one. So stop waiting for the host to put something in VRAM and
        // supply the content ourselves: green top half, blue bottom half, red 32-pixel
        // border. If that shows up on the VNC capture, the path works end to end and the
        // open question becomes "what should be in VRAM", not "why is presentation dead".
        //
        // Gated by kStduPaint so it can be switched off the moment a real content source
        // exists -- painting over the framebuffer every tick would then hide it.
        if (kStduPaint && _stduVirt != nullptr) {
            volatile UInt32 *px = (volatile UInt32 *)_stduVirt;
            const UInt32 w = kStduWidth, h = kStduSurfHeight;
            const UInt32 green = 0xFF00FF00, blue = 0xFF0000FF, red = 0xFFFF0000;
            const UInt32 white = 0xFFFFFFFF;
            for (UInt32 y = 0; y < h; y++) {
                const bool edgeRow = (y < 32) || (y >= h - 32);
                UInt32 *row = (UInt32 *)px + (IOByteCount)y * w;
                if (edgeRow) {
                    for (UInt32 x = 0; x < w; x++) row[x] = red;
                } else {
                    const UInt32 mid = (y < h / 2) ? green : blue;
                    for (UInt32 x = 0; x < w; x++)
                        row[x] = (x < 32 || x >= w - 32) ? red : mid;
                }
            }

            // §38.11: a 128x128 white block that walks left to right, one step per tick.
            //
            // The alternating half-rect above only changes *which* dwords the update names.
            // If the host instead diffs the pixels it was told about against what it last
            // encoded, a static pattern still diffs to nothing no matter how the rect moves.
            // Moving the block changes the pixel content itself on every frame, so either
            // kind of change-detection now has something to see. Position is a plain
            // function of _presentTicks, so the VNC capture can be matched against the tick
            // value in the log and the two cannot drift apart.
            const UInt32 blockSize = 128;
            const UInt32 travel = w - blockSize;
            const UInt32 blockX = (UInt32)((_presentTicks * 8) % travel);
            const UInt32 blockY = (h - blockSize) / 2;
            for (UInt32 y = blockY; y < blockY + blockSize; y++) {
                UInt32 *row = (UInt32 *)px + (IOByteCount)y * w;
                for (UInt32 x = blockX; x < blockX + blockSize; x++) row[x] = white;
            }
            __asm__ volatile ("" ::: "memory");
            // §45: flush the painted frame out to the pages the host will read.
            //
            // On x86 kIOMemoryIncoherentIOFlush is a fence, not a cache writeback, so this
            // is not strictly load-bearing on this host -- but it is the only thing that
            // makes the memory visible as "the guest asserted these bytes are final", and
            // it costs one call per frame. It was present on the flat arm and missing here,
            // which is exactly the kind of asymmetry that makes a bisect lie.
            (void)_stduMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kStduBytes);
            // Log once, from the first painted frame: proves the loop ran and shows the
            // buffer's physical backing, so a black capture can't be blamed on the wrong
            // buffer. kStduPaint + this line is also what build.sh greps for.
            if (_paintLogged == 0) {
                _paintLogged = 1;
                IOLog(VGPU_FB_TAG ": present: STDU paint armed (red border, green top, "
                      "blue bottom, walking white block) first=0x%08x mid=0x%08x\n",
                      px[0], px[(IOByteCount)(h / 2 + h / 4) * w + w / 2]);
            }
        }

        // §38h control arm: the same pattern straight into the legacy frame buffer, plus
        // the legacy SVGA_CMD_UPDATE that has always made it to the screen. See the
        // kStduVramControl comment for what each outcome means.
        // §47 control arm, rewritten: MAGENTA into VRAM, not a copy of the surface.
        //
        // The original arm memcpy'd the surface into VRAM, which meant both sources held
        // exactly the same pattern -- so whatever the capture showed, it could not tell the
        // two apart. That was true of the code and it invalidated the arm, which is why it
        // sat at 0 for so long. Now VRAM gets a colour the surface never has, so a capture
        // answers a question no log can:
        //
        //   red border / green top  -> the host is presenting our surface; the STDU path is
        //                              correct and the black screen has some other cause
        //   magenta                 -> the host is scanning VRAM after all; the screen fails
        //                              because nothing writes the framebuffer, not because
        //                              the screen target is wrong
        //   black                   -> the host presents NEITHER: taking the FIFO is what
        //                              killed presentation, and every screen-target command
        //                              written since has been beside the point
        //
        // SVGA_CMD_UPDATE has no header: it IS the dwords {cmd, x, y, width, height}.
        if (kStduVramControl && _vramVirt != nullptr) {
            const IOByteCount vramPixels = (IOByteCount)kStduWidth * kStduSurfHeight;
            volatile UInt32 *vramPx = (volatile UInt32 *)_vramVirt;
            for (IOByteCount i = 0; i < vramPixels; i++) {
                vramPx[i] = 0xFFFF00FF;   // magenta, opaque
            }
            __asm__ volatile ("" ::: "memory");
            UInt32 upd0[5];
            upd0[0] = kSvgaCmdUpdate;
            upd0[1] = 0;
            upd0[2] = 0;
            upd0[3] = kStduWidth;
            upd0[4] = kStduSurfHeight;
            fifoSubmitWords(_presentFifo, upd0, 5);
            if (_vramLogged == 0) {
                _vramLogged = 1;
                IOLog(VGPU_FB_TAG ": present: VRAM control arm armed (magenta %llu px at "
                      "%p), surface keeps the red/green pattern\n",
                      (unsigned long long)vramPixels, _vramVirt);
            }
        }

        // §37: SURFACE_DMA is a proven no-op on this host. §37.3 measured it directly --
        // READBACK into a zeroed scratch buffer left all 768 sampled words zero, so the
        // DMA engine never touched memory in either direction. M6 (b89c619) got a live
        // display anyway, and its commit is explicit about which command did the work:
        //
        //   "UPDATE_GB_IMAGE (1101) after writing the surface's MOB: the host keeps an
        //    internal copy of surface contents; a direct MOB write is invisible until it
        //    is told to re-read (vmwgfx's vmw_kms_update_proxy does exactly this)."
        //
        // So kStduDma = 0 makes the present loop the M6 shape exactly: write the MOB,
        // UPDATE_GB_IMAGE, UPDATE_GB_SCREENTARGET. If the display lights up, the DMA was
        // not just useless but actively harmful -- an unanswered DMA may be what made the
        // host treat the surface as busy and skip the proxy update as well.
        // §45: the upload names the same half as the UPDATE_GB_IMAGE / UPDATE_GB_SCREENTARGET
        // pair below. It used to compute its own half from the tick parity, so on every
        // frame the DMA uploaded one half while the screen update asked the host to show the
        // other -- the last command in the stream won, and it was always asking for data
        // that had not been uploaded on that frame.
        const UInt32 halfH = kStduSurfHeight / 2;
        const bool lowerHalf = (_presentTicks & 1) != 0;
        const UInt32 halfY = lowerHalf ? halfH : 0;

        if (kStduDma) {
        UInt32 dma[21];
        dma[0] = 1044;                  // SVGA_3D_CMD_SURFACE_DMA
        dma[1] = 28 + 36 + 12;          // body + one CopyBox + suffix
        dma[2] = _stduMobId;            // guest.ptr.gmrId (mobid on a GB device)
        dma[3] = 0;                     // guest.ptr.offset
        dma[4] = kStduWidth * 4;        // guest.pitch
        dma[5] = _surfaceId;            // host.sid
        dma[6] = 0;                     // host.face
        dma[7] = 0;                     // host.mipmap
        dma[8] = 1;                     // transfer = SVGA3D_WRITE_HOST_VRAM
        dma[9]  = 0;                    // box.x
        dma[10] = halfY;                // box.y
        dma[11] = 0;                    // box.z
        dma[12] = kStduWidth;           // box.w
        dma[13] = halfH;                // box.h
        dma[14] = 1;                    // box.d
        dma[15] = 0;                    // box.srcx
        dma[16] = halfY;                // box.srcy
        dma[17] = 0;                    // box.srcz
        dma[18] = 12;                   // suffix.suffixSize
        dma[19] = (UInt32)((IOByteCount)kStduWidth * kStduSurfHeight * 4);   // suffix.maximumOffset
        dma[20] = 0;                    // suffix.flags
        fifoSubmitWords(_presentFifo, dma, 21);
        }

        // §38.8 said the upstream STDU CPU path sends neither UPDATE_GB_SURFACE (1102) nor
        // UPDATE_GB_IMAGE (1101) per frame, so both were dropped from the loop.
        //
        // §45: that reasoning was built on a broken measurement. At the time this was
        // decided, every surface we created had a mis-filled OTable entry (surfaceFlags
        // written as 64 bits into a 32-bit field), so the host's surface described
        // width=0 height=1, and no upload command could have worked. Dropping 1101 because
        // "the DMA already moved the pixels" assumed the DMA worked, and §37's evidence
        // that it did not was collected under the same defect.
        //
        // M6's commit b89c619 is explicit that UPDATE_GB_IMAGE is what made the display go
        // live: "the host keeps an internal copy of surface contents; a direct MOB write is
        // invisible until it is told to re-read". KMS on this same device class does exactly
        // this (vmw_kms_update_proxy). It is kept as its own switch so a single boot can
        // attribute the result to one command rather than to the bundle.
        //
        // §38.11: the rect alternates between the two halves on every frame, because the
        // host's log suggests it diffs consecutive rects. `halfY` is computed once above,
        // so the DMA, the image update and the screen update all name the SAME half --
        // when they disagreed, whichever came last silently undid the others.

        if (kStduImage) {
            // §45: authoritative SVGA3dCmdUpdateGBImage, 36-byte body:
            //   image{sid,face,mipmap} then box{x,y,z,w,h,d}
            UInt32 img[11];
            img[0]  = kSvga3dCmdUpdateGbImage;
            img[1]  = 36;
            img[2]  = _surfaceId;
            img[3]  = 0;                        // face
            img[4]  = 0;                        // mipmap
            img[5]  = 0;                        // box.x
            img[6]  = halfY;                    // box.y
            img[7]  = 0;                        // box.z
            img[8]  = kStduWidth;               // box.w
            img[9]  = halfH;                    // box.h
            img[10] = 1;                        // box.d
            fifoSubmitWords(_presentFifo, img, 11);
        }

        UInt32 upd[7];
        upd[0] = kSvga3dCmdUpdateGbScreenTarget;
        upd[1] = 20;
        upd[2] = _stduStid;
        upd[3] = 0;                                   // rect.x
        upd[4] = halfY;                               // rect.y -- same half as the upload
        upd[5] = kStduWidth;                          // rect.w
        upd[6] = halfH;                               // rect.h
        fifoSubmitWords(_presentFifo, upd, 7);
        return;
    }

    // §38.12: the flat arm is the live experiment. Flip it back to 0 here (or at its
    // definition) once it has answered; nothing below is reachable while it is 1.

    // Legacy screen-object present: one full-screen blit; no fence -- the FIFO order
    // guarantees a later blit wins, and skipping the wait keeps the timer cadence stable.
    UInt32 words[9];
    words[0] = kSvgaCmdBlitGmrfbToScreen;
    words[1] = 28;
    words[2] = 0;
    words[3] = 0;
    words[4] = 0;
    words[5] = 0;
    words[6] = _deviceWidth;
    words[7] = _deviceHeight;
    words[8] = 0;
    fifoSubmitWords(_presentFifo, words, 9);
}

// The backdoor needs the write before each read; a tiny wrapper keeps the dump loop honest.
UInt32 vgpuFramebuffer::devcapRead(UInt32 index) {
    svgaWriteRegister(_svgaPortBase, kSvgaRegDevCap, index);
    return svgaReadRegister(_svgaPortBase, kSvgaRegDevCap);
}

//
// Submit one SVGA_CMD_FENCE and poll for its acknowledgement.
//
// Submission follows the reserve protocol. Two corrections went in here after the first
// attempt hung with no log output at all:
//
//   1. The host was never pinged. NEXT_CMD is the guest's write pointer; advancing it is a
//      memory store the host has no reason to notice on its own. vmw_local_fifo_commit()
//      always follows it with vmw_fifo_ping_host(SVGA_SYNC_GENERIC) == write SVGA_REG_SYNC 1.
//      Without that, "the host did not consume our command" is not even a finding -- the host
//      was never asked to.
//
//   2. The poll was an unbounded-feeling 400M-iteration volatile spin. Even if it did return,
//      it would have blocked the whole kext for tens of seconds on this VM, and the success
//      path -- the only path that logged -- printed nothing on failure until the end. Now the
//      loop is bounded by wall-clock time, and it logs the transition it saw.
//
//   3. CONFIG_DONE was being written to register 25 instead of 20 (§45), so none of the above
//      ever mattered: the host never believed a FIFO existed. Both the FIFO-memory capability
//      dword and the register one are logged side by side now, because they disagree and the
//      register is the authoritative one.
void vgpuFramebuffer::fenceTest(volatile UInt32 *fifo) {
    const UInt32 caps = fifo[kSvgaFifoCapabilities];
    // The FIFO-memory capability dword and the register capability word are different things,
    // and this host only fills in the latter. SVGA_REG_FIFO_CAPS = 68 is the one vmwgfx reads
    // when it wants to know whether reserve/fence are implemented.
    const UInt32 regCaps = svgaReadRegister(_svgaPortBase, kSvgaRegFifoCaps);
    const UInt32 next0 = fifo[kSvgaFifoNextCmd];
    const UInt32 stop0 = fifo[kSvgaFifoStop];
    const UInt32 fence0 = fifo[kSvgaFifoFence];
    IOLog(VGPU_FB_TAG ": fence test: fifo caps 0x%x, SVGA_REG_FIFO_CAPS(68) 0x%x "
          "(reserve %s, fence %s); start next_cmd %u stop %u fence 0x%x\n",
          caps, regCaps,
          (regCaps & kSvgaFifoCapReserve) ? "yes" : "no",
          (regCaps & kSvgaFifoCapFence) ? "yes" : "no",
          next0, stop0, fence0);

    const UInt32 next = fifo[kSvgaFifoNextCmd];
    const UInt32 stop = fifo[kSvgaFifoStop];
    const UInt32 fmin = fifo[kSvgaFifoMin];
    const UInt32 fmax = fifo[kSvgaFifoMax];
    // Same free-space rule as vmw_fifo_is_full(): when the write pointer has wrapped past the
    // read pointer the ring is full, not empty.
    const UInt32 bytesFree = (next >= stop) ? ((fmax - next) + (stop - fmin))
                                            : (stop - next);
    if (bytesFree < 8) {
        IOLog(VGPU_FB_TAG ": fence test skipped: %u bytes free (min %u max %u next %u stop %u)\n",
              bytesFree, fmin, fmax, next, stop);
        return;
    }

    const UInt32 fenceValue = 0x31415926;
    const bool  canReserve  = (regCaps & kSvgaFifoCapReserve) != 0;
    if (canReserve) {
        fifo[kSvgaFifoReserved] = 8;
    }
    fifo[(next / sizeof (UInt32))]     = kSvgaCmdFence;
    fifo[(next / sizeof (UInt32)) + 1] = fenceValue;
    __asm__ volatile ("" ::: "memory");
    fifo[kSvgaFifoNextCmd] = next + 8;
    __asm__ volatile ("" ::: "memory");
    if (canReserve) {
        fifo[kSvgaFifoReserved] = 0;
    }
    svgaPingHost(_svgaPortBase);
    IOLog(VGPU_FB_TAG ": fence 0x%x written at %u, next_cmd -> %u, host pinged (SYNC=1), polling\n",
          fenceValue, next, next + 8);

    // 2 s in 1000 steps. Long enough for a healthy host, short enough that a wedged device
    // costs 2 seconds of boot instead of 30+.
    const UInt32 kSteps = 2000;
    const UInt32 kStepUs = 1000;
    UInt32 lastStop = stop;
    for (UInt32 i = 0; i < kSteps; i++) {
        if (fifo[kSvgaFifoFence] == fenceValue) {
            IOLog(VGPU_FB_TAG ": FIFO command stream VERIFIED: fence 0x%x acked after ~%u us "
                  "(next_cmd %u stop %u)\n",
                  fenceValue, i * kStepUs, fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop]);
            return;
        }
        const UInt32 nowStop = fifo[kSvgaFifoStop];
        if (nowStop != lastStop) {
            IOLog(VGPU_FB_TAG ": fence test: host advanced STOP %u -> %u after ~%u us\n",
                  lastStop, nowStop, i * kStepUs);
            lastStop = nowStop;
        }
        IODelay(kStepUs);
    }

    IOLog(VGPU_FB_TAG ": fence 0x%x NOT acked in %u us. next_cmd %u stop %u fence 0x%x "
          "(started at next_cmd %u stop %u fence 0x%x)\n",
          fenceValue, kSteps * kStepUs,
          fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop], fifo[kSvgaFifoFence],
          next0, stop0, fence0);

    // A command the host never consumed must not be left in the ring: the next boot would
    // either re-parse it or, worse, parse its tail as a header. Put the write pointer back.
    fifo[kSvgaFifoNextCmd] = next;
    __asm__ volatile ("" ::: "memory");
    IOLog(VGPU_FB_TAG ": fence test: rewound next_cmd to %u\n", next);
}

//
// FIFO submission helpers.
//
// fifoSubmitWords writes a dword stream through the reserve protocol and refuses to wrap
// past FIFO_MAX (our streams are tens of bytes; the data area is 256 KiB). fenceAck submits
// SVGA_CMD_FENCE with a fresh sequence number and polls; because the FIFO is processed in
// order, an acknowledged fence implies every earlier command was consumed.
//
bool vgpuFramebuffer::fifoSubmitWords(volatile UInt32 *fifo, const UInt32 *words, UInt32 wordCount) {
    // Free-space accounting, checked against upstream vmw_local_fifo_reserve()
    // (drivers/gpu/drm/vmwgfx/vmwgfx_cmd.c, formerly vmwgfx_fifo.c):
    //
    //   next >= stop  ->  the ring has wrapped past us; free = (max - next) + (stop - min)
    //   next <  stop  ->  free = stop - next
    //
    // The old code used "max - next" for the first case and dropped the (stop - min)
    // term. That is wrong in exactly the state the ring ends up in after one full lap:
    // next parks just below FIFO_MAX, the only contiguous run left is min..stop, and the
    // host is happily consuming it -- but we computed free == 8 and refused every
    // subsequent command forever. Observing it: "present tick 1800 (next 262136 stop
    // 262136)" followed by an endless "fifo submit refused: N bytes needed, 8 free".
    // In the second case the old code was also one term short: a command that does not
    // fit before STOP must wrap, and what it wraps into is (max - next) + (stop - min).
    const UInt32 next = fifo[kSvgaFifoNextCmd];
    const UInt32 fmax = fifo[kSvgaFifoMax];
    const UInt32 fmin = fifo[kSvgaFifoMin];
    const IOByteCount byteCount = (IOByteCount)wordCount * sizeof (UInt32);

    // Retry loop on the "the ring really is full" path, mirroring vmw_fifo_wait():
    // wait for the host to advance STOP rather than dropping the command. A dropped
    // submission is far worse than a slow one -- for a multi-command sequence it leaves
    // the remainder to be parsed as a fresh command header, which is how a black screen
    // turns into an aborted FIFO.
    //
    // 3 s is the upstream timeout. In the per-frame present loop this should never be
    // reached (a frame is ~90 bytes against a 254 KiB ring); it exists so a genuine
    // stall degrades into a late frame instead of a dead pipeline.
    const UInt64 spinBound = (UInt64)kPresentIntervalMs * 40;   // ~3 s at 33 ms/tick
    UInt32 stop = fifo[kSvgaFifoStop];
    UInt32 bytesFree = (next >= stop) ? ((fmax - next) + (stop - fmin))
                                      : (stop - next);
    UInt32 reNext = next;
    for (UInt64 spins = 0; byteCount > bytesFree; spins++) {
        if (spins >= spinBound) {
            IOLog(VGPU_FB_TAG ": fifo submit refused: %u bytes needed, %u free "
                  "(next %u stop %u) after wait\n",
                  (unsigned)byteCount, bytesFree, reNext, stop);
            return false;
        }
        stop = fifo[kSvgaFifoStop];
        // The host may also have advanced NEXT_CMD past us in the meantime only if we
        // wrapped; it never writes NEXT_CMD, so only STOP moves. Re-read NEXT_CMD anyway
        // so the accounting stays honest if the ring was re-pointed under us.
        reNext = fifo[kSvgaFifoNextCmd];
        bytesFree = (reNext >= stop) ? ((fmax - reNext) + (stop - fmin))
                                     : (stop - reNext);
        // vmw_fifo_wait() pings with SVGA_SYNC_FIFOFULL before sleeping. The reason is the
        // same as everywhere else in this file: the host does not poll the ring on its own.
        if ((spins & 0xFF) == 0) {
            svgaPingHost(_svgaPortBase);
        }
        IODelay(50);
    }
    if (reNext != next) {
        // Someone else advanced the ring (reboot/console takeover). Do not chase it:
        // writing at a position we did not reserve would race the host.
        IOLog(VGPU_FB_TAG ": fifo submit refused: NEXT_CMD moved %u -> %u under us\n",
              next, reNext);
        return false;
    }
    const UInt32 regCaps = svgaReadRegister(_svgaPortBase, kSvgaRegFifoCaps);
    const bool canReserve = (regCaps & kSvgaFifoCapReserve) != 0;
    const IOByteCount bytesToEnd = (IOByteCount)(fmax - next);

    if (byteCount <= bytesToEnd) {
        // Fits before FIFO_MAX: the simple case.
        if (canReserve) {
            fifo[kSvgaFifoReserved] = byteCount;
        }
        for (UInt32 i = 0; i < wordCount; i++) {
            fifo[(next / sizeof (UInt32)) + i] = words[i];
        }
        __asm__ volatile ("" ::: "memory");
        // Upstream folds a NEXT_CMD that lands exactly on max back to min
        // (vmw_fifo_commit: "if (next_cmd >= max) next_cmd -= max - min;"). Leaving it
        // AT max is legal per the spec but makes the next reservation's
        // (max - next) term zero, so do it the way the working driver does.
        fifo[kSvgaFifoNextCmd] = (next + (UInt32)byteCount >= fmax)
                                 ? (fmin + (next + (UInt32)byteCount - fmax))
                                 : (next + (UInt32)byteCount);
        __asm__ volatile ("" ::: "memory");
        if (canReserve) {
            fifo[kSvgaFifoReserved] = 0;
        }
        // The doorbell. vmw_local_fifo_commit() writes SVGA_REG_SYNC here unconditionally;
        // without it the host has no reason to inspect NEXT_CMD and the whole stream sits
        // unread in memory while every caller waits for a fence that cannot arrive.
        svgaPingHost(_svgaPortBase);
        return true;
    }

    // Would straddle FIFO_MAX. Pad the tail with SVGA_CMD_INVALID_CMD (0) dwords, wrap
    // NEXT_CMD to FIFO_MIN, and write the command there. Nothing is written after
    // advancing NEXT_CMD, so the host never observes a half-written command; the pad
    // dwords only become visible when NEXT_CMD finally sweeps past them.
    if (canReserve) {
        fifo[kSvgaFifoReserved] = 0;
    }
    const UInt32 padDwords = (UInt32)(bytesToEnd / sizeof (UInt32));
    for (UInt32 i = 0; i < padDwords; i++) {
        fifo[(next / sizeof (UInt32)) + i] = 0;   // SVGA_CMD_INVALID_CMD
    }
    for (UInt32 i = 0; i < wordCount; i++) {
        fifo[(fmin / sizeof (UInt32)) + i] = words[i];
    }
    __asm__ volatile ("" ::: "memory");
    fifo[kSvgaFifoNextCmd] = fmin + (UInt32)byteCount;
    __asm__ volatile ("" ::: "memory");
    svgaPingHost(_svgaPortBase);
    return true;
}

bool vgpuFramebuffer::fenceAck(volatile UInt32 *fifo, UInt32 *seq) {
    *seq = *seq + 1;
    UInt32 words[2] = { kSvgaCmdFence, *seq };
    if (!fifoSubmitWords(fifo, words, 2)) {
        return false;
    }
    // Bounded the same way as fenceTest(): 2 s of wall clock, not a spin count, so a wedged
    // device costs a known amount of boot time instead of an unbounded hang.
    const UInt32 kSteps = 2000;
    for (UInt32 i = 0; i < kSteps; i++) {
        if (fifo[kSvgaFifoFence] == *seq) {
            return true;
        }
        IODelay(1000);
    }
    IOLog(VGPU_FB_TAG ": fence %u NOT acked in %u us (next_cmd %u stop %u fence 0x%x)\n",
          *seq, kSteps, fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop], fifo[kSvgaFifoFence]);
    return false;
}

//
// Guest-backed object bring-up.
//
// Lays out one contiguous working buffer as [page tables | object tables | data], points
// the host's MOB and surface object tables at it, then defines a MOB and a GB surface.
// Every command is fence-verified. This is the state a real driver's object model grows
// out of: contexts, shaders and render targets all live in the same tables.
//
// The SVGA3D bodies are assembled into dword streams with memcpy because they are packed
// structures; x86 tolerates the unaligned stores, but memcpy keeps the layout explicit.
//
// kGbStage gates how far the bring-up goes, because the display side effects of each GB
// step are not yet understood:
//   0 = nothing GB at all (fence test only)
//   1 = register the object-table bases
//   2 = also define the data MOB
//   3 = also define the GB surface
//   4 = also the DX context table and DX_DEFINE_CONTEXT
//   5 = also the STDU present path (stduBringUp)
// (The value itself now lives next to kFbStage near the top of the file.)
//
void vgpuFramebuffer::gbBringUp(volatile UInt32 *fifo) {
    if (_gbMem != nullptr) {
        return;   // already up (kext start runs once, but stay idempotent)
    }
    if (kGbStage == 0) {
        IOLog(VGPU_FB_TAG ": gb: disabled (kGbStage=0)\n");
        return;
    }

    _gbMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        kGbWorkingBytes, 0xFFFULL);
    if (_gbMem == nullptr) {
        IOLog(VGPU_FB_TAG ": gb: could not allocate the %llu-byte working buffer\n",
              (unsigned long long)kGbWorkingBytes);
        return;
    }
    _gbLen = _gbMem->getLength();
    _gbVirt = (void *)_gbMem->map()->getVirtualAddress();
    IOVirtualAddress phys = 0;
    IOByteCount segLen = 0;
    phys = _gbMem->getPhysicalSegment(0, &segLen);
    if (phys == 0 || segLen < _gbLen) {
        IOLog(VGPU_FB_TAG ": gb: working buffer is not contiguous (%llu of %llu)\n",
              (unsigned long long)segLen, (unsigned long long)_gbLen);
        _gbMem->release();
        _gbMem = nullptr;
        return;
    }
    _gbPhys = (UInt64)phys;
    bzero(_gbVirt, _gbLen);
    IOLog(VGPU_FB_TAG ": gb: working buffer phys 0x%llx len %llu\n",
          (unsigned long long)_gbPhys, (unsigned long long)_gbLen);

    const UInt64 pagePpn = _gbPhys >> kGbPageShift;
    volatile UInt64 *ptOtable  = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtOtable);
    volatile UInt64 *ptSurface = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtSurface);
    volatile UInt64 *ptData    = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtData);
    volatile UInt64 *ptDxCtx   = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtDxCtx);

    // Page tables: PPN64 of each 4 KiB page in the region each table describes.
    const IOByteCount mobTableBytes    = (IOByteCount)kGbMobTableEntries * kGbMobEntryBytes;
    const IOByteCount surfaceTableBytes = (IOByteCount)kGbSurfaceTableEnts * kGbSurfaceEntryBytes;
    const IOByteCount dxCtxTableBytes   = (IOByteCount)kGbDxCtxTableEnts * kGbDxCtxEntryBytes;
    const IOByteCount dataBytes         = (IOByteCount)kGbDataPages * kGbPageBytes;
    const UInt32 mobTablePages    = (UInt32)(mobTableBytes / kGbPageBytes);
    const UInt32 surfaceTablePages = 1;
    for (UInt32 i = 0; i < mobTablePages; i++) {
        ptOtable[i] = pagePpn + ((kGbOffMobTable / kGbPageBytes) + i);
    }
    for (UInt32 i = 0; i < surfaceTablePages; i++) {
        ptSurface[i] = pagePpn + ((kGbOffSurfaceTable / kGbPageBytes) + i);
    }
    for (UInt32 i = 0; i < kGbDataPages; i++) {
        ptData[i] = pagePpn + ((kGbOffData / kGbPageBytes) + i);
    }
    // One page holds the DX context table (256 * 8 = 2 KiB).
    ptDxCtx[0] = pagePpn + (kGbOffDxCtxTable / kGbPageBytes);
    __asm__ volatile ("" ::: "memory");

    const UInt64 ppnOtablePt  = pagePpn + (kGbOffPtOtable  >> kGbPageShift);
    const UInt64 ppnSurfacePt = pagePpn + (kGbOffPtSurface >> kGbPageShift);
    const UInt64 ppnDataPt    = pagePpn + (kGbOffPtData    >> kGbPageShift);
    const UInt64 ppnDxCtxPt   = pagePpn + (kGbOffPtDxCtx   >> kGbPageShift);
    const UInt32 dataMobId    = 1;
    const UInt32 surfaceId    = 1;
    const UInt32 dxCtxId      = 1;

    // SET_OTABLE_BASE64: {type, baseAddress PPN64, sizeInBytes, validSizeInBytes, ptDepth}
    // 16 dwords covers every command built here; DEFINE_GB_SURFACE needs 12 and an earlier
    // revision declared 10 -- a two-dword stack overflow that corrupted adjacent locals and
    // froze boots nondeterministically. Keep the bound generous.
    UInt32 words[16];

    // Context MOB for the DX context (hypothesis: the DXCONTEXT otable entry needs a valid
    // mobid, not 0). One 4 KiB page; PT64_0 means the base page IS the data page.
    // BISECT 2026-10-07: with this block (or the destroys below) in the stream, the host
    // freezes at DEFINE_GB_SCREENTARGET even on a fresh power-cycled MKS -- so the extra
    // commands themselves poison it. Disabled pending one-variable-at-a-time bisection.
    if (false) _ctxPage = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_ctxPage != nullptr) {
        bzero((void *)_ctxPage->map()->getVirtualAddress(), kGbPageBytes);
        IOByteCount ctxLen = 0;
        IOVirtualAddress ctxAddr = _ctxPage->getPhysicalSegment(0, &ctxLen);
        if (ctxAddr != 0) {
            const UInt64 ctxPpn = (UInt64)ctxAddr >> kGbPageShift;
            words[0] = kSvga3dCmdDefineGbMob64;
            words[1] = 20;
            words[2] = _ctxMobId;
            words[3] = kSvga3dMobFmtPt64_0;
            memcpy(&words[4], &ctxPpn, 8);
            words[6] = kGbPageBytes;
            if (fifoSubmitWords(fifo, words, 7)) {
                fenceAck(fifo, &_fenceSeq);
                // DXCONTEXT otable entry {cid, mobid} -- written BEFORE DX_DEFINE_CONTEXT
                // (the host snapshots entries when it processes the define).
                volatile UInt8 *dxTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);
                volatile UInt8 *dxEntry = dxTable + (IOByteCount)1 * kGbDxCtxEntryBytes;
                memcpy((void *)(dxEntry + 0), &dxCtxId, 4);
                memcpy((void *)(dxEntry + 4), &_ctxMobId, 4);
                __asm__ volatile ("" ::: "memory");
                IOLog(VGPU_FB_TAG ": gb: context MOB %u defined (ppn %llu), DXCONTEXT entry "
                      "written\n", _ctxMobId, (unsigned long long)ctxPpn);
            }
        }
    }
    words[0] = kSvga3dCmdSetOtableBase64;
    words[1] = 24;   // body size in bytes
    words[2] = kSvgaOtableMob;
    memcpy(&words[3], &ppnOtablePt, 8);
    words[5] = mobTableBytes;
    words[6] = 0;
    // PT64_1: the base page is a page table whose entries are the data pages.
    // (10-21: this used to say PT64_0 -- "base page IS the data page" -- while
    // passing the PT page, so the host read PPN numbers as table entries. The
    // DXCONTEXT entry read validates the mobid through this table, which is why
    // every nonzero mobid failed there.)
    words[7] = kSvga3dMobFmtPt64_1;
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: MOB object table base refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: MOB otable base set (ppn %llu, %u bytes)\n",
          (unsigned long long)ppnOtablePt, mobTableBytes);

    words[2] = kSvgaOtableSurface;
    memcpy(&words[3], &ppnSurfacePt, 8);
    words[5] = surfaceTableBytes;
    words[6] = 0;
    words[7] = kSvga3dMobFmtPt64_1;   // same base-is-a-PT-page fix as the MOB table
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: SURFACE object table base refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: SURFACE otable base set (ppn %llu, %u bytes)\n",
          (unsigned long long)ppnSurfacePt, surfaceTableBytes);

    // The guest writes its half of the MOB entry before defining it.
    if (kGbStage >= 2) {
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = dataMobId * kGbMobEntryBytes;
    // §38c: upstream vmw_mob_pt_populate() decides the depth from the page count --
    //   if (num_data_pages == 1) pt_level = PTDEPTH_0, pt_root = data page;
    //   else                     build a PT page, pt_root = that PT page.
    // kGbDataPages is 4, so PT64_0 here meant the host took ppnDataPt (a page full of
    // PPN64 values) as the first data page. Nothing crashed -- the MOB simply resolved
    // to garbage, and every surface bound on it came back as zeroes. PT64_1 is the
    // only legal encoding for >1 page; ptData[] is already exactly that page table.
    UInt32 mobDepth = kSvga3dMobFmtPt64_1;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &dataBytes, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &ppnDataPt, 8);
    __asm__ volatile ("" ::: "memory");

    // DEFINE_GB_MOB64: {mobid, ptDepth, base PPN64, sizeInBytes}
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = dataMobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &ppnDataPt, 8);
    words[6] = dataBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DEFINE_GB_MOB64 refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: data MOB %u defined (pt ppn %llu, %u bytes, PT64_1)\n",
          dataMobId, (unsigned long long)ppnDataPt, dataBytes);
    }

    // DEFINE_GB_SURFACE: {sid, surfaceFlags(32), format, numMipLevels, multisampleCount,
    //                     autogenFilter, size{w,h,d}} -- 36 bytes (§45).
    //
    // §45: this said 40 and carried an extra trailing "depth" dword. SVGA3dSize already
    // contains depth, so the body is 9 dwords, not 10. Sending 40 made the host's parser
    // start the next command 4 bytes late -- commands were still "accepted" because the
    // status follows the FIFO write, not the field-by-field sanity check. The comment that
    // used to sit here claimed the 64-bit layout and the 40-byte size were what the host
    // wanted; the authoritative header says otherwise, and vmwgfx's own V1 branch proves it.
    if (kGbStage >= 3) {
    const UInt64 surfaceFlags = kSvga3dSurfaceHintDynamic | kSvga3dSurfaceHintTexture;
    const UInt32 width = 256, height = 256;

    defineGbSurfaceFrame(words, surfaceId, surfaceFlags, kSvga3dSurfaceFormatX8R8G8B8,
                         width, height);
    if (!fifoSubmitWords(fifo, words, kSvgaDefineGbSurfaceWords) ||
        !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DEFINE_GB_SURFACE refused\n");
        return;
    }

    // The guest fills the surface's OTable entry too (§45: authoritative field offsets).
    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)surfaceId * kGbSurfaceEntryBytes;
    surfaceEntryFill(entry, kSvga3dSurfaceFormatX8R8G8B8, surfaceFlags,
                     width, height, dataMobId, width * 4, 0);
    __asm__ volatile ("" ::: "memory");

    IOLog(VGPU_FB_TAG ": gb: GB SURFACE %u defined (%ux%u X8R8G8B8 on MOB %u) -- "
          "GB object model is UP\n", surfaceId, width, height, dataMobId);
    }  // kGbStage >= 3

    // ------------------------------------------------------------------
    // DX (vGPU10) context. Gated on the DXCONTEXT device capability: the
    // DX command set is only legal when the host advertises it.
    // ------------------------------------------------------------------
    if (kGbStage >= 4) {
    const UInt32 devcapDx  = devcapRead(kSvgaDevcapDxContext);
    const UInt32 devcapSm41 = devcapRead(kSvgaDevcapSm41);
    const UInt32 devcapGl43 = devcapRead(kSvgaDevcapGl43);
    IOLog(VGPU_FB_TAG ": gb: devcaps DXCONTEXT=%u SM41=%u GL43=%u\n",
          devcapDx, devcapSm41, devcapGl43);
    if (devcapDx == 0) {
        IOLog(VGPU_FB_TAG ": gb: host does not advertise DXCONTEXT; skipping DX context\n");
        return;
    }

    // SVGA_3D_CMD_SET_OTABLE_BASE64 for SVGA_OTABLE_DXCONTEXT.
    //
    // 10-21: format PT64_0 means "the base page IS the data page" (MOBFMT_PT64_0
    // == 4, no page table). This used to register the PT page (offset 0xC000)
    // instead of the table page (0xD000), so the host read PPN numbers as entries:
    // DEFINE_CONTEXT saw entry[1] != -1, reset it to {-1,-1} *inside the PT page*
    // (which is why the reset was invisible in our table page) and BIND's otable
    // fallback then found {-1,-1} -> COMMAND_ERROR at offset 12. Same class of bug
    // may exist for the MOB/SURFACE otables, but those are proven irrelevant
    // (their DEFINE commands carry everything in the command body), so this change
    // is DXCONTEXT-only on purpose -- one variable at a time.
    const UInt64 ppnDxCtxData = pagePpn + (kGbOffDxCtxTable >> kGbPageShift);
    words[0] = kSvga3dCmdSetOtableBase64;
    words[1] = 24;
    words[2] = kSvgaOtableDxContext;
    // 10-22: back to the exact configuration that produced the one accepted
    // DEFINE (23:48) -- PT page base, PT64_0, size 2048 -- to isolate the index.
    memcpy(&words[3], &ppnDxCtxPt, 8);
    // vmwgfx (vmw_otables_setup -> vmw_setup_otable_base) PFN_ALIGNs the size
    // before handing it to the host: a 2 KiB table is registered as 4096.
    words[5] = dxCtxTableBytes;
    words[6] = 0;
    words[7] = kSvga3dMobFmtPt64_0;
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DXCONTEXT object table base refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: DXCONTEXT otable base set (pt ppn %llu, %u bytes, PT64_0)\n",
          (unsigned long long)ppnDxCtxPt, dxCtxTableBytes);

    // Context state MOB: the DX context only becomes live once its state mob is bound
    // with DX_BIND_CONTEXT. 512 KiB scattered (the SVGADXContextMobFormat state block
    // is large), PT64_1 (one PT page holds all 128 PPNs).
    _ctxPage = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kGbPageBytes * 128, 0xFFFULL);
    if (_ctxPage == nullptr) {
        IOLog(VGPU_FB_TAG ": gb: could not allocate the context MOB\n");
        return;
    }
    bzero((void *)_ctxPage->map()->getVirtualAddress(), kGbPageBytes * 128);
    _dxPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_dxPtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": gb: could not allocate the context PT page\n");
        return;
    }
    IOByteCount ctxLen = 0;
    IOVirtualAddress ptAddr = _dxPtMem->getPhysicalSegment(0, &ctxLen);
    if (ptAddr == 0) {
        IOLog(VGPU_FB_TAG ": gb: context PT page not resolvable\n");
        return;
    }
    volatile UInt64 *ctxPt = (volatile UInt64 *)_dxPtMem->map()->getVirtualAddress();
    for (UInt32 i = 0; i < 128; i++) {
        IOVirtualAddress addr = _ctxPage->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &ctxLen);
        if (addr == 0) {
            IOLog(VGPU_FB_TAG ": gb: context MOB page %u not resolvable\n", i);
            return;
        }
        ctxPt[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 ctxPtPpn = (UInt64)ptAddr >> kGbPageShift;

    // DXCONTEXT otable entry {cid, mobid} -- BEFORE the define (host snapshots).
    volatile UInt8 *dxEntryBase = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);
    volatile UInt8 *dxEntry = dxEntryBase + (IOByteCount)dxCtxId * kGbDxCtxEntryBytes;
    memcpy((void *)(dxEntry + 0), &dxCtxId, 4);
    memcpy((void *)(dxEntry + 4), &kCtxMobId, 4);
    __asm__ volatile ("" ::: "memory");

    // DEFINE_GB_MOB64 for the context state mob.
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = kCtxMobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &ctxPtPpn, 8);
    words[6] = kGbPageBytes * 128;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: context MOB define refused\n");
        return;
    }

    // DX_DEFINE_CONTEXT + DX_BIND_CONTEXT are submitted through the command buffer
    // (see dxReadbackTest): DX commands in the legacy FIFO are accepted but ignored.
    IOLog(VGPU_FB_TAG ": gb: DX context state MOB %u ready (define+bind deferred to CB)\n",
          kCtxMobId);
    }  // kGbStage >= 4

    // ------------------------------------------------------------------
    // STDU present path (kGbStage >= 5): a full-display surface living in a
    // 3 MB PT64_1 MOB, bound to a GB screen target. The present loop copies
    // the framebuffer (VRAM) into the surface memory and issues
    // UPDATE_GB_SCREENTARGET; the host reads the surface data straight out
    // of guest RAM.
    // ------------------------------------------------------------------
    if (kGbStage >= 5) {
        stduBringUp(fifo);
    }
}

//
// STDU bring-up. Sequence (each step fence-verified):
//
//   1. allocate the 3 MB backing store (pages may be scattered) and 16 KiB
//      of contiguous PT pages (root + 2 second-level);
//   2. DEFINE_GB_MOB64 mobid 2, PT64_1, covering the backing store;
//   3. register the SCREENTARGET object table (type 4) and write entry 0
//      {image sid 2, size, root, PRIMARY};
//   4. DEFINE_GB_SURFACE sid 2 (1024x768 X8R8G8B8) on mobid 2;
//   5. DEFINE_GB_SCREENTARGET stid 0 (primary);
//   6. copy the current framebuffer into the surface memory (first frame);
//   7. BIND_GB_SCREENTARGET {stid 0, sid 2}, then UPDATE_GB_SCREENTARGET.
//
// After this the present loop re-copies and re-updates every interval.
//
//
// stduFlatBringUp -- §38.12: the same screen target, backed by a MOB that needs no
// page table at all.
//
// Everything in the normal path is kept identical -- same sid, same mobid, same stid,
// same surface/STDU otable entries, same DEFINE/BIND/UPDATE sequence -- with exactly one
// difference: the MOB is defined as PT64_0 over a single 4 KiB page, and that page holds
// the surface's bytes directly. The host therefore resolves the surface's first (and only)
// data page straight from the otable entry's basePpn, with no PPN64 table in between.
//
// Why this is the decisive experiment: the guest has proved its page table correct four
// different ways and the host still reads zeros. The table walk is the one step we cannot
// observe from inside the guest, and it is also the one step this arm deletes. So:
//
//   screen shows content -> the table walk was the fault (PPN64 encoding, page-count
//                           rounding, PT depth...). Fix the table, not the present path.
//   screen stays black  -> the present path itself is at fault (the host never samples
//                           the surface we bound). Stop touching page tables.
//
// The surface is declared 1024x1 so the whole thing fits one page (pitch 4096). The screen
// target stays at full panel size, so the host has one real 4096-byte row to place where a
// 1024x512 image would go -- whatever it shows, it is showing real data from our MOB.
//
void vgpuFramebuffer::stduFlatBringUp(volatile UInt32 *fifo) {
    IOLog(VGPU_FB_TAG ": flat: bring-up starting (PT64_0, one 4 KiB page, no page table)\n");

    _flatMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kGbPageBytes, 0xFFFULL);
    if (_flatMem == nullptr) {
        IOLog(VGPU_FB_TAG ": flat: could not allocate the payload page\n");
        return;
    }
    if (_flatMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog(VGPU_FB_TAG ": flat: prepare failed\n");
        _flatMem->release();
        _flatMem = nullptr;
        return;
    }
    _flatPrepared = true;
    _flatVirt = _flatMem->getBytesNoCopy();

    IOByteCount len = 0;
    const IOVirtualAddress phys = _flatMem->getPhysicalSegment(0, &len);
    if (phys == 0 || len < kGbPageBytes) {
        IOLog(VGPU_FB_TAG ": flat: payload page not resolvable\n");
        return;
    }
    const UInt64 flatPpn = (UInt64)phys >> kGbPageShift;
    const UInt32 flatBytes = (UInt32)kGbPageBytes;

    // A recognisable seed pattern, so the first frame is already something other than
    // black even before the present loop starts recolouring it.
    {
        UInt32 *px = (UInt32 *)_flatVirt;
        for (UInt32 i = 0; i < (UInt32)(kGbPageBytes / 4); i++) {
            px[i] = ((i & 32) != 0) ? 0xFFFF00FFu : 0xFF00FFFFu;   // magenta / cyan stripes
        }
        (void)_flatMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);
    }

    UInt32 words[16];

    // 1. MOB otable entry: PT64_0, size one page, basePpn = the payload page itself.
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = _stduMobId * kGbMobEntryBytes;
    UInt32 mobDepth = kSvga3dMobFmtPt64_0;
    UInt32 mobSize = flatBytes;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &mobSize, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &flatPpn, 8);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = _stduMobId;
    words[3] = mobDepth;
    memcpy(&words[4], &flatPpn, 8);
    words[6] = flatBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: DEFINE_GB_MOB64 refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": flat: MOB %u defined (PT64_0, base ppn %llu, %u bytes)\n",
          _stduMobId, (unsigned long long)flatPpn, flatBytes);

    // 2. SCREENTARGET otable base -- same two-level shape as the normal path, unchanged.
    const UInt64 pagePpn = _gbPhys >> kGbPageShift;
    volatile UInt64 *ptSt = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtSt);
    ptSt[0] = pagePpn + (kGbOffStTable / kGbPageBytes);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdSetOtableBase64;
    words[1] = 24;
    words[2] = kSvgaOtableScreenTarget;
    const UInt64 ppnStPt = pagePpn + (kGbOffPtSt >> kGbPageShift);
    memcpy(&words[3], &ppnStPt, 8);
    words[5] = kGbStTableEnts * kGbStEntryBytes;
    words[6] = 0;
    words[7] = kSvga3dMobFmtPt64_0;
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: SCREENTARGET otable base refused\n");
        return;
    }

    // 3. Surface otable entry, then DEFINE_GB_SURFACE 1024x1 X8R8G8B8 on mobid 2.
    const UInt32 flatW = kStduWidth;
    const UInt32 flatH = (UInt32)(kGbPageBytes / 4) / flatW;   // 1

    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)_surfaceId * kGbSurfaceEntryBytes;
    const UInt64 surfaceFlags = kSvga3dSurfaceHintDynamic | kSvga3dSurfaceHintTexture |
                                kSvga3dSurfaceScreentarget;
    UInt64 zero64 = 0;
    surfaceEntryFill(entry, kSvga3dSurfaceFormatX8R8G8B8, surfaceFlags,
                     flatW, flatH, _stduMobId, flatW * 4, 0);
    __asm__ volatile ("" ::: "memory");

    defineGbSurfaceFrame(words, _surfaceId, surfaceFlags, kSvga3dSurfaceFormatX8R8G8B8,
                         flatW, flatH);
    if (!fifoSubmitWords(fifo, words, kSvgaDefineGbSurfaceWords) ||
        !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: DEFINE_GB_SURFACE refused\n");
        return;
    }

    words[0] = kSvga3dCmdBindGbSurface;
    words[1] = 8;
    words[2] = _surfaceId;
    words[3] = _stduMobId;
    if (!fifoSubmitWords(fifo, words, 4) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: BIND_GB_SURFACE refused\n");
        return;
    }
    words[0] = kSvga3dCmdUpdateGbSurface;
    words[1] = 4;
    words[2] = _surfaceId;
    if (!fifoSubmitWords(fifo, words, 3) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: UPDATE_GB_SURFACE refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": flat: surface %u defined %ux%u on mob %u, bound and updated\n",
          _surfaceId, flatW, flatH, _stduMobId);

    // 4. Screen target: otable entry, DEFINE, BIND, UPDATE -- full panel size.
    volatile UInt8 *stTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffStTable);
    volatile UInt8 *stEntry = stTable + (IOByteCount)_stduStid * kGbStEntryBytes;
    UInt32 zero32 = 0;
    memcpy((void *)(stEntry + 0), &_surfaceId, 4);    // image.sid
    memcpy((void *)(stEntry + 4), &zero64, 8);        // image.face, image.mipmap
    memcpy((void *)(stEntry + 12), &kStduWidth, 4);
    memcpy((void *)(stEntry + 16), &kStduSurfHeight, 4);
    memcpy((void *)(stEntry + 20), &zero32, 4);       // xRoot
    memcpy((void *)(stEntry + 24), &zero32, 4);       // yRoot
    memcpy((void *)(stEntry + 28), &kSvgaStflagPrimary, 4);
    memcpy((void *)(stEntry + 32), &zero32, 4);       // dpi
    bzero((void *)(stEntry + 36), 28);                // pad[7]
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdDefineGbScreenTarget;
    words[1] = 28;
    words[2] = _stduStid;
    words[3] = kStduWidth;
    words[4] = kStduSurfHeight;
    words[5] = 0;
    words[6] = 0;
    words[7] = kSvgaStflagPrimary;
    words[8] = 0;
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: DEFINE_GB_SCREENTARGET refused\n");
        return;
    }

    words[0] = kSvga3dCmdBindGbScreenTarget;
    words[1] = 16;
    words[2] = _stduStid;
    words[3] = _surfaceId;
    words[4] = 0;
    words[5] = 0;
    if (!fifoSubmitWords(fifo, words, 6) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: BIND_GB_SCREENTARGET refused\n");
        return;
    }

    words[0] = kSvga3dCmdUpdateGbScreenTarget;
    words[1] = 20;
    words[2] = _stduStid;
    words[3] = 0;
    words[4] = 0;
    words[5] = kStduWidth;
    words[6] = kStduSurfHeight;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flat: UPDATE_GB_SCREENTARGET refused\n");
        return;
    }

    _stduActive = true;
    IOLog(VGPU_FB_TAG ": flat: screen target 0 (%ux%u) bound to surface %u -- "
          "STDU present path is UP, page table NOT involved\n",
          kStduWidth, kStduSurfHeight, _surfaceId);
}

//
// stduFlatReadback -- §38.13: does the HOST hold our pixels, or only look at them?
//
// Everything up to here measures one thing: what the VNC framebuffer shows. That conflates
// two mechanisms inside the host:
//
//   (a) the host's copy of the surface content -- what UPDATE_GB_SCREENTARGET makes it
//       sample, and what READBACK_GB_IMAGE writes back to guest memory, and
//   (b) the encode/scanout path that turns (a) into the VNC framebuffer.
//
// If (a) is empty, the display is empty for an honest reason and the fault is upstream of
// the display. If (a) holds our colour and the screen is still black, then the surface
// pipeline works completely and the fault is in the host's screen-target scanout.
//
// READBACK_GB_IMAGE takes SVGA3dSurfaceImageId {sid, face, mipmap} and writes the surface
// back into the MOB it is bound to. So the check needs a second MOB: read the surface into
// _flatRbMem (its own PT64_0 page) after pushing a known colour, then compare. A zeroed
// destination therefore means the host wrote nothing, not that we read our own page back.
//
void vgpuFramebuffer::stduFlatReadback(volatile UInt32 *fifo) {
    if (_flatVirt == nullptr) {
        return;
    }
    if (_flatRbMem == nullptr) {
        _flatRbMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, kGbPageBytes, 0xFFFULL);
        if (_flatRbMem == nullptr) {
            IOLog(VGPU_FB_TAG ": flatrb: scratch allocation failed\n");
            return;
        }
        if (_flatRbMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            IOLog(VGPU_FB_TAG ": flatrb: scratch prepare failed\n");
            _flatRbMem->release();
            _flatRbMem = nullptr;
            return;
        }
        _flatRbPrepared = true;
        _flatRbVirt = _flatRbMem->getBytesNoCopy();

        IOByteCount len = 0;
        const IOVirtualAddress phys = _flatRbMem->getPhysicalSegment(0, &len);
        if (phys == 0 || len < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": flatrb: scratch page not resolvable\n");
            return;
        }
        const UInt64 rbPpn = (UInt64)phys >> kGbPageShift;
        const UInt32 rbBytes = (UInt32)kGbPageBytes;

        volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
        const UInt32 off = _flatRbMobId * kGbMobEntryBytes;
        UInt32 depth = kSvga3dMobFmtPt64_0;
        UInt32 size = rbBytes;
        memcpy((void *)(mobTable + off), &depth, 4);
        memcpy((void *)(mobTable + off + 4), &size, 4);
        memcpy((void *)(mobTable + off + 8), &rbPpn, 8);
        __asm__ volatile ("" ::: "memory");

        UInt32 def[7];
        def[0] = kSvga3dCmdDefineGbMob64;
        def[1] = 20;
        def[2] = _flatRbMobId;
        def[3] = depth;
        memcpy(&def[4], &rbPpn, 8);
        def[6] = rbBytes;
        if (!fifoSubmitWords(fifo, def, 7) || !fenceAck(fifo, &_fenceSeq)) {
            IOLog(VGPU_FB_TAG ": flatrb: DEFINE_GB_MOB64 refused (mob %u)\n", _flatRbMobId);
            return;
        }
        IOLog(VGPU_FB_TAG ": flatrb: scratch MOB %u ready (PT64_0, ppn %llu)\n",
              _flatRbMobId, (unsigned long long)rbPpn);
    }

    // 1. Put a colour the screen has never shown into the surface, and push it through the
    //    exact same two commands the present loop uses. Anything non-zero coming back can
    //    only have come from this step.
    const UInt32 marker = 0xFF00A5FFu;   // orange, distinct from every phase colour
    {
        UInt32 *px = (UInt32 *)_flatVirt;
        const UInt32 n = (UInt32)(kGbPageBytes / 4);
        for (UInt32 i = 0; i < n; i++) {
            px[i] = marker;
        }
        (void)_flatMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);
    }

    // §45: authoritative SVGA3dCmdUpdateGBImage -- 36-byte body, {sid,face,mipmap} then
    // SVGA3dBox{x,y,z,w,h,d}. It used to be 52 bytes with src/dst fields that do not exist.
    UInt32 img[11];
    img[0] = kSvga3dCmdUpdateGbImage;
    img[1] = 36;
    img[2] = _surfaceId;
    img[3] = 0;                     // face
    img[4] = 0;                     // mipmap
    img[5] = 0;                     // box.x
    img[6] = 0;                     // box.y
    img[7] = 0;                     // box.z
    img[8] = kStduWidth;            // box.w
    img[9] = 1;                     // box.h
    img[10] = 1;                    // box.d
    fifoSubmitWords(fifo, img, 11);

    UInt32 upd[7];
    upd[0] = kSvga3dCmdUpdateGbScreenTarget;
    upd[1] = 20;
    upd[2] = _stduStid;
    upd[3] = 0; upd[4] = 0;
    upd[5] = kStduWidth;
    upd[6] = kStduSurfHeight;
    fifoSubmitWords(fifo, upd, 7);

    // 2. Rebind the same surface onto the scratch MOB. READBACK writes through the current
    //    binding, so the content has to land somewhere other than the page we just wrote.
    UInt32 bind[4];
    bind[0] = kSvga3dCmdBindGbSurface;
    bind[1] = 8;
    bind[2] = _surfaceId;
    bind[3] = _flatRbMobId;
    if (!fifoSubmitWords(fifo, bind, 4) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flatrb: BIND_GB_SURFACE to the scratch MOB refused\n");
        return;
    }

    // 3. Wipe the destination, then ask for the readback, then fence so the compare runs
    //    strictly after the host has finished writing.
    bzero(_flatRbVirt, kGbPageBytes);
    UInt32 rb[5];
    rb[0] = kSvga3dCmdReadbackGbImage;
    rb[1] = 12;                      // sizeof(SVGA3dSurfaceImageId)
    rb[2] = _surfaceId;
    rb[3] = 0;                       // face
    rb[4] = 0;                       // mipmap
    if (!fifoSubmitWords(fifo, rb, 5) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flatrb: READBACK_GB_IMAGE refused\n");
    }
    // kIOMemoryIncoherentIOFlush on x86 is a plain mfence; the constant
    // kIOMemoryIncoherentIOInvalidate does not exist in this SDK, and the flush (which
    // orders both directions on x86) is the portable choice.
    (void)_flatRbMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);
    __asm__ volatile ("mfence" ::: "memory");

    const UInt32 *got = (const UInt32 *)_flatRbVirt;
    UInt32 nonzero = 0, matches = 0, firstNonzero = 0, firstVal = 0;
    const UInt32 n = (UInt32)(kGbPageBytes / 4);
    for (UInt32 i = 0; i < n; i++) {
        if (got[i] != 0) {
            if (nonzero == 0) { firstNonzero = i; firstVal = got[i]; }
            nonzero++;
            if (got[i] == marker) {
                matches++;
            }
        }
    }
    IOLog(VGPU_FB_TAG ": flatrb: host surface 2 -> %u/%u words nonzero, %u match the "
          "marker 0x%08x (first nonzero @%u = 0x%08x) VERDICT %s\n",
          nonzero, n, matches, marker, firstNonzero, firstVal,
          (matches > (n / 2)) ? "*** host HOLDS our pixels ***"
                              : "*** host holds nothing ***");

    // §38.13b control: does READBACK work at all on this host?
    //
    // The verdict above is only evidence if READBACK itself is a working channel. It is
    // cheap to check: the surface is still bound to the scratch MOB, so ask for a readback
    // once more WITHOUT touching the source page in between. Whatever the host writes this
    // time can only come from its own copy -- and if it writes nothing even then, the zero
    // above says nothing about the host's surface and everything about READBACK.
    // The source page still holds `marker`, so a working READBACK would reproduce it.
    UInt32 rb2[5];
    rb2[0] = kSvga3dCmdReadbackGbImage;
    rb2[1] = 12;
    rb2[2] = _surfaceId;
    rb2[3] = 0;
    rb2[4] = 0;
    if (!fifoSubmitWords(fifo, rb2, 5) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": flatrb: control READBACK refused\n");
    }
    (void)_flatRbMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);
    __asm__ volatile ("mfence" ::: "memory");
    {
        UInt32 ctlNonzero = 0, ctlFirst = 0, ctlFirstIdx = 0;
        for (UInt32 i = 0; i < n; i++) {
            if (got[i] != 0) {
                if (ctlNonzero == 0) { ctlFirstIdx = i; ctlFirst = got[i]; }
                ctlNonzero++;
            }
        }
        IOLog(VGPU_FB_TAG ": flatrb: control readback -> %u/%u words nonzero "
              "(first @%u = 0x%08x) -- READBACK %s\n",
              ctlNonzero, n, ctlFirstIdx, ctlFirst,
              (ctlNonzero > 0) ? "is a live channel" : "writes nothing on this host");
    }

    // 4. Hand the surface back to the page the present loop writes, so the next tick is
    //    not presenting through a stale binding.
    bind[3] = _stduMobId;
    fifoSubmitWords(fifo, bind, 4);
    fenceAck(fifo, &_fenceSeq);
}

//
// stduScreenDma -- §38.14: ask the host to hand the screen's content back to us.
//
// §38.13 established that READBACK_GB_IMAGE gives nothing: the host's copy of the surface
// we bound is empty, so there is nothing for READBACK to write out. That left two readings,
// and they point at completely different fixes:
//
//   (a) the host never sampled the surface (so its copy is empty -- READBACK is honest), or
//   (b) the host sampled it, but READBACK is the thing that does not work here.
//
// GB_SCREEN_DMA (1131) separates them, because it does not go through the surface at all:
// it asks the host to write WHAT IT IS CURRENTLY DISPLAYING for a screen into a MOB we
// name. It is a read of the display pipeline, not of a surface.
//
//   non-zero content -> the host is displaying something and can push pixels into guest
//                       memory; whatever is on screen is real, and (b) is the answer.
//   all zero        -> the host's display for this screen is genuinely blank *and* it
//                       writes blanks; the screen really is showing nothing, and the
//                       question becomes why the host's screen has no content, which is
//                       one step further upstream than anything tried so far.
//
void vgpuFramebuffer::stduScreenDma(volatile UInt32 *fifo) {
    const IOByteCount sdBytes = (IOByteCount)kStduWidth * kStduSurfHeight * 4;
    const UInt32 sdPages = (UInt32)(sdBytes / kGbPageBytes);        // 512

    if (_sdMem == nullptr) {
        _sdMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, sdBytes, 0xFFFULL);
        if (_sdMem == nullptr || _sdMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            IOLog(VGPU_FB_TAG ": sd: destination allocation failed\n");
            return;
        }
        _sdPrepared = true;
        _sdVirt = _sdMem->getBytesNoCopy();

        // One page is enough for the page table: 512 entries == 512 pages (PT64_1).
        _sdPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, kGbPageBytes, 0xFFFULL);
        if (_sdPtMem == nullptr || _sdPtMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            IOLog(VGPU_FB_TAG ": sd: page-table allocation failed\n");
            return;
        }
        _sdPtPrepared = true;
        _sdPtVirt = _sdPtMem->getBytesNoCopy();
        bzero(_sdPtVirt, kGbPageBytes);

        volatile UInt64 *pt = (volatile UInt64 *)_sdPtVirt;
        for (UInt32 i = 0; i < sdPages; i++) {
            IOByteCount len = 0;
            const IOVirtualAddress addr =
                _sdMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &len);
            if (addr == 0 || len < kGbPageBytes) {
                IOLog(VGPU_FB_TAG ": sd: destination page %u not resolvable\n", i);
                return;
            }
            pt[i] = (UInt64)addr >> kGbPageShift;
        }
        __asm__ volatile ("" ::: "memory");
        (void)_sdPtMem->performOperation(kIOMemoryIncoherentIOFlush, 0, kGbPageBytes);

        IOByteCount pLen = 0;
        const IOVirtualAddress ptPhys = _sdPtMem->getPhysicalSegment(0, &pLen);
        if (ptPhys == 0 || pLen < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": sd: page-table page not resolvable\n");
            return;
        }
        const UInt64 ptPpn = (UInt64)ptPhys >> kGbPageShift;

        volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
        const UInt32 off = _flatSdMobId * kGbMobEntryBytes;
        UInt32 depth = kSvga3dMobFmtPt64_1;
        UInt32 size = (UInt32)sdBytes;
        memcpy((void *)(mobTable + off), &depth, 4);
        memcpy((void *)(mobTable + off + 4), &size, 4);
        memcpy((void *)(mobTable + off + 8), &ptPpn, 8);
        __asm__ volatile ("" ::: "memory");

        UInt32 def[7];
        def[0] = kSvga3dCmdDefineGbMob64;
        def[1] = 20;
        def[2] = _flatSdMobId;
        def[3] = depth;
        memcpy(&def[4], &ptPpn, 8);
        def[6] = (UInt32)sdBytes;
        if (!fifoSubmitWords(fifo, def, 7) || !fenceAck(fifo, &_fenceSeq)) {
            IOLog(VGPU_FB_TAG ": sd: DEFINE_GB_MOB64 refused (mob %u)\n", _flatSdMobId);
            return;
        }
        IOLog(VGPU_FB_TAG ": sd: destination MOB %u ready (%u pages, PT64_1, pt ppn %llu)\n",
              _flatSdMobId, sdPages, (unsigned long long)ptPpn);
    }

    bzero(_sdVirt, sdBytes);
    (void)_sdMem->performOperation(kIOMemoryIncoherentIOFlush, 0, sdBytes);

    // SVGA3dCmdGBScreenDMA (svga3d_cmd.h:1467, 20-byte body, 5 dwords):
    //   screenId, dead, destMobID, destPitch, changeMapMobID
    //
    // §45b: this used to submit a 5-dword frame ({cmd, size, stid, dead, mobid}) and then
    // push destPitch as a SEPARATE one-dword submission, with a comment claiming the
    // trailing fields were only needed "if the command needed them". They always are --
    // `size` says 20, so the host consumes exactly the five body dwords and would have
    // read the pitch from wherever the FIFO happened to be pointing. Same class of bug as
    // the 40-vs-36 DEFINE body: the header said more bytes than were framed.
    UInt32 sd[7];
    sd[0] = kSvga3dCmdGbScreenDma;
    sd[1] = 20;                             // body bytes
    sd[2] = _stduStid;                      // screenId
    sd[3] = 0;                              // dead
    sd[4] = _flatSdMobId;                   // destMobID
    sd[5] = kStduWidth * 4;                 // destPitch
    sd[6] = SVGA3D_INVALID_ID_U32;          // changeMapMobID: not wanted
    if (!fifoSubmitWords(fifo, sd, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": sd: GB_SCREEN_DMA refused\n");
        return;
    }

    (void)_sdMem->performOperation(kIOMemoryIncoherentIOFlush, 0, sdBytes);
    __asm__ volatile ("mfence" ::: "memory");

    const UInt32 *got = (const UInt32 *)_sdVirt;
    const UInt32 n = (UInt32)(sdBytes / 4);
    UInt32 nonzero = 0, firstVal = 0, firstIdx = 0;
    for (UInt32 i = 0; i < n; i++) {
        if (got[i] != 0) {
            if (nonzero == 0) { firstIdx = i; firstVal = got[i]; }
            nonzero++;
        }
    }
    IOLog(VGPU_FB_TAG ": sd: GB_SCREEN_DMA stid 0 -> %u/%u words nonzero "
          "(first @%u = 0x%08x) VERDICT %s\n",
          nonzero, n, firstIdx, firstVal,
          (nonzero > 0) ? "*** host dispenses real pixels ***"
                        : "*** host dispenses nothing ***");
}

void vgpuFramebuffer::stduBringUp(volatile UInt32 *fifo) {
    // The 256x256 experiment surface (kGbStage>=3) owns sid 1; the STDU surface takes a
    // fresh id so no redefinition is involved.
    _surfaceId = 2;

    // §38.12: the flat-MOB arm replaces the whole MOB/page-table construction. It is a
    // separate early return rather than a flag threaded through the code below, because
    // the entire point is that none of that machinery runs.
    if (kStduFlat) {
        stduFlatBringUp(fifo);
        return;
    }

    // 1. Backing store (pages may be scattered; the MOB page tables handle that) and page
    //    tables (root + up to two second-level pages; PT64_1 needs only one page total).
    const IOByteCount stduBytes = (IOByteCount)kStduWidth * kStduSurfHeight * 4;
    const UInt32      stduPages = kStduSurfPages;
    const UInt32      ptPageCount = (kStduPt1 == 1) ? 1 : 3;

    // §38f: prepare() + getBytesNoCopy() instead of getVirtualAddress().
    //
    // These buffers are handed to the DEVICE as well as written by the CPU: the PPNs
    // returned by getPhysicalSegment() end up in MOB page tables that the host walks by
    // DMA. Two things have to hold for that to work, and neither did before:
    //
    //   prepare()          pins the page list, so "physical segment N" means the same
    //                      thing at DEFINE time as it did when we recorded the PPN;
    //   getBytesNoCopy()   gives the descriptor's own kernel mapping. map() returns a
    //                      *window* mapping, which is not guaranteed to alias the very
    //                      pages getPhysicalSegment() reports -- writing through it can
    //                      land somewhere the device never reads.
    //
    // That combination produces exactly the symptom we chased through §38a-e: a table
    // the guest can walk perfectly (root[i] == what we wrote) while the host resolves
    // zeroes. Complete everything through one mapping and the CPU/device views agree.
    _stduMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, stduBytes, 0xFFFULL);
    if (_stduMem == nullptr) {
        IOLog(VGPU_FB_TAG ": stdu: could not allocate the %llu-byte backing store\n",
              (unsigned long long)stduBytes);
        return;
    }
    if (_stduMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog(VGPU_FB_TAG ": stdu: backing store prepare failed\n");
        _stduMem->release();
        _stduMem = nullptr;
        return;
    }
    _stduPrepared = true;
    _stduVirt = _stduMem->getBytesNoCopy();
    bzero(_stduVirt, stduBytes);

    _stduPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 3 * kGbPageBytes, 0xFFFULL);
    if (_stduPtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": stdu: could not allocate the page-table pages\n");
        return;
    }
    if (_stduPtMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog(VGPU_FB_TAG ": stdu: page-table prepare failed\n");
        _stduPtMem->release();
        _stduPtMem = nullptr;
        return;
    }
    _stduPtPrepared = true;
    _stduPtVirt = _stduPtMem->getBytesNoCopy();
    bzero(_stduPtVirt, 3 * kGbPageBytes);

    // Root page: PPNs of the two second-level pages (pages 1 and 2 of the PT allocation).
    // PT64_1 has no root: page 0 is the leaf table itself.
    volatile UInt64 *root = (volatile UInt64 *)_stduPtVirt;
    for (UInt32 p = 0; p < ((kStduPt1 == 1) ? 1 : 3); p++) {
        IOVirtualAddress addr = 0;
        IOByteCount len = 0;
        addr = _stduPtMem->getPhysicalSegment((IOByteCount)p * kGbPageBytes, &len);
        if (addr == 0 || len < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": stdu: PT page %u not resolvable\n", p);
            return;
        }
        if (p == 0) {
            _stduPtPhys = (UInt64)addr;
        } else {
            root[p - 1] = (UInt64)addr >> kGbPageShift;
        }
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 rootPpn = _stduPtPhys >> kGbPageShift;

    // Second-level pages: PPN of each backing-store page.
    //
    // §38: the table must be laid out the way the device walks it, and the walk is
    // defined by vmwgfx (vmwgfx_mob.c:vmw_mob_pt_setup / vmw_mob_build_pt):
    //
    //     num_pt_pages = DIV_ROUND_UP(num_data_pages * VMW_PPN_SIZE, PAGE_SIZE)
    //
    // One second-level page holds PAGE_SIZE / 8 = 512 entries, so at 768 data pages we
    // need TWO of them, and the data pages are laid out contiguously across them:
    // L1page[n][i] describes data page n * 512 + i. The previous code wrote all 768
    // entries starting at second-level page 0 -- 6144 bytes into a 4096-byte page, so
    // entries 512..767 spilled over into the other second-level page and clobbered its
    // first 256 slots. The guest-side probe below only sampled pages 0 / 384 / 767
    // through that same wrong base, so it agreed with itself and still reported OK.
    const UInt32 ptEntsPerPage = (UInt32)(kGbPageBytes / 8);          // 512
    const UInt32 ptLvl1Pages  = (kStduPt1 == 1)
                                ? 0
                                : (stduPages + ptEntsPerPage - 1) / ptEntsPerPage;
    for (UInt32 i = 0; i < stduPages; i++) {
        IOVirtualAddress addr = 0;
        IOByteCount len = 0;
        addr = _stduMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &len);
        if (addr == 0 || len < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": stdu: backing-store page %u not resolvable\n", i);
            return;
        }
        if (kStduPt1 == 1) {
            // Single level: page 0 IS the leaf table.
            root[i] = (UInt64)addr >> kGbPageShift;
        } else {
            const UInt32 lvl = i / ptEntsPerPage;
            const UInt32 idx = i % ptEntsPerPage;
            volatile UInt64 *l1 = (volatile UInt64 *)((char *)_stduPtVirt +
                                                     (IOByteCount)(lvl + 1) * kGbPageBytes);
            l1[idx] = (UInt64)addr >> kGbPageShift;
        }
    }
    __asm__ volatile ("" ::: "memory");
    // §38f: make the table visible to the device before the DEFINE tells the host to
    // walk it. A plain compiler barrier only orders the CPU's own accesses; the host
    // reads these pages by DMA. kIOMemoryIncoherentIOFlush is exactly the "store to
    // memory and push it through every cache level" primitive (IOMemoryDescriptor.h:
    // "may be used for non-coherent I/O ... It is NOT required for PCI coherent
    // operations"). It requires the descriptor to have been prepared, which is why the
    // prepare() above matters; on x86 it is a cheap no-op.
    (void)_stduPtMem->performOperation(kIOMemoryIncoherentIOFlush, 0, 3 * kGbPageBytes);
    __asm__ volatile ("mfence" ::: "memory");

    const UInt32 stduMobDepth = (kStduPt1 == 1) ? kSvga3dMobFmtPt64_1
                                                : kSvga3dMobFmtPt64_2;
    IOLog(VGPU_FB_TAG ": stdu: backing store %llu bytes (%u pages, %ux%u) at pt ppn %llu "
          "(PT64_%u)\n",
          (unsigned long long)stduBytes, stduPages, kStduWidth, kStduSurfHeight,
          (unsigned long long)rootPpn, kStduPt1);

    // §38: prove the table resolves, from the guest side, before any command is
    // submitted -- walking it the way the device does: root[lvl] -> second level page ->
    // entry[idx], with idx = page % 512 and lvl = page / 512 (lvl is always 0 and the
    // page itself is the leaf when kStduPt1 == 1).
    {
        volatile UInt64 *r = (volatile UInt64 *)_stduPtVirt;
        const UInt32 probePages[4] = { 0, (stduPages > 1) ? 1u : 0u,
                                       stduPages / 2, stduPages - 1 };
        UInt32 bad = 0;
        for (UInt32 i = 0; i < 4; i++) {
            const UInt32 pg = probePages[i];
            IOByteCount len = 0;
            IOVirtualAddress want = _stduMem->getPhysicalSegment(
                (IOByteCount)pg * kGbPageBytes, &len);
            const UInt64 wantPpn = (UInt64)want >> kGbPageShift;
            const UInt32 lvl = pg / ptEntsPerPage;
            const UInt32 idx = pg % ptEntsPerPage;
            // Map root[lvl] (a PPN) back to a virtual address by comparing against the
            // page-table pages we actually allocated -- they are not guaranteed to be
            // physically contiguous, so offset arithmetic on PPNs is not valid.
            volatile UInt64 *l2 = nullptr;
            if (kStduPt1 == 1) {
                l2 = root;                      // the leaf table is page 0 itself
            } else {
                for (UInt32 p = 1; p < 3; p++) {
                    IOVirtualAddress ptAddr = _stduPtMem->getPhysicalSegment(
                        (IOByteCount)p * kGbPageBytes, &len);
                    if (ptAddr != 0 && ((UInt64)ptAddr >> kGbPageShift) == r[lvl]) {
                        l2 = (volatile UInt64 *)((char *)_stduPtVirt +
                                                 (IOByteCount)p * kGbPageBytes);
                        break;
                    }
                }
            }
            const UInt64 l1Ent = (l2 != nullptr) ? l2[idx] : 0;
            const bool ok = (want != 0) && (l2 != nullptr) && (l1Ent == wantPpn);
            if (!ok) {
                bad++;
            }
            IOLog(VGPU_FB_TAG ": pt: page %u (lvl %u idx %u) want ppn %llu, got %llu %s\n",
                  pg, lvl, idx, (unsigned long long)wantPpn, (unsigned long long)l1Ent,
                  ok ? "OK" : "BROKEN");
        }
        IOLog(VGPU_FB_TAG ": pt: VERDICT %s (root[0]=%llu root[1]=%llu, %u lvl1 pages)\n",
              (bad == 0) ? "*** page table resolves ***" : "*** page table is BROKEN ***",
              (unsigned long long)r[0], (unsigned long long)r[1], ptLvl1Pages);
    }

    UInt32 words[16];

    // The MOB's OTable entry FIRST -- the host snapshots the entry when it processes the
    // DEFINE, so a zero entry (write-after-define) gives it an empty object.
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = _stduMobId * kGbMobEntryBytes;
    UInt32 mobDepth = stduMobDepth;
    UInt32 mobSize = (UInt32)stduBytes;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &mobSize, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &rootPpn, 8);
    __asm__ volatile ("" ::: "memory");

    // 2. DEFINE_GB_MOB64 for the backing store.
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = _stduMobId;
    words[3] = stduMobDepth;
    memcpy(&words[4], &rootPpn, 8);
    words[6] = (UInt32)stduBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_MOB64 refused\n");
        return;
    }

    // 2b. UPDATE_GB_MOB_MAPPING (§38): tell the host to (re)read this MOB's page tables.
    //
    // The guest-side probe above proves root -> L1 -> page resolves correctly, yet the
    // host behaves as if mobid 2 maps to nothing (READBACK leaves a zeroed buffer zero).
    // The one command that explains that gap is the notice that the mapping changed --
    // svga3d_cmd.h describes it as exactly that, and it takes nothing but a mobid.
    words[0] = kSvga3dCmdUpdateGbMobMapping;
    words[1] = 4;
    words[2] = _stduMobId;
    if (!fifoSubmitWords(fifo, words, 3) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: UPDATE_GB_MOB_MAPPING refused\n");
        return;
    }

    // 3. SCREENTARGET object table: PT page at kGbOffPtSt, table at kGbOffStTable.
    const UInt64 pagePpn = _gbPhys >> kGbPageShift;
    volatile UInt64 *ptSt = (volatile UInt64 *)((char *)_gbVirt + kGbOffPtSt);
    ptSt[0] = pagePpn + (kGbOffStTable / kGbPageBytes);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdSetOtableBase64;
    words[1] = 24;
    words[2] = kSvgaOtableScreenTarget;
    const UInt64 ppnStPt = pagePpn + (kGbOffPtSt >> kGbPageShift);
    memcpy(&words[3], &ppnStPt, 8);
    words[5] = kGbStTableEnts * kGbStEntryBytes;   // 4096
    words[6] = 0;
    words[7] = kSvga3dMobFmtPt64_0;
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: SCREENTARGET otable base refused\n");
        return;
    }

    // 4. The surface's OTable entry FIRST (same snapshot rule as the MOB entry), then
    //    DEFINE_GB_SURFACE sid 2 at full display size on mobid 2.
    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)_surfaceId * kGbSurfaceEntryBytes;
    const UInt64 surfaceFlags = kSvga3dSurfaceHintDynamic | kSvga3dSurfaceHintTexture |
                                kSvga3dSurfaceScreentarget;
    // §45: this used to hand-roll 8-byte surfaceFlags into a 4-byte field. See
    // surfaceEntryFill/defineGbSurfaceFrame at the top of the file for the authoritative
    // layout and for why this one shift invalidated everything downstream of it.
    surfaceEntryFill(entry, kSvga3dSurfaceFormatX8R8G8B8, surfaceFlags,
                     kStduWidth, kStduSurfHeight, _stduMobId, kStduWidth * 4, 0);

    UInt64 zero64 = 0;
    UInt32 zero32 = 0;

    defineGbSurfaceFrame(words, _surfaceId, surfaceFlags, kSvga3dSurfaceFormatX8R8G8B8,
                         kStduWidth, kStduSurfHeight);
    if (!fifoSubmitWords(fifo, words, kSvgaDefineGbSurfaceWords) ||
        !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_SURFACE refused\n");
        return;
    }

    // 4b. BIND_GB_SURFACE -- THE step that was missing (§37).
    //
    // DEFINE_GB_SURFACE only creates an empty surface object; it has no backing
    // store until this command names one. Filling `mobid` into the surface otable
    // entry does NOT do it -- the host learns the binding from this command.
    //
    // §37 found the consequence: SURFACE_DMA with transfer=WRITE_HOST_VRAM ran,
    // fences passed, the FIFO advanced, and not one byte moved (readback into a
    // zeroed scratch buffer came back 0/768 sampled words nonzero). A surface with
    // no backing is exactly what a silently-dropped DMA looks like.
    //
    // SVGA3dCmdBindGBSurface { uint32 sid; SVGAMobId mobid; } -- 8 bytes, cmd 1099.
    words[0] = kSvga3dCmdBindGbSurface;
    words[1] = 8;
    words[2] = _surfaceId;
    words[3] = _stduMobId;
    if (!fifoSubmitWords(fifo, words, 4) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: BIND_GB_SURFACE refused (sid %u -> mob %u)\n",
              _surfaceId, _stduMobId);
        return;
    }
    IOLog(VGPU_FB_TAG ": stdu: surface %u bound to mob %u (BIND_GB_SURFACE)\n",
          _surfaceId, _stduMobId);

    // 4c. UPDATE_GB_SURFACE (§38) -- the step that follows BIND in the reference driver.
    //
    // vmwgfx binds a guest-backed surface like this (vmwgfx_surface.c:vmw_gb_surface_bind):
    //
    //     SVGA_3D_CMD_BIND_GB_SURFACE   {sid, mobid}
    //     SVGA_3D_CMD_UPDATE_GB_SURFACE {sid}          <- when guest_memory_dirty
    //
    // BIND only attaches the MOB; UPDATE_GB_SURFACE is what tells the host "the guest
    // memory behind this surface has changed, re-read all of it". We were sending BIND
    // and then going straight to UPDATE_GB_IMAGE at present time, so the host had a
    // surface pointing at a MOB it had never been told to read -- which is precisely the
    // symptom measured in §37: commands accepted, screens black, readback zero.
    //
    // SVGA3dCmdUpdateGBSurface { uint32 sid } -- 4 bytes, cmd 1102.
    words[0] = kSvga3dCmdUpdateGbSurface;
    words[1] = 4;
    words[2] = _surfaceId;
    if (!fifoSubmitWords(fifo, words, 3) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: UPDATE_GB_SURFACE refused (sid %u)\n", _surfaceId);
        return;
    }
    IOLog(VGPU_FB_TAG ": stdu: UPDATE_GB_SURFACE accepted (sid %u)\n", _surfaceId);

    // 5. The screen target's OTable entry (its `image` field names the content surface --
    //    this is what was missing when the screen stayed black), then
    //    DEFINE_GB_SCREENTARGET stid 0.
    //
    // SVGAOTableScreenTargetEntry (svga3d_cmd.h, packed, 64 bytes):
    //   +0  SVGA3dSurfaceImageId image{sid,face,mipmap}   12
    //   +12 uint32              width                      4
    //   +16 uint32              height                     4
    //   +20 int32               xRoot                      4
    //   +24 int32               yRoot                      4
    //   +28 SVGAScreenTargetFlags flags                    4
    //   +32 uint32              dpi                        4
    //   +36 uint32              pad[7]                    28
    // This layout was already correct; it is spelled out here so the next reader does not
    // have to re-derive it (§45's bug was one field over in the surface table).
    volatile UInt8 *stTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffStTable);
    volatile UInt8 *stEntry = stTable + (IOByteCount)_stduStid * kGbStEntryBytes;
    bzero((void *)stEntry, kGbStEntryBytes);
    memcpy((void *)(stEntry + 0), &_surfaceId, 4);    // image.sid
    memcpy((void *)(stEntry + 4), &zero32, 4);        // image.face
    memcpy((void *)(stEntry + 8), &zero32, 4);        // image.mipmap
    memcpy((void *)(stEntry + 12), &kStduWidth, 4);
    memcpy((void *)(stEntry + 16), &kStduSurfHeight, 4);
    memcpy((void *)(stEntry + 20), &zero32, 4);       // xRoot
    memcpy((void *)(stEntry + 24), &zero32, 4);       // yRoot
    memcpy((void *)(stEntry + 28), &kSvgaStflagPrimary, 4);
    memcpy((void *)(stEntry + 32), &zero32, 4);       // dpi
    __asm__ volatile ("" ::: "memory");

    // STDU objects survive guest reboots, and redefining an existing object poisons the
    // host's FIFO processing (the abort surfaces on the NEXT command). Destroy them all
    // first and ignore the outcome -- on the first boot after a power cycle they simply
    // do not exist yet.
    if (kStduTeardown) {
    words[0] = kSvga3dCmdDestroyGbScreenTarget;
    words[1] = 4;
    words[2] = _stduStid;
    fifoSubmitWords(fifo, words, 3);
    if (!fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DESTROY_GB_SCREENTARGET not acked (first boot?)\n");
    }
    words[0] = kSvga3dCmdSurfaceDestroy;
    words[1] = 4;
    words[2] = _surfaceId;
    fifoSubmitWords(fifo, words, 3);
    if (!fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: SURFACE_DESTROY not acked\n");
    }
    words[0] = kSvga3dCmdDestroyGbMob;
    words[1] = 4;
    words[2] = _stduMobId;
    fifoSubmitWords(fifo, words, 3);
    if (!fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DESTROY_GB_MOB not acked\n");
    }
    }

    if (kStduTopology) {
        // §45: the topology block lives in probeFifo now, BEFORE CONFIG_DONE -- see the
        // comment there. Writing it here, while the host is draining a live FIFO, is what
        // made DEFINE_GB_SCREENTARGET abort this boot. The registers are already declared
        // by the time stduBringUp runs, so all that is left is to log what the host echoes
        // back, which confirms the state survived the FIFO takeover.
        IOLog(VGPU_FB_TAG ": stdu: topology at screen-target time -> id %u primary %u "
              "pos %u,%u size %ux%u\n",
              svgaReadRegister(_svgaPortBase, 35), svgaReadRegister(_svgaPortBase, 36),
              svgaReadRegister(_svgaPortBase, 37), svgaReadRegister(_svgaPortBase, 38),
              svgaReadRegister(_svgaPortBase, 39), svgaReadRegister(_svgaPortBase, 40));
    }

    words[0] = kSvga3dCmdDefineGbScreenTarget;
    words[1] = 28;
    words[2] = _stduStid;
    words[3] = kStduWidth;
    words[4] = kStduSurfHeight;
    words[5] = 0;                   // xRoot
    words[6] = 0;                   // yRoot
    words[7] = kSvgaStflagPrimary;
    words[8] = 0;                   // dpi
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_SCREENTARGET refused\n");
        // Dump the whole command stream for offline diffing: 4 dwords per line.
        const UInt32 dumpStart = fifo[kSvgaFifoMin];
        for (UInt32 o = dumpStart; o < fifo[kSvgaFifoNextCmd]; o += 16) {
            IOLog(VGPU_FB_TAG ": fifo[%4u] %08x %08x %08x %08x\n", o,
                  fifo[o / 4], fifo[o / 4 + 1], fifo[o / 4 + 2], fifo[o / 4 + 3]);
        }
        return;
    }


    // 6. First frame: copy the current framebuffer into the surface memory.
    _vramMap = _aperture->map();
    if (_vramMap == nullptr) {
        IOLog(VGPU_FB_TAG ": stdu: could not map the framebuffer\n");
        return;
    }
    _vramVirt = (void *)_vramMap->getVirtualAddress();
    memcpy(_stduVirt, _vramVirt, (IOByteCount)kStduWidth * kStduSurfHeight * 4);

    // 7. BIND then UPDATE -- the screen target shows surface sid 2 from here on.
    words[0] = kSvga3dCmdBindGbScreenTarget;
    words[1] = 16;
    words[2] = _stduStid;
    words[3] = _surfaceId;
    words[4] = 0;                   // face
    words[5] = 0;                   // mipmap
    if (!fifoSubmitWords(fifo, words, 6) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: BIND_GB_SCREENTARGET refused\n");
        return;
    }

    words[0] = kSvga3dCmdUpdateGbScreenTarget;
    words[1] = 20;
    words[2] = _stduStid;
    words[3] = 0;                   // rect.x
    words[4] = 0;                   // rect.y
    words[5] = kStduWidth;
    words[6] = kStduSurfHeight;     // not kStduHeight: the surface is only 512 rows in PT64_1
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: UPDATE_GB_SCREENTARGET refused\n");
        return;
    }

    _stduActive = true;
    IOLog(VGPU_FB_TAG ": stdu: screen target 0 (%ux%u) bound to surface %u on MOB %u -- "
          "STDU present path is UP\n", kStduWidth, kStduSurfHeight, _surfaceId, _stduMobId);

    // §45: prove in the log which layout this binary actually used, by reading the OTable
    // entry back the way the host would parse it. Six boots in a row were spent reasoning
    // about a field order that was wrong by one dword; a one-line echo makes the next
    // occurrence of that class of bug impossible to miss. If these numbers do not match
    // what was written above, nothing downstream of here means anything.
    {
        UInt32 rFmt = 0, rFlags = 0, rMips = 0, rW = 0, rH = 0, rMob = 0, rPitch = 0;
        memcpy(&rFmt,   (const void *)(entry +  0), 4);
        memcpy(&rFlags, (const void *)(entry +  4), 4);
        memcpy(&rMips,  (const void *)(entry +  8), 4);
        memcpy(&rW,     (const void *)(entry + 20), 4);
        memcpy(&rH,     (const void *)(entry + 24), 4);
        memcpy(&rMob,   (const void *)(entry + 32), 4);
        memcpy(&rPitch, (const void *)(entry + 40), 4);
        IOLog(VGPU_FB_TAG ": stdu: surface otable -> fmt %u flags 0x%08x mips %u %ux%u "
              "mobid %u pitch %u (expect fmt %u flags 0x%08x mips 1 %ux%u mobid %u pitch %u)\n",
              rFmt, rFlags, rMips, rW, rH, rMob, rPitch,
              kSvga3dSurfaceFormatX8R8G8B8, (UInt32)(surfaceFlags & kSvgaSurfaceFlagsMask32),
              kStduWidth, kStduSurfHeight, _stduMobId, kStduWidth * 4);
    }

    // The DX round-trip is deferred to a present tick (~20 s after boot): running it
    // inline at IOFramebuffer start destabilises the guest later (see 2026-10-07 notes).
    // kDxTest gates it; presentTick fires it once at tick 600.
}

//
// DX command-stream round trip, fully self-contained: private 256x256 surface (sid 3) on a
// private 256 KB MOB (mobid 4), RTV 0, clear to magenta through the host 3D engine,
// READBACK_GB_SURFACE, verify the pixels in guest memory. Deliberately independent of the
// display objects so a DX failure cannot take the display down.
//
// =====================================================================
// dxVm3dmpTemplate: build the DX context the way vm3dmp.sys does, then submit
// ONE render command. Split out of dxReadbackTest because keeping both the GB
// experiment and this one in a single frame made clang emit a weak
// ____chkstk_darwin reference, and kmutil rejects a kext with weak refs (Code=31).
//
// The disassembly this follows (imagebase 0x140000000):
//   CreateDXContext 0x140010c78 -- DEFINE and BIND go into ONE CB (helper
//   0x1400095d8(dev, pre=1, ctx=cid, ...) -> 0x14001a76c appends to the CB whose
//   cbContext IS the DX context id), the 11..12 SET_COTABLE calls for the unused
//   types are in that same CB, and every bind carries validSize=0.
// Returns true when the single DEFINE_RTV was accepted, i.e. the host really did
// create the DX manager for us.
// =====================================================================
bool vgpuFramebuffer::dxVm3dmpTemplate(volatile UInt32 *fifo, UInt32 surfId,
                                      UInt32 dxW, UInt32 dxH) {
    // First statement on purpose: if this line is missing from the log the function was
    // never entered, which is a completely different bug from "it bailed out early".
    IOLog(VGPU_FB_TAG ": dx: TEMPLATE entered (sid %u %ux%u)\n", surfId, dxW, dxH);
    UInt32 words[24];
    const UInt32 tcid = 1;
    const UInt32 trtv = 1;
    const UInt32 stateMobId = 6;
    const UInt32 ctxStateBytes = 12288;     // sizeof(SVGADXContextMobFormat)
    _ctxStateMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, ctxStateBytes, 0xFFFULL);
    _ctxStatePtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        kGbPageBytes, 0xFFFULL);
    if (_ctxStateMem == nullptr || _ctxStatePtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": dx: template state MOB alloc failed\n");
        return false;
    }
    bzero((void *)_ctxStateMem->map()->getVirtualAddress(), ctxStateBytes);
    volatile UInt64 *spt = (volatile UInt64 *)_ctxStatePtMem->map()->getVirtualAddress();
    IOByteCount smLen = 0;
    IOVirtualAddress smPt = _ctxStatePtMem->getPhysicalSegment(0, &smLen);
    if (smPt == 0) {
        IOLog(VGPU_FB_TAG ": dx: template state PT page not resolvable\n");
        return false;
    }
    for (UInt32 i = 0; i < ctxStateBytes / kGbPageBytes; i++) {
        IOVirtualAddress addr = _ctxStateMem->getPhysicalSegment(
            (IOByteCount)i * kGbPageBytes, &smLen);
        if (addr == 0) {
            IOLog(VGPU_FB_TAG ": dx: template state MOB page %u not resolvable\n", i);
            return false;
        }
        spt[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 smPtPpn = (UInt64)smPt >> kGbPageShift;
    {
        volatile UInt8 *dxTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);
        volatile UInt8 *dxEntry = dxTable + (IOByteCount)tcid * kGbDxCtxEntryBytes;
        memcpy((void *)(dxEntry + 0), &tcid, 4);
        memcpy((void *)(dxEntry + 4), &stateMobId, 4);
        __asm__ volatile ("" ::: "memory");
    }
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = stateMobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &smPtPpn, 8);
    words[6] = ctxStateBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: template state MOB define refused\n");
        return false;
    }
    IOLog(VGPU_FB_TAG ": dx: template state MOB %u defined (%u B PT64_1)\n",
          stateMobId, ctxStateBytes);

    // COTable per type (0..11, vm3dmp 0x140011178 drives the entry counts).
    //
    // 10-29 21:5x -- THE BUG THAT COST THREE BOOTS. The 4th field of
    // SVGA3dCmdDXSetCOTable is *validSizeInBytes*, NOT a 0/1 "valid" flag:
    //
    //   typedef struct SVGA3dCmdDXSetCOTable {
    //       uint32 cid;
    //       uint32 mobid;
    //       SVGACOTableType type;
    //       uint32 validSizeInBytes;     // ★ here
    //   } SVGA3dCmdDXSetCOTable;
    //
    // We were sending 0 for every table. A zero-byte COTable is a legal request
    // (it is exactly how vmwgfx's vmw_cotable_scrub() *unbinds* a table:
    // `cmd1->body.mobid = SVGA3D_INVALID_ID; cmd1->body.validSizeInBytes = 0;`),
    // so SET_COTABLE came back ACCEPTED and looked like it had worked -- while the
    // RTVIEW table stayed 0 bytes wide. DEFINE_RTV then had nowhere to allocate
    // slot 1 and returned COMMAND_ERROR with errorOffset 0, which is why every
    // payload permutation we tried failed identically: the payload was never the
    // problem. Valid size must not exceed the MOB's own size, and our RTVIEW MOB
    // is one 4 KiB page, so 4096 is both legal and the whole table.
    const UInt32 kCotableValidBytes = kGbPageBytes;   // 4096, == the MOB we defined
    struct { UInt32 type; UInt32 mob; UInt32 validSize; } tc[12] = {
        { 0,  kCotableMobId, kCotableValidBytes },   // RTVIEW: the only one we back
        { 1,  SVGA3D_INVALID_ID_U32, 0 },  // DepthStencilView
        { 2,  SVGA3D_INVALID_ID_U32, 0 },  // SRV
        { 3,  SVGA3D_INVALID_ID_U32, 0 },  // UAV
        { 4,  SVGA3D_INVALID_ID_U32, 0 },  // VB
        { 5,  SVGA3D_INVALID_ID_U32, 0 },  // IB
        { 6,  SVGA3D_INVALID_ID_U32, 0 },  // SO
        { 7,  SVGA3D_INVALID_ID_U32, 0 },  // StreamOut
        { 8,  SVGA3D_INVALID_ID_U32, 0 },  // BlendState
        { 9,  SVGA3D_INVALID_ID_U32, 0 },  // DepthStencilState
        { 10, SVGA3D_INVALID_ID_U32, 0 },  // RasterizerState
        { 11, SVGA3D_INVALID_ID_U32, 0 },  // ElementLayout
    };
    // 8 dwords of DEFINE+BIND plus 12 cotables at 6 dwords each = 80. The array used to
    // be tpl[64], so the last two cotables (and n itself, on this frame) were written
    // past the end -- the log still said "80 dwords" because n had already counted them,
    // and the payload the host saw was whatever the neighbouring stack slots held. Size
    // it from the same arithmetic that fills it, and assert the bound.
    // 8 dwords of DEFINE+BIND plus 12 cotables at 6 dwords each = 80.
    //
    // The batch deliberately DROPS the leading DEFINE+BIND (8 dwords): STEP 1 below has
    // just sent both as single-command CBs. Re-sending them would be the only thing in
    // this CB whose effect we cannot attribute -- a second DX_DEFINE_CONTEXT for a cid
    // that already exists may itself be the COMMAND_ERROR we are chasing.
    //
    // 10-29 22:0x (§32): even that reduced batch turned out to be the poison -- see
    // STEP 2 below, which is now retired. The 12-entry table above still describes
    // what the host should be told; STEP 1 sends the only member that matters (RTVIEW)
    // on its own, and STEP 3a re-binds the others on fresh cids.
    (void)tc;
    (void)trtv;
    // 10-28 23:14: the template finally ran, and this exact CB was NOT refused -- it
    // never COMPLETED. hdr[4] read back 7 = NO_IRQ|DX_CONTEXT|MOB, and vm3dmp.sys
    // never sets NO_IRQ on DX streams (only DX_CONTEXT), so with NO_IRQ the host has
    // no reason to write status = COMPLETED into guest memory and our poll spins out
    // (0x140013a51 is the builder that sets DX_CONTEXT alone). Fire-and-forget the DX
    // CB the way the driver does, then SYNC with one plain (NO_IRQ) CB on the device
    // queue: completion of the plain stream means the DX pipeline took everything
    // before it -- that is how the driver learns its DX work was accepted.
    UInt32 sync[3] = { 0 };
    // ---- STEP 1: one command per CB, on a pristine engine (docs §25.5) ----------
    // This has to come FIRST. A diagnostic re-send after a failed batch is worthless:
    // the first header/command error stops the queue from answering at all (§22.1), so
    // every probe after it just times out -- exactly what the 20:28 boot produced.
    // Sending each command in its own CB up front also tells us WHICH command fails,
    // which the 13-commands-in-one-CB shape never could (errorOffset was always 0).
    UInt32 one[6];
    one[0] = kSvga3dCmdDxDefineContext; one[1] = 4; one[2] = tcid;
    bool okDef = cbSubmitCtx(one, 3, tcid, 0, 1);
    IOLog(VGPU_FB_TAG ": dx: SPLIT DX_DEFINE_CONTEXT {cid %u} -> %s\n",
          tcid, okDef ? "ACCEPTED" : "refused");
    one[0] = kSvga3dCmdDxBindContext; one[1] = 12; one[2] = tcid;
    one[3] = stateMobId; one[4] = 0;
    bool okBind = okDef && cbSubmitCtx(one, 5, tcid, 0, 1);
    IOLog(VGPU_FB_TAG ": dx: SPLIT DX_BIND_CONTEXT {cid %u mob %u} -> %s\n",
          tcid, stateMobId, okBind ? "ACCEPTED" : "refused");
    one[0] = kSvga3dCmdDxSetCotable; one[1] = 16; one[2] = tcid;
    one[3] = tc[0].mob; one[4] = tc[0].type; one[5] = tc[0].validSize;
    bool okCot = okBind && cbSubmitCtx(one, 6, tcid, 0, 1);
    IOLog(VGPU_FB_TAG ": dx: SPLIT DX_SET_COTABLE {cid %u mob %u type %u} -> %s\n",
          tcid, tc[0].mob, tc[0].type, okCot ? "ACCEPTED" : "refused");
    if (!okDef || !okBind || !okCot) {
        // Stop here: the batch below would only measure the wedged queue.
        IOLog(VGPU_FB_TAG ": dx: TEMPLATE skipped (split step failed; engine state unknown)\n");
        return false;
    }

    // ---- STEP 2: RETIRED 10-29 22:0x (§32). ---------------------------------
    //
    // This used to send DEFINE+BIND plus all 12 SET_COTABLEs as one 72-dword CB, as
    // vm3dmp does. It always came back COMPLETED -- and everything submitted AFTER it
    // failed with STATUS 3 / offset 0, including SET_COTABLE itself, on EVERY
    // cbContext (q1, q2, 0x3f). That is the single strongest signal we have: the batch
    // is the only step that hands the host eleven `mob = 0xFFFFFFFF` bindings for one
    // DX context, and a COMPLETED batch can still leave the context permanently
    // unusable -- a status a second command on the same context cannot recover from.
    //
    // STEP 1 above already sends the same three commands one-per-CB and has never
    // failed once in four boots, so the batch adds no information and only poisons the
    // state the later probes are trying to measure. Drop it. Keep the barrier: it tells
    // us the host drained everything STEP 1 sent before we start measuring.
    bool okTpl = true;
    {
        bool okSync = cbSubmitCtx(sync, 0, 0, 0, 1);
        IOLog(VGPU_FB_TAG ": dx: TEMPLATE sync CB (q0 flags1) -> %s\n",
              okSync ? "COMPLETED" : "did not complete");
        okTpl = okSync;
    }
    if (okTpl) {
        // ---- STEP 3: one shot, format = A8R8G8B8 (§34.4). -----------------------
        // §34 established that cbContext is not "which queue carries this CB" but
        // "which queue this DX context is bound to", and that queue 1 is the only queue
        // that ever carried a DX context. So every earlier sweep -- the four-format
        // enumeration (§30), the validSize sweep (§31), the surfaceFlags redefine and
        // the per-variant (cid, queue) pairs (§32/§33) -- was varying a field on a
        // context that can never hold a DX context (STATUS 4 at DEFINE_CONTEXT), or on
        // cid 1 after cid 1 had ALREADY failed one DEFINE_RTV in this same boot.
        //
        // All of them are retired. What is left is the cleanest cell of the matrix we
        // have never tested: the cid STEP 1 just DEFINE'd on queue 1 (tcid), a fresh
        // SET_COTABLE from STEP 1, and a DEFINE_RTV payload identical to STEP 1's
        // except ONE field -- format, from X8R8G8B8 (1) to A8R8G8B8 (2). A8R8G8B8 is
        // what real D3D9/GL swap chains, and vm3dmp's own template, present.
        // Everything else is pinned: dim = 3 (texture2d), desc = {0,0,1}, cmdSize 28,
        // dxContext 0, flags 2.
        //
        // ACCEPTED => X8R8G8B8 was not a legal RTV format; the blocker was the enum,
        //             and the DX path stays open.
        // refused  => format is not the blocker. DEFINE_RTV cannot succeed on this host
        //             at all; freeze that in STATUS.md and fall back to the 2D path
        //             (GB surface + SVGA_3D_CMD_SURFACE_DMA / blit, §31.5-3).
        UInt32 rtv2[9] = { kSvga3dCmdDxDefineRenderTargetView, 28,
                           trtv, surfId, 2 /* A8R8G8B8 */, 3 /* texture2d */, 0, 0, 1 };
        bool okRtv = cbSubmitCtx(rtv2, 9, tcid, 0, 2);
        UInt32 rtvQ = tcid;
        IOLog(VGPU_FB_TAG ": dx: RTV fmt2 A8R8G8B8/t2d {rtv %u sid %u q%u} -> %s\n",
              trtv, surfId, tcid, okRtv ? "ACCEPTED" : "refused");
        if (okRtv) {
            IOLog(VGPU_FB_TAG ": dx: *** DX MANAGER ALIVE (q%u, fmt2 A8R8G8B8/t2d) ***\n",
                  tcid);
        }
        IOLog(VGPU_FB_TAG ": dx: TEMPLATE verdict -> %s\n",
              okRtv ? "*** DX MANAGER ALIVE ***" : "still refused");
        if (!okRtv) {
            IOLog(VGPU_FB_TAG ": dx: TEMPLATE stopping (RTV refused; engine state unknown)\n");
            return false;
        }
        {
            // SET_SHADER_IFACE is its own CB in the driver, on a capability bit. Sent
            // last on purpose: if it errors it must not take the RTV verdict with it.
            UInt32 si[5] = { kSvga3dCmdDxSetShaderIface, 12, tcid, stateMobId, 0x2000 };
            bool okSi = cbSubmitCtx(si, 5, rtvQ, 0, 2);
            IOLog(VGPU_FB_TAG ": dx: TEMPLATE SET_SHADER_IFACE (q%u) -> %s\n",
                  rtvQ, okSi ? "submitted" : "refused");
        }
        if (okRtv) {
            UInt32 clr[8] = { kSvga3dCmdDxClearRenderTargetView, 20, trtv, 0, 0, 0, 0 };
            { float g[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
              memcpy(&clr[3], g, 16); }
            bool okClr = cbSubmitCtx(clr, 7, rtvQ, 0, 2);
            if (okClr) {
                okClr = cbSubmitCtx(sync, 0, 0, 0, 1);
            }
            IOLog(VGPU_FB_TAG ": dx: TEMPLATE CLEAR_RTV -> %s\n",
                  okClr ? "ACCEPTED" : "refused");
            if (okClr) {
                UInt32 rb[4] = { kSvga3dCmdDxReadbackSubResource, 8, surfId, 0 };
                bool okRb = cbSubmitCtx(rb, 4, rtvQ, 0, 2);
                if (okRb) {
                    okRb = cbSubmitCtx(sync, 0, 0, 0, 1);
                }
                IOLog(VGPU_FB_TAG ": dx: TEMPLATE READBACK -> %s\n",
                      okRb ? "ACCEPTED" : "refused");
                __asm__ volatile ("" ::: "memory");
                if (okRb) {
                    UInt32 px = *(volatile UInt32 *)_dxVirt;
                    UInt32 mid = *(volatile UInt32 *)((char *)_dxVirt +
                                  (IOByteCount)(dxW * (dxH / 2) + dxW / 2) * 4);
                    IOLog(VGPU_FB_TAG ": dx: TEMPLATE pixels first=0x%08x mid=0x%08x "
                          "(want 0xff00ff00)\n", px, mid);
                }
            }
            return true;
        }
    }
    return false;
}

// =====================================================================
// dxTemplateSetupSurface: everything the template needs BEFORE the first DX
// command -- the 256x256 surface (mob 4, PT64_1), the RTVIEW COTable mob, the
// MOB-form CB payload page, the surface OTable entry and DEFINE_GB_SURFACE.
// Split out of dxReadbackTest (10-28) because kDxTemplateOnly must run the
// template on a PRISTINE command engine: any earlier DX error wedges the queue
// (docs 22), so this uses the legacy FIFO path only, which is proven working.
// Returns false if any step failed; caller must not submit DX then.
// =====================================================================
bool vgpuFramebuffer::dxTemplateSetupSurface(volatile UInt32 *fifo,
                                             const UInt32 surfId, const UInt32 mobId,
                                             const UInt32 dxW, const UInt32 dxH) {
    const UInt32 dxBytes = dxW * dxH * 4;
    const UInt32 dxPages = dxBytes / kGbPageBytes;
    UInt32 words[24];
    // Surface backing store: 64 pages + PT page (PT64_1).
    _dxMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
    kernel_task, kIODirectionInOut, dxBytes, 0xFFFULL);
    _surfPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
    kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_dxMem == nullptr || _surfPtMem == nullptr) {
    IOLog(VGPU_FB_TAG ": dx: surface memory alloc failed\n");
    return false;
    }
    bzero((void *)_dxMem->map()->getVirtualAddress(), dxBytes);
    _dxVirt = (void *)_dxMem->map()->getVirtualAddress();
    {
    // Discriminator fill: the GB-context CLEAR uses WHITE; if readback returns
    // magenta, CLEAR did not render (or readback is a no-op); if white, the
    // legacy-pipeline render path works end to end.
    volatile UInt32 *fill = (volatile UInt32 *)_dxVirt;
    for (UInt32 i = 0; i < dxW * dxH; i++) {
        fill[i] = 0xFFFF00FF;
    }
    }
    volatile UInt64 *pt = (volatile UInt64 *)_surfPtMem->map()->getVirtualAddress();
    IOByteCount segLen = 0;
    IOVirtualAddress ptAddr = _surfPtMem->getPhysicalSegment(0, &segLen);
    if (ptAddr == 0) {
    IOLog(VGPU_FB_TAG ": dx: surface PT page not resolvable\n");
    return false;
    }
    for (UInt32 i = 0; i < dxPages; i++) {
    IOVirtualAddress addr = _dxMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &segLen);
    if (addr == 0 || segLen < kGbPageBytes) {
        IOLog(VGPU_FB_TAG ": dx: surface page %u not resolvable\n", i);
        return false;
    }
    pt[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 ptPpn = (UInt64)ptAddr >> kGbPageShift;

    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = mobId * kGbMobEntryBytes;
    UInt32 mobDepth = kSvga3dMobFmtPt64_1;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &dxBytes, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &ptPpn, 8);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = mobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &ptPpn, 8);
    words[6] = dxBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
    IOLog(VGPU_FB_TAG ": dx: legacy DEFINE_GB_MOB64 refused\n");
    return false;
    }

    // COTABLE mob for RTVIEW: EXACT per-type size (vm3dmp 10.3/12.4 both):
    // RTVIEW = 32 entries * 4 B = 128 B -> one 4 KiB page, PT64_0. LEGACY define.
    _cotableMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
    kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_cotableMem == nullptr) {
    IOLog(VGPU_FB_TAG ": dx: cotable mob alloc failed\n");
    return false;
    }
    bzero((void *)_cotableMem->map()->getVirtualAddress(), kGbPageBytes);
    IOByteCount ctLen = 0;
    IOVirtualAddress ctAddr = _cotableMem->getPhysicalSegment(0, &ctLen);
    if (ctAddr == 0) {
    IOLog(VGPU_FB_TAG ": dx: cotable page not resolvable\n");
    return false;
    }
    const UInt64 ctPpn = (UInt64)ctAddr >> kGbPageShift;
    UInt32 ctEntryOff = kCotableMobId * kGbMobEntryBytes;
    UInt32 ctDepth0 = kSvga3dMobFmtPt64_0;
    memcpy((void *)(mobTable + ctEntryOff), &ctDepth0, 4);
    memcpy((void *)(mobTable + ctEntryOff + 4), &kGbPageBytes, 4);
    memcpy((void *)(mobTable + ctEntryOff + 8), &ctPpn, 8);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = kCotableMobId;
    words[3] = kSvga3dMobFmtPt64_0;
    memcpy(&words[4], &ctPpn, 8);
    words[6] = kGbPageBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
    IOLog(VGPU_FB_TAG ": dx: legacy cotable DEFINE_GB_MOB64 refused\n");
    return false;
    }
    IOLog(VGPU_FB_TAG ": dx: legacy cotable MOB %u defined (4 KiB PT64_0)\n", kCotableMobId);

    // MOB-form CB payload (see cbMobSetup): kept out of this function so the
    // frame stays under the stack-probe threshold -- adding it inline made clang
    // emit a weak ____chkstk_darwin reference, which kmutil refuses to link into
    // the auxiliary kernel collection (Code=31).
    cbMobSetup(fifo);

    // The surface's OTable entry, then DEFINE_GB_SURFACE via legacy.
    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)surfId * kGbSurfaceEntryBytes;
    UInt64 sflags = kSvga3dSurfaceHintRenderTarget | kSvga3dSurfaceBindRenderTarget;
    // SVGAOTableSurfaceEntry is 64 bytes (svga3d_cmd.h):
    //   format, surface1Flags, numMipLevels, multisampleCount, autogenFilter,
    //   size{width,height,depth}, mobid, arraySize, mobPitch, surface2Flags,
    //   multisamplePattern(u8), qualityLevel(u8), bufferByteStride(u16),
    //   minLOD(float), pad0[2].
    // We used to write a 72-byte layout with a 64-bit flags field, so every entry
    // after sid 0 was misaligned and the DX RTV path read garbage for sid 3.
    //
    // §45: the fix that was applied here by hand is now surfaceEntryFill()/defineGbSurfaceFrame()
    // at the top of the file, shared with the STDU arm -- which still had the old bug.
    surfaceEntryFill(entry, kSvga3dSurfaceFormatX8R8G8B8, sflags,
                     dxW, dxH, mobId, dxW * 4, 0);
    __asm__ volatile ("" ::: "memory");

    defineGbSurfaceFrame(words, surfId, sflags, kSvga3dSurfaceFormatX8R8G8B8, dxW, dxH);
    if (!fifoSubmitWords(fifo, words, kSvgaDefineGbSurfaceWords) ||
        !fenceAck(fifo, &_fenceSeq)) {
    IOLog(VGPU_FB_TAG ": dx: legacy DEFINE_GB_SURFACE refused\n");
    return false;
    }
    IOLog(VGPU_FB_TAG ": dx: legacy DEFINE_GB_SURFACE accepted (sid %u on mob %u)\n",
      surfId, mobId);
    return true;
}
void vgpuFramebuffer::dxReadbackTest(volatile UInt32 *fifo) {
    if (!cbInit()) {
        return;
    }
    // Template-only mode: the CB mechanism is the ONLY thing brought up before the
    // template runs. Every other DX/GB probe in this function ends with the engine
    // wedged (one error = one dead queue), so a template submitted after any of them
    // could never be told apart from "the engine was already dead".
    if (kDxTemplateOnly) {
        const UInt32 tSurf = 3, tMob = 4, tW = 256, tH = 256;
        if (!dxTemplateSetupSurface(fifo, tSurf, tMob, tW, tH)) {
            IOLog(VGPU_FB_TAG ": dx: TEMPLATE only mode aborted (surface setup failed)\n");
            return;
        }
        bool alive = dxVm3dmpTemplate(fifo, tSurf, tW, tH);
        IOLog(VGPU_FB_TAG ": dx: TEMPLATE only mode -> %s\n",
              alive ? "*** DX MANAGER ALIVE ***" : "DX manager still absent");
        return;
    }
    // Bootstrap the CB mechanism itself (device context 0x3f).
    {
        UInt32 dc[3] = { 1, 1, 0 };      // id=START_STOP_CONTEXT, enable=1, context=0
        if (!cbSubmitCtx(dc, 3, 0x3F, 0)) {
            IOLog(VGPU_FB_TAG ": cb: device-context START_STOP failed\n");
            return;
        }
        IOLog(VGPU_FB_TAG ": cb: CB mechanism started (device context)\n");
        // DC cmd 3 {cid}: vm3dmp sends this device command (8-byte body, gated on a
        // devcap bit) around DX context lifetime -- plausibly "register DX context".
        // Sent on the device queue (0x3f) BEFORE any DX CB.
        {
            const UInt32 dcCid = 1;   // cid of the DX chain below (declared later)
            UInt32 dc3[2] = { 3, dcCid };
            bool okdc = cbSubmitCtx(dc3, 2, 0x3F, 0);
            IOLog(VGPU_FB_TAG ": dx: DC cmd 3 {cid %u} -> %s\n",
                  dcCid, okdc ? "ACCEPTED" : "refused");
            // START_STOP sweep on the (non-wedging) device queue. The bootstrap
            // above proves {cmd 1, 1, 0} works, and {cmd 1, 1, 1} does not -- so
            // the last dword selects the queue/context and id 1 is not servable
            // yet. Sweep 0..3 to see which ids exist after DC cmd 3.
            for (UInt32 q = 0; q < 4; q++) {
                UInt32 startQ[3] = { 1, 1, q };
                bool okq = cbSubmitCtx(startQ, 3, 0x3F, 0);
                IOLog(VGPU_FB_TAG ": dx: START_STOP {enable 1, id %u} post-cmd3 -> %s\n",
                      q, okq ? "ACCEPTED" : "refused");
            }
        }
    }
    const UInt32 cid = 1;           // matches the prewritten DXCONTEXT otable slot {1,3}
    const UInt32 rtvId = 0;
    const UInt32 invalidId = 0xFFFFFFFF;
    const UInt32 surfId = 3;
    const UInt32 mobId = 4;
    const UInt32 dxW = 256, dxH = 256;
    const UInt32 dxBytes = dxW * dxH * 4;
    const UInt32 dxPages = dxBytes / kGbPageBytes;
    const UInt32 expected = 0xFFFF00FF;
    UInt32 words[24];

    // ---- GB objects via LEGACY FIFO (the only path this MKS accepts for GB) ----
    if (!dxTemplateSetupSurface(fifo, surfId, mobId, dxW, dxH)) {
        return;
    }

    // ---- DX CONTEXT chain (10-21: otable entry stride is 8 B, not 16) -------
    // Live/static host facts:
    //   * SET_OTABLE_BASE64(DXCONTEXT=5) lands in the VMX otables[5] record as
    //     {base ppn, size 0x800, state 4, count 256} -> 8 bytes per entry.
    //   * DEFINE_CONTEXT (0x477) with flags=1 (NO_IRQ only) is ACCEPTED on queue 0.
    //     Its handler reads otable[cid] and, if the entry is not {-1,-1}, resets
    //     the whole entry to {-1,-1} (i.e. "unbind") and succeeds.
    //   * BIND_CONTEXT (0x479) then fails (errorOffset=12): no hash record exists
    //     and the otable fallback requires otable[cid] = {x,y}, both != -1.
    // Experiment: DEFINE, read the entry back (is the reset visible in OUR MOB or
    // only in a host shadow?), rewrite {cid, stateMob}, then BIND.
    {
        volatile UInt8 *dxTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);

        // Entry-format probe matrix (10-21). DEFINE_CONTEXT yields 0x100 only when
        // the host can READ otable[cid] and entry[0] != -1 (it then resets the entry
        // to {-1,-1}); a refused DEFINE therefore means the read itself failed.
        // Vary cid and the mob field to find what that read validates.
        struct { UInt32 cid; UInt32 e0; UInt32 e1; } variants[4] = {
            { 1,  1,  3  },   // index 1 (the one that was accepted at 23:48)
            { 2,  2,  3  },   // index 2
            { 3,  3,  3  },   // index 3
            { 8,  8,  3  },   // index 8 -- does a low index limit exist?
        };
        for (UInt32 v = 0; v < 4; v++) {
            UInt32 c = variants[v].cid;
            UInt32 m = variants[v].e1;
            UInt32 x0 = variants[v].e0;
            UInt32 x1 = variants[v].e1;
            volatile UInt8 *e = dxTable + (IOByteCount)c * kGbDxCtxEntryBytes;
            memcpy((void *)(e + 0), &x0, 4);
            memcpy((void *)(e + 4), &x1, 4);
            __asm__ volatile ("" ::: "memory");

            // DEFINE first (it needs a readable, non-empty entry; it then resets
            // the entry to {-1,-1} and reports success), then rewrite + BIND.
            UInt32 defCmd[3] = { kSvga3dCmdDxDefineContext, 4, c };
            bool okd = cbSubmitCtx(defCmd, 3, 0, 0);
            UInt32 a0 = *(volatile UInt32 *)(e + 0);
            UInt32 a1 = *(volatile UInt32 *)(e + 4);
            IOLog(VGPU_FB_TAG ": dx: VAR%u cid %u entry {%u,%u} DEFINE %s entry-after {%d,%d}\n",
                  v, c, x0, x1, okd ? "ACCEPTED" : "refused", (int)a0, (int)a1);
            if (!okd) {
                continue;
            }
            memcpy((void *)(e + 0), &x0, 4);
            memcpy((void *)(e + 4), &x1, 4);
            __asm__ volatile ("" ::: "memory");

            UInt32 bindCmd[5] = { kSvga3dCmdDxBindContext, 12, c, m, 0 };
            bool okb = cbSubmitCtx(bindCmd, 5, 0, 0);
            IOLog(VGPU_FB_TAG ": dx: VAR%u cid %u entry {%u,%u} BIND -> %s\n",
                  v, c, x0, x1, okb ? "ACCEPTED" : "refused");
            if (okb) {
                IOLog(VGPU_FB_TAG ": dx: *** BIND ACCEPTED cid %u ***\n", c);
                // SVGA3dCmdDXSetCOTable is {cid, mobid, type, validSizeInBytes}
                // -- mobid comes BEFORE type (we had them swapped, which is why
                // even the unbid form was refused).
                bool cotOk = false;
                struct { const char *tag; UInt32 mob; UInt32 valid; } cots[2] = {
                    { "mob5 valid4096", kCotableMobId, kGbPageBytes },
                    { "mob5 valid8192", kCotableMobId, 8192 },
                };
                for (UInt32 k = 0; k < 2; k++) {
                    UInt32 cot[6] = { kSvga3dCmdDxSetCotable, 16, c,
                                      cots[k].mob, 0 /*RTVIEW*/, cots[k].valid };
                    bool okc = cbSubmitCtx(cot, 6, 0, 0);
                    IOLog(VGPU_FB_TAG ": dx: SET_COTABLE %s -> %s\n",
                          cots[k].tag, okc ? "ACCEPTED" : "refused");
                    if (okc) {
                        cotOk = true;
                        break;
                    }
                }
                if (!cotOk) {
                    break;
                }

                // SET_SHADER_IFACE (0x4fd) must come AFTER the cotable: when it was
                // sent before, the cotable binding was refused (state ordering).
                {
                    UInt32 si[5] = { kSvga3dCmdDxSetShaderIface, 12, c,
                                     kCtxMobId, 0x2000 };
                    bool oki = cbSubmitCtx(si, 5, 0, 0);
                    IOLog(VGPU_FB_TAG ": dx: SET_SHADER_IFACE mob3 post-cotable -> %s\n",
                          oki ? "ACCEPTED" : "refused");
                }

                // ---- SPIN: keep the host inside a DX-flagged CB long enough to be caught
                // 2026-10-27. The only writer of the DX manager (0x14050a077) is reached
                // from 0x1404c7a2b, which sits in a PERIODIC TICK (0x1404c5ee0, no direct
                // callers, has a tick counter) and only runs its preemption branch while
                // `[dev+0x9bc] & 0x3f00` is nonzero AND `[dev+0x71] == 0`. `[dev+0x9bc]`
                // is set from a packed word whose bits 8..13 are six bits wide -- exactly
                // a cbContext (0..63) -- so it is nonzero only while the host is executing
                // a guest CB. Our CBs are a few microseconds long, so no tick ever lands
                // inside one. Hence: submit DX-flagged CBs whose payload is a long run of a
                // known-accepted, idempotent command, to give the tick many chances.
                // Filler = DX_SET_COTABLE with the values that were already ACCEPTED
                // (6 dwords each, 170 reps = 4080 B, just under the 4096 B payload MOB).
                {
                    IOBufferMemoryDescriptor *spinMem =
                        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                            kernel_task, kIODirectionInOut, kGbPageBytes, 0xFFFULL);
                    if (spinMem == nullptr) {
                        IOLog(VGPU_FB_TAG ": dx: SPIN buffer alloc failed\n");
                    } else {
                        UInt32 *sp = (UInt32 *)spinMem->map()->getVirtualAddress();
                        const UInt32 rep = 170;
                        for (UInt32 r = 0; r < rep; r++) {
                            sp[r * 6 + 0] = kSvga3dCmdDxSetCotable;
                            sp[r * 6 + 1] = 16;
                            sp[r * 6 + 2] = cid;
                            sp[r * 6 + 3] = kCotableMobId;
                            sp[r * 6 + 4] = 0;
                            sp[r * 6 + 5] = kGbPageBytes;
                        }
                        __asm__ volatile ("" ::: "memory");
                        UInt32 okSpin = 0;
                        bool spinBad = false;
                        for (UInt32 n = 0; n < 40; n++) {
                            if (!cbSubmitCtx(sp, rep * 6, 1, cid, 3)) {
                                spinBad = true;
                                break;
                            }
                            okSpin++;
                        }
                        IOLog(VGPU_FB_TAG ": dx: SPIN %u long DX CBs (q1 dx%u%s) -> %s\n",
                              okSpin, cid, spinBad ? ", stopped early" : "",
                              spinBad ? "one refused" : "all COMPLETED");
                        spinMem->release();
                    }
                }

                // ---- PRIME: give the host a chance to create the DX context ----
                // 2026-10-26. Every DX render handler bails on the null check
                // `mov rbx,[0x1414930A0]; test rbx,rbx; je fail` (RTV handler
                // 0x14055be80), and the ONLY writer of that global is 0x14050a077,
                // inside the CB-completion fragment at 0x140509fe8 gated on
                // `test r8b,2` (header->flags & SVGA_CB_FLAG_DX_CONTEXT). Its value
                // comes from lookup-or-create by header->dxContext (+0x24) via
                // 0x140a17350 -> 0x140a15b90 -> 0x1404cde70, which returns false (and
                // therefore stores NULL) unless the DXCONTEXT cotable slot for that id
                // resolves -- i.e. `entry[0] == cid` and its mobid reads back.
                //
                // Our earlier flags=2/3 probes all carried dxContext = 0, and no DX
                // context 0 was ever defined, so any such CB could only store NULL.
                // Untested until now: an EMPTY CB with the DX bit AND dxContext = cid.
                // Empty payload cannot raise COMMAND_ERROR, so it cannot wedge the
                // engine; if the lazy create is real the RTV below finds a live context.
                {
                    const UInt32 pc = 1;    // cid of the DX chain (otable slot {1,3})
                    bool okPrime = cbSubmitCtx(nullptr, 0, 1, pc, 3);
                    IOLog(VGPU_FB_TAG ": dx: PRIME (q1 dx%u flags3 len0) -> %s\n",
                          pc, okPrime ? "COMPLETED" : "failed");
                    if (!okPrime) {
                        okPrime = cbSubmitCtx(nullptr, 0, 0x3F, pc, 3);
                        IOLog(VGPU_FB_TAG ": dx: PRIME (q63 dx%u flags3 len0) -> %s\n",
                              pc, okPrime ? "COMPLETED" : "failed");
                    }
                }

                const UInt32 rtvId = 1;
                bool okRtv = false;
                UInt32 rtvQ = 0, rtvDx = 0, rtvFlags = 1;
                // 2026-10-25 (host patched): the CB header validator now lets flags
                // bit1 through, so DX_CONTEXT can finally be expressed.
                // 2026-10-26: the scanner below is nearly useless as a matrix -- the
                // FIRST DX command that returns COMMAND_ERROR wedges the whole command
                // engine, so every later cell just times out (measured: cb[19] error ->
                // cb[20..23] never completed). Only t=0 carries real information, so it
                // must be the cell with the best prior. After the PRIME above the
                // context should exist, so lead with the non-wedging device queue.
                struct { UInt32 q; UInt32 dx; UInt32 flags; } rv[4] = {
                    { 1, 1, 3 },     // vm3dmp-exact: ctx 1, NO_IRQ|DX_CONTEXT, dx = cid
                    { 1, 0, 3 },     // flag set, dxContext id left 0
                    { 0x3F, 1, 3 },  // device queue: header validator rejects dx!=0 here
                    { 1, 1, 2 },     // DX_CONTEXT only (no NO_IRQ)
                };
                for (UInt32 t = 0; t < 4 && !okRtv; t++) {
                    UInt32 q = rv[t].q;
                    UInt32 dx = rv[t].dx ? c : 0;
                    UInt32 fl = rv[t].flags;
                    UInt32 rtv[9] = { kSvga3dCmdDxDefineRenderTargetView, 28,
                                      rtvId, surfId, kSvga3dSurfaceFormatX8R8G8B8,
                                      kSvga3dResourceTexture2D, 0, 0, 1 };
                    okRtv = cbSubmitCtx(rtv, 9, q, dx, fl);
                    IOLog(VGPU_FB_TAG ": dx: DEFINE_RTV q%u dx%u flags%u -> %s\n",
                          q, dx ? c : 0, fl, okRtv ? "ACCEPTED" : "refused");
                    if (okRtv) {
                        rtvQ = q;
                        rtvDx = dx;
                        rtvFlags = fl;
                    }
                }
                if (!okRtv && kPackedTest) {
                    // 2026-10-25: the "current context" may only live for ONE CB
                    // submission. vm3dmp packs DEFINE+BIND into a single CB (builder
                    // 0x140013990); we have always split BIND and the render command
                    // into separate CBs, so the command layer may have no context
                    // when it reaches the cid-less RTV. Test three packings on
                    // queue 0 / flags 1 (the only legal flags on this host):
                    //   P0 {BIND, RTV}
                    //   P1 {BIND, SET_COTABLE, RTV}
                    //   P2 {DEFINE, BIND, RTV}
                    for (UInt32 p = 0; p < 3; p++) {
                        UInt32 pk[24];
                        UInt32 n = 0;
                        if (p == 2) {
                            pk[n++] = kSvga3dCmdDxDefineContext; pk[n++] = 4; pk[n++] = c;
                        }
                        pk[n++] = kSvga3dCmdDxBindContext;   pk[n++] = 12;
                        pk[n++] = c; pk[n++] = kCtxMobId; pk[n++] = 0;
                        if (p == 1) {
                            pk[n++] = kSvga3dCmdDxSetCotable; pk[n++] = 16;
                            pk[n++] = c; pk[n++] = kCotableMobId; pk[n++] = 0;
                            pk[n++] = kGbPageBytes;
                        }
                        pk[n++] = kSvga3dCmdDxDefineRenderTargetView; pk[n++] = 28;
                        pk[n++] = rtvId; pk[n++] = surfId;
                        pk[n++] = kSvga3dSurfaceFormatX8R8G8B8;
                        pk[n++] = kSvga3dResourceTexture2D;
                        pk[n++] = 0; pk[n++] = 0; pk[n++] = 1;
                        bool okp = cbSubmitCtx(pk, n, 0, 0);
                        IOLog(VGPU_FB_TAG ": dx: PACKED p%u (%u dwords) -> %s\n",
                              p, n, okp ? "ACCEPTED" : "refused");
                        if (okp) {
                            okRtv = true;
                            rtvQ = 0;
                            rtvDx = 0;
                            break;
                        }
                    }
                }
                if (!okRtv) {
                    // Do NOT bail: CLEAR/READBACK tell us whether the whole class of
                    // cid-less render commands is blocked (context not active) or only
                    // the RTV definition is.
                    rtvQ = 0;
                    rtvDx = 0;
                    IOLog(VGPU_FB_TAG ": dx: DEFINE_RTV refused -- probing CLEAR/READBACK anyway\n");
                }
                IOLog(VGPU_FB_TAG ": dx: DEFINE_RTV {rtv %u, sid %u} -> %s\n",
                      rtvId, surfId, okRtv ? "ACCEPTED" : "refused");
                if (!okRtv) {
                    break;
                }

                // ---- clear it to green (X8R8G8B8 0xFF00FF00) ----
                UInt32 clr[8] = { kSvga3dCmdDxClearRenderTargetView, 20, rtvId, 0, 0, 0, 0 };
                { float g[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
                  memcpy(&clr[3], g, 16); }
                bool okClr = cbSubmitCtx(clr, 7, rtvQ, rtvDx, rtvFlags);
                IOLog(VGPU_FB_TAG ": dx: CLEAR_RTV {rtv %u} -> %s\n",
                      rtvId, okClr ? "ACCEPTED" : "refused");
                if (!okClr) {
                    break;
                }

                // ---- read the surface back into our buffer ----
                UInt32 rb[4] = { kSvga3dCmdDxReadbackSubResource, 8, surfId, 0 };
                bool okRb = cbSubmitCtx(rb, 4, rtvQ, rtvDx, rtvFlags);
                IOLog(VGPU_FB_TAG ": dx: READBACK_SUBRESOURCE {sid %u} -> %s\n",
                      surfId, okRb ? "ACCEPTED" : "refused");
                __asm__ volatile ("" ::: "memory");
                if (okRb) {
                    UInt32 px = *(volatile UInt32 *)_dxVirt;
                    UInt32 mid = *(volatile UInt32 *)((char *)_dxVirt +
                                  (IOByteCount)(dxW * (dxH / 2) + dxW / 2) * 4);
                    IOLog(VGPU_FB_TAG ": dx: READBACK pixels first=0x%08x mid=0x%08x"
                          " (want 0xff00ff00)\n", px, mid);
                }
                break;
            }
        }
    }

    // =====================================================================
    // GB CONTEXT RENDER PATH (vmwgfx/QEMU-style: no DX commands at all).
    //   DEFINE_GB_CONTEXT(1107){cid}, BIND_GB_CONTEXT(1109){cid,mob,valid},
    //   SETRENDERTARGET(1050){cid,type,::sid,face,mip}, CLEAR(1057){cid,flag,
    //   color,depth,stencil}, READBACK_GB_IMAGE(1103){sid,face,mip}.
    // Transport: on this vGPU10/GBOBJECTS device the legacy FIFO parses+fences
    // every 3D command but never executes the render pipeline (10-10: CLEAR
    // no-op, state MOB untouched). vmwgfx on GBOBJECTS devices submits ALL 3D
    // commands through command buffers -- so kGbcViaCb=1 sends the render path
    // via plain CB (dxContext=0, flags=0), keeping the GB MOB define on the
    // legacy FIFO (proven by the working display path). Single variable.
    // =====================================================================
    {
#define GBC_SUBMIT(w, n) (kGbcViaCb ? cbSubmit((w), (n)) \
                                     : (fifoSubmitWords(fifo, (w), (n)) && fenceAck(fifo, &_fenceSeq)))
        const UInt32 gcid = 1;
        const UInt32 gctxMob = 7;
        const UInt32 gctxBytes = 65536;

        _ctxStateMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, gctxBytes, 0xFFFULL);
        _ctxStatePtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
        if (_ctxStateMem == nullptr || _ctxStatePtMem == nullptr) {
            IOLog(VGPU_FB_TAG ": gbc: context MOB alloc failed\n");
            return;
        }
        bzero((void *)_ctxStateMem->map()->getVirtualAddress(), gctxBytes);
        volatile UInt64 *gpt = (volatile UInt64 *)_ctxStatePtMem->map()->getVirtualAddress();
        IOByteCount gLen = 0;
        IOVirtualAddress gPt = _ctxStatePtMem->getPhysicalSegment(0, &gLen);
        if (gPt == 0) {
            IOLog(VGPU_FB_TAG ": gbc: context PT page not resolvable\n");
            return;
        }
        for (UInt32 i = 0; i < gctxBytes / kGbPageBytes; i++) {
            IOVirtualAddress addr = _ctxStateMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &gLen);
            if (addr == 0) {
                IOLog(VGPU_FB_TAG ": gbc: context MOB page %u not resolvable\n", i);
                return;
            }
            gpt[i] = (UInt64)addr >> kGbPageShift;
        }
        __asm__ volatile ("" ::: "memory");
        const UInt64 gPtPpn = (UInt64)gPt >> kGbPageShift;

        // DEFINE_GB_CONTEXT {cid}
        words[0] = kSvga3dCmdDefineGbContext;
        words[1] = 4;
        words[2] = gcid;
        if (!GBC_SUBMIT(words, 3)) {
            IOLog(VGPU_FB_TAG ": gbc: DEFINE_GB_CONTEXT refused\n");
            goto gbFailed;
        }
        IOLog(VGPU_FB_TAG ": gbc: DEFINE_GB_CONTEXT accepted\n");

        // DEFINE_GB_MOB64 for the context state mob (before bind reads it).
        words[0] = kSvga3dCmdDefineGbMob64;
        words[1] = 20;
        words[2] = gctxMob;
        words[3] = kSvga3dMobFmtPt64_1;
        memcpy(&words[4], &gPtPpn, 8);
        words[6] = gctxBytes;
        if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
            IOLog(VGPU_FB_TAG ": gbc: context MOB define refused\n");
            goto gbFailed;
        }

        // BIND_GB_CONTEXT {cid, mobid, validContents}
        words[0] = kSvga3dCmdBindGbContext;
        words[1] = 12;
        words[2] = gcid;
        words[3] = gctxMob;
        words[4] = 0;
        if (!GBC_SUBMIT(words, 5)) {
            IOLog(VGPU_FB_TAG ": gbc: BIND_GB_CONTEXT refused\n");
            goto gbFailed;
        }
        IOLog(VGPU_FB_TAG ": gbc: BIND_GB_CONTEXT accepted (mob %u, %u B)\n", gctxMob, gctxBytes);

        // SETRENDERTARGET {cid, type=COLOR0(2), sid 3, face 0, mip 0}
        words[0] = kSvga3dCmdSetRenderTarget;
        words[1] = 20;
        words[2] = gcid;
        words[3] = 2;                   // SVGA3D_RT_COLOR0
        words[4] = surfId;
        words[5] = 0;
        words[6] = 0;
        if (!GBC_SUBMIT(words, 7)) {
            IOLog(VGPU_FB_TAG ": gbc: SETRENDERTARGET refused\n");
            goto gbFailed;
        }
        IOLog(VGPU_FB_TAG ": gbc: SETRENDERTARGET accepted (sid %u)\n", surfId);

        // SETVIEWPORT {cid, rect} -- the context state MOB is all-zero, so the
        // viewport would be 0x0 and CLEAR would clip to nothing (readback all-zero).
        // Zero-initialised minZ/maxZ handled by the host for the legacy pipeline;
        // rect covers the whole 256x256 target.
        words[0] = kSvga3dCmdSetViewport;
        words[1] = 20;
        words[2] = gcid;
        words[3] = 0;                   // rect.x
        words[4] = 0;                   // rect.y
        words[5] = dxW;                 // rect.w
        words[6] = dxH;                 // rect.h
        if (!GBC_SUBMIT(words, 7)) {
            IOLog(VGPU_FB_TAG ": gbc: SETVIEWPORT refused\n");
            goto gbFailed;
        }
        IOLog(VGPU_FB_TAG ": gbc: SETVIEWPORT accepted\n");

        // CLEAR {cid, flag=COLOR, color, depth 1.0, stencil 0} -- WHITE against the
        // magenta prefill discriminates CLEAR-vs-readback responsibility.
        const UInt32 gbcClear = 0xFFFFFFFF;
        words[0] = kSvga3dCmdClearRt;
        words[1] = 20;
        words[2] = gcid;
        words[3] = 1;                   // SVGA3D_CLEAR_COLOR
        words[4] = gbcClear;            // white
        words[5] = 0x3F800000;          // 1.0f
        words[6] = 0;
        if (!GBC_SUBMIT(words, 7)) {
            IOLog(VGPU_FB_TAG ": gbc: CLEAR refused\n");
            goto gbFailed;
        }
        IOLog(VGPU_FB_TAG ": gbc: CLEAR accepted\n");

        // READBACK_GB_IMAGE {sid 3, face 0, mip 0}
        words[0] = kSvga3dCmdReadbackGbImage;
        words[1] = 12;
        words[2] = surfId;
        words[3] = 0;
        words[4] = 0;
        if (!GBC_SUBMIT(words, 5)) {
            IOLog(VGPU_FB_TAG ": gbc: READBACK_GB_IMAGE refused\n");
            goto gbFailed;
        }
        __asm__ volatile ("" ::: "memory");
        volatile UInt32 *px = (volatile UInt32 *)_dxVirt;
        UInt32 p0 = px[0];
        UInt32 pMid = px[(dxH / 2) * dxW + (dxW / 2)];
        UInt32 pLast = px[(dxH - 1) * dxW + (dxW - 1)];
        IOLog(VGPU_FB_TAG ": gbc: readback p0=%08x pMid=%08x pLast=%08x "
              "(white=%08x magenta=%08x)\n",
              p0, pMid, pLast, gbcClear, 0xFFFF00FFu);
        // Probe: did the host write pipeline state (viewport etc.) into the bound
        // context MOB? Nonzero dwords = host mirrors state into guest memory.
        {
            volatile UInt32 *st = (volatile UInt32 *)_ctxStateMem->map()->getVirtualAddress();
            UInt32 nz = 0;
            for (UInt32 i = 0; i < gctxBytes / 4 && nz < 8; i++) {
                if (st[i] != 0) {
                    IOLog(VGPU_FB_TAG ": gbc: state mob dword[%u] @+%#x = %08x\n",
                          i, i * 4, st[i]);
                    nz++;
                }
            }
            if (nz == 0) {
                IOLog(VGPU_FB_TAG ": gbc: state mob all zero (host keeps state internal)\n");
            }
        }
        if (p0 == gbcClear && pMid == gbcClear && pLast == gbcClear) {
            IOLog(VGPU_FB_TAG ": gbc: *** GB CONTEXT RENDER ROUND TRIP VERIFIED ***\n");
        } else if (p0 == 0xFFFF00FF) {
            IOLog(VGPU_FB_TAG ": gbc: CLEAR did not render (buffer unchanged)\n");
        } else {
            IOLog(VGPU_FB_TAG ": gbc: readback mismatch\n");
        }
        // This bare block always reaches here. Every early exit above (and this one) is
        // written as `if (kDxVm3dmpTemplate) goto gbFailed; return;` so the template below
        // stays reachable in the compiler's view (docs §24: an unconditional return here
        // let clang drop the whole template block, and the 10-27 22:26 install silently
        // ran the legacy chain instead).
        if (!kDxVm3dmpTemplate) {
            return;   // GB path is this boot's answer; skip the DX chain
        }
    }
    // Anchor: keeps the legacy GB chain live in the binary even when a mode flag
    // short-circuits the whole block, so build.sh's reachability check keeps
    // guarding the chain that the *next* experiment will turn back on. Without
    // it, flipping one flag turns 300 lines into "never written" and the marker
    // check starts failing for a reason that has nothing to do with this change.
    IOLog(VGPU_FB_TAG ": gbc: chain block reached (surf %u mob %u %ux%u, next=%u)\n",
          surfId, mobId, dxW, dxH, (UInt32)kDxVm3dmpTemplate);

gbFailed:
    if (!kDxVm3dmpTemplate) {
        return;
    }
    IOLog(VGPU_FB_TAG ": gbc: chain stopped early; continuing to the DX template\n");

    // =====================================================================
    // kDxVm3dmpTemplate: assemble the DX context the way vm3dmp.sys builds it,
    // then submit ONE render command. Every earlier attempt differed from the
    // driver in the same three ways below, so the first version of this block
    // is the template almost verbatim (disassembly: CreateDXContext 0x140010c78,
    // context ctor 0x1400109c0..0x140010a8e, COTable bind 0x1400113e8).
    //   A. DEFINE+BIND share ONE CB, and that same CB is also the preemptible
    //      ("pre") batch: helper 0x1400095d8(dev, pre=1, ctx=cid, words, n,
    //      tok, prio=2, ...) -> 0x14001a76c appends it to the CB whose context
    //      index is `cid` (cbContext == DX context id, not the default 1).
    //   B. SET_COTABLE carries mobid=0xFFFFFFFF for every type the context does
    //      NOT use (11 or 12 of them), and those binds happen INSIDE the
    //      DEFINE+BIND CB: COTable size is read at context creation
    //      (0x1400113e8(dev, cid, type, mobid, valid, doBind)), not later.
    //   C. No VALID CONTENTS (validSize=0 everywhere) and header.dxContext = 0.
    // The context state MOB is declared here (the legacy chain below owns the
    // same mobid but never runs while this switch is on).
    // Only ONE DX command closes the sequence, per the one-error-one-reboot rule.
    // =====================================================================
    if (kDxVm3dmpTemplate) {
        bool alive = dxVm3dmpTemplate(fifo, surfId, dxW, dxH);
        IOLog(VGPU_FB_TAG ": dx: TEMPLATE verdict -> %s\n",
              alive ? "*** DX MANAGER ALIVE ***" : "DX manager still absent");
        return;
    }

    // ---- DX chain via the CB (vm3dmp semantics: flags=3, header dxContext=0) ----
    // Unreachable while the template block above is enabled; it submits several
    // DX commands and every error costs a reboot. (The template block returns on its
    // own; no extra guard is needed here, and one was the source of the §24 dead code.)

    // DXCONTEXT OTable entry {cid, mobid}: ABSENT in the 14:04 boots where BIND was
    // accepted; present (added 80ba752) in every boot where BIND was refused. The
    // host's DEFINE_CONTEXT creates the entry itself -- pre-writing it plausibly
    // makes the host's define/bind path see a conflicting entry. Gated off by default.
    if (kOtablePrewrite) {
        volatile UInt8 *dxTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);
        volatile UInt8 *dxEntry = dxTable + (IOByteCount)cid * kGbDxCtxEntryBytes;
        UInt32 stateMob = 1;
        memcpy((void *)(dxEntry + 0), &cid, 4);
        memcpy((void *)(dxEntry + 4), &stateMob, 4);
        __asm__ volatile ("" ::: "memory");
        IOLog(VGPU_FB_TAG ": dx: DXCONTEXT otable entry {cid %u, mob %u} written\n",
              cid, stateMob);
    } else {
        IOLog(VGPU_FB_TAG ": dx: otable pre-write skipped (kOtablePrewrite=0)\n");
    }

    words[0] = kSvga3dCmdDxDefineContext;
    words[1] = 4;
    words[2] = cid;
    if (!cbSubmit(words, 3)) {
        IOLog(VGPU_FB_TAG ": dx: CB DX_DEFINE refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": dx: DX_DEFINE accepted\n");

    // Exact-size state MOB experiment: sizeof(SVGADXContextMobFormat) = 8964 B
    // (computed from svga3d_dx.h with SHADERTYPE=7, QUERY=64, COTABLE=12, UAV=64)
    // -> 3 pages = 12288 B. The host may validate the BIND mob size against the
    // DX context format; 16 KiB and 512 KiB both failed. Fresh mob 6 (PT64_1).
    const UInt32 stateMobId = 6;
    const UInt32 ctxStateBytes = 12288;
    _ctxStateMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, ctxStateBytes, 0xFFFULL);
    _ctxStatePtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_ctxStateMem == nullptr || _ctxStatePtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": dx: exact-size state MOB alloc failed\n");
        return;
    }
    bzero((void *)_ctxStateMem->map()->getVirtualAddress(), ctxStateBytes);
    volatile UInt64 *spt = (volatile UInt64 *)_ctxStatePtMem->map()->getVirtualAddress();
    IOByteCount smLen = 0;
    IOVirtualAddress smPt = _ctxStatePtMem->getPhysicalSegment(0, &smLen);
    if (smPt == 0) {
        IOLog(VGPU_FB_TAG ": dx: state MOB PT page not resolvable\n");
        return;
    }
    for (UInt32 i = 0; i < ctxStateBytes / kGbPageBytes; i++) {
        IOVirtualAddress addr = _ctxStateMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &smLen);
        if (addr == 0) {
            IOLog(VGPU_FB_TAG ": dx: state MOB page %u not resolvable\n", i);
            return;
        }
        spt[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 smPtPpn = (UInt64)smPt >> kGbPageShift;
    {
        volatile UInt8 *dxTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffDxCtxTable);
        volatile UInt8 *dxEntry = dxTable + (IOByteCount)cid * kGbDxCtxEntryBytes;
        memcpy((void *)(dxEntry + 0), &cid, 4);
        memcpy((void *)(dxEntry + 4), &stateMobId, 4);
        __asm__ volatile ("" ::: "memory");
        IOLog(VGPU_FB_TAG ": dx: otable entry {cid %u, mob %u} (exact-size 12288 B)\n",
              cid, stateMobId);
    }
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = stateMobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &smPtPpn, 8);
    words[6] = ctxStateBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: exact-size state MOB define refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": dx: exact-size state MOB %u defined (12288 B PT64_1)\n", stateMobId);

    words[0] = kSvga3dCmdDxBindContext;
    words[1] = 12;
    words[2] = cid;
    words[3] = stateMobId;
    words[4] = 0;
    if (!cbSubmit(words, 5)) {
        IOLog(VGPU_FB_TAG ": dx: CB DX_BIND refused (queue wedged; stop)\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": dx: DX_BIND accepted (exact-size state MOB)\n");

    // BISECT: DEFINE and BIND as SEPARATE single-command CBs (this MKS rejects
    // multi-command DX CBs with CB_HEADER_ERROR), state MOB = mobid 1 (the 16 KiB
    // data MOB -- the only mob BIND has ever accepted here).
    // SET_SHADER_IFACE (0x4fd) -- vm3dmp sends this IMMEDIATELY after DEFINE+BIND
    // (create sequence: DEFINE+BIND packed CB, then SET_SHADER_IFACE CB, and only
    // later cotable binds). Body = {cid, stateMobid, prependOffset=0x2000}: the
    // shader interface block lives at offset 0x2000 inside the state MOB.
    words[0] = kSvga3dCmdDxSetShaderIface;
    words[1] = 12;
    words[2] = cid;
    words[3] = 1;                   // state MOB (same field the BIND used)
    words[4] = 0x2000;
    if (!cbSubmit(words, 5)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_SHADER_IFACE refused (continuing)\n");
    } else {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_SHADER_IFACE accepted\n");
    }

    // SET_COTABLE {cid, RTVIEW, mobid 5, validSize 0} (vm3dmp initial bind).
    words[0] = kSvga3dCmdDxSetCotable;
    words[1] = 16;
    words[2] = cid;
    words[3] = kCotableMobId;
    words[4] = 0;                   // SVGA_COTABLE_RTVIEW
    words[5] = 0;
    if (!cbSubmit(words, 6)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_COTABLE refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": dx: DX_SET_COTABLE accepted\n");

    // RTV define, cotable resize to 1 entry, render targets, viewport, scissor.
    words[0] = kSvga3dCmdDxDefineRenderTargetView;
    words[1] = 28;
    words[2] = rtvId;
    words[3] = surfId;
    words[4] = kSvga3dSurfaceFormatX8R8G8B8;
    words[5] = kSvga3dResourceTexture2D;
    words[6] = 0;
    words[7] = 0;
    words[8] = 1;
    if (!cbSubmit(words, 9)) {
        IOLog(VGPU_FB_TAG ": dx: DX_DEFINE_RENDERTARGET_VIEW refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetCotable;
    words[1] = 16;
    words[2] = cid;
    words[3] = kCotableMobId;
    words[4] = 0;
    words[5] = 32;
    if (!cbSubmit(words, 6)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_COTABLE resize refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetRenderTargets;
    words[1] = 8;
    words[2] = invalidId;
    words[3] = rtvId;
    if (!cbSubmit(words, 4)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_RENDERTARGETS refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetViewports;
    words[1] = 4 + 24;
    words[2] = 0;
    const float viewport[6] = { 0.0f, 0.0f, (float)dxW, (float)dxH, 0.0f, 1.0f };
    memcpy(&words[3], viewport, 24);
    if (!cbSubmit(words, 9)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_VIEWPORTS refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetScissorRects;
    words[1] = 4 + 16;
    words[2] = 0;
    words[3] = 0;
    words[4] = 0;
    words[5] = dxW;
    words[6] = dxH;
    if (!cbSubmit(words, 7)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_SCISSORRECTS refused\n");
        return;
    }

    // DMA round trip: upload magenta, download, verify.
    volatile UInt32 *fill = (volatile UInt32 *)_dxVirt;
    for (UInt32 i = 0; i < dxBytes / 4; i++) {
        fill[i] = expected;
    }
    __asm__ volatile ("" ::: "memory");

    UInt32 dma[24];
    dma[0] = 1044;
    dma[1] = 28 + 36 + 12;
    dma[2] = mobId;
    dma[3] = 0;
    dma[4] = dxW * 4;
    dma[5] = surfId;
    dma[6] = 0;
    dma[7] = 0;
    dma[8] = 1;                     // SVGA3D_WRITE_HOST_VRAM
    dma[9] = 0;  dma[10] = 0;  dma[11] = 0;
    dma[12] = dxW; dma[13] = dxH; dma[14] = 1;
    dma[15] = 0;  dma[16] = 0;  dma[17] = 0;
    dma[18] = 12; dma[19] = dxBytes; dma[20] = 0;
    if (!fifoSubmitWords(fifo, dma, 21) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: SURFACE_DMA upload refused\n");
        return;
    }

    for (UInt32 i = 0; i < dxBytes / 4; i++) {
        fill[i] = 0x11223344;
    }
    __asm__ volatile ("" ::: "memory");

    dma[8] = 2;                     // SVGA3D_READ_HOST_VRAM
    if (!fifoSubmitWords(fifo, dma, 21) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: SURFACE_DMA download refused\n");
        return;
    }
    __asm__ volatile ("" ::: "memory");

    volatile UInt32 *px = (volatile UInt32 *)_dxVirt;
    UInt32 p0 = px[0];
    UInt32 pMid = px[(dxH / 2) * dxW + (dxW / 2)];
    UInt32 pLast = px[(dxH - 1) * dxW + (dxW - 1)];
    if (p0 == expected && pMid == expected && pLast == expected) {
        IOLog(VGPU_FB_TAG ": dx: DMA ROUND TRIP VERIFIED -- magenta survived upload+download\n");
        // The verdict: clear through the host 3D engine, pull back with DMA.
        words[0] = kSvga3dCmdDxClearRenderTargetView;
        words[1] = 20;
        words[2] = rtvId;
        const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
        memcpy(&words[3], magenta, 16);
        if (!cbSubmit(words, 7)) {
            IOLog(VGPU_FB_TAG ": dx: DX_CLEAR refused\n");
            return;
        }
        for (UInt32 i = 0; i < dxBytes / 4; i++) {
            fill[i] = 0x11223344;
        }
        __asm__ volatile ("" ::: "memory");
        dma[8] = 2;
        if (!fifoSubmitWords(fifo, dma, 21) || !fenceAck(fifo, &_fenceSeq)) {
            IOLog(VGPU_FB_TAG ": dx: post-clear DMA refused\n");
            return;
        }
        p0 = px[0];
        pMid = px[(dxH / 2) * dxW + (dxW / 2)];
        pLast = px[(dxH - 1) * dxW + (dxW - 1)];
        if (p0 == expected && pMid == expected && pLast == expected) {
            IOLog(VGPU_FB_TAG ": dx: ROUND TRIP VERIFIED -- DX CLEAR read back as 0x%08x "
                  "(host 3D engine wrote guest-visible memory)\n", p0);
        } else {
            IOLog(VGPU_FB_TAG ": dx: post-clear mismatch: p0=0x%08x pMid=0x%08x pLast=0x%08x\n",
                  p0, pMid, pLast);
        }
    } else if (p0 == 0x11223344) {
        IOLog(VGPU_FB_TAG ": dx: SURFACE_DMA wrote nothing -- sentinel survived\n");
    } else {
        IOLog(VGPU_FB_TAG ": dx: DMA round-trip mismatch: p0=0x%08x pMid=0x%08x pLast=0x%08x\n",
              p0, pMid, pLast);
    }
}

// Initialise the command-buffer areas: a 64-byte header and a 64 KiB command
// payload, both physically contiguous.
bool vgpuFramebuffer::cbMobSetup(volatile UInt32 *fifo) {
    if (kCbMobMode == 0) {
        return false;
    }
    UInt32 words[24];
    _cbMobMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        kGbPageBytes, 0xFFFULL);
    if (_cbMobMem == nullptr) {
        IOLog(VGPU_FB_TAG ": dx: cb payload MOB alloc failed\n");
        return false;
    }
    _cbMobVirt = (volatile UInt8 *)_cbMobMem->map()->getVirtualAddress();
    bzero((void *)_cbMobVirt, kGbPageBytes);
    IOByteCount mbLen = 0;
    IOVirtualAddress mbAddr = _cbMobMem->getPhysicalSegment(0, &mbLen);
    if (mbAddr == 0) {
        IOLog(VGPU_FB_TAG ": dx: cb payload MOB page not resolvable\n");
        return false;
    }
    const UInt64 mbPpn = (UInt64)mbAddr >> kGbPageShift;
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mbSize = kGbPageBytes;
    UInt32 mbDepth = kSvga3dMobFmtPt64_0;
    UInt32 mbOff = kCbMobId * kGbMobEntryBytes;
    memcpy((void *)(mobTable + mbOff), &mbDepth, 4);
    memcpy((void *)(mobTable + mbOff + 4), &mbSize, 4);
    memcpy((void *)(mobTable + mbOff + 8), &mbPpn, 8);
    __asm__ volatile ("" ::: "memory");
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = kCbMobId;
    words[3] = kSvga3dMobFmtPt64_0;
    memcpy(&words[4], &mbPpn, 8);
    words[6] = kGbPageBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: cb payload MOB DEFINE_GB_MOB64 refused\n");
        return false;
    }
    _cbMobReady = true;
    IOLog(VGPU_FB_TAG ": dx: CB payload MOB %u ready (1 page PT64_0)\n", kCbMobId);
    return true;
}

bool vgpuFramebuffer::cbInit(void) {
    _cbHeaderMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 64, 0x3F);
    _cbDataMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kCbDataBytes, 0xFFFULL);
    if (_cbHeaderMem == nullptr || _cbDataMem == nullptr) {
        IOLog(VGPU_FB_TAG ": cb: allocation failed\n");
        return false;
    }
    _cbHeaderVirt = (volatile UInt8 *)_cbHeaderMem->map()->getVirtualAddress();
    _cbDataVirt = (volatile UInt8 *)_cbDataMem->map()->getVirtualAddress();
    // Host-side debug marker: never touched by the host (beyond CB length), used by
    // a host debugger to locate this buffer in the VMX address space and thereby
    // derive the guest-PA -> host-VA mapping of the kernel allocation.
    {
        static const char kCbMarker[32] = "VGPU-CB-DATA-AT-0x800-DEADBEEF!";
        memcpy((void *)((char *)_cbDataVirt + 0x800), kCbMarker, 32);
        __asm__ volatile ("" ::: "memory");
    }
    IOByteCount len = 0;
    IOVirtualAddress pa = _cbHeaderMem->getPhysicalSegment(0, &len);
    if (pa == 0 || (pa & 0x3F) != 0) {
        IOLog(VGPU_FB_TAG ": cb: header PA not 64-aligned\n");
        return false;
    }
    _cbHeaderPA = (UInt64)pa;
    pa = _cbDataMem->getPhysicalSegment(0, &len);
    if (pa == 0 || (pa & 0x3F) != 0) {
        IOLog(VGPU_FB_TAG ": cb: data PA not 64-aligned\n");
        return false;
    }
    _cbDataPA = (UInt64)pa;
    IOLog(VGPU_FB_TAG ": cb: header @ %llx, data @ %llx\n",
          (unsigned long long)_cbHeaderPA, (unsigned long long)_cbDataPA);

    // Guest driver identity. NOTE: the original comment here claimed "vm3dmp writes exactly
    // this at init" -- it does not, and never did. The host driver writes no register above
    // 0x36 at all (tools/rev/reg-indices.py). Left off; the switch exists only so the
    // hypothesis can be re-tested deliberately.
    IOLog(VGPU_FB_TAG ": cb: GUEST_DRIVER_ID write %s (kGuestDriverId=%u)\n",
          kGuestDriverId ? "enabled" : "skipped", kGuestDriverId);

    // Register a PREPEND buffer (SVGA_REG_CMD_PREPEND_LOW/HIGH = 53/54), exactly as
    // vm3dmp does before/with every submission: PREPEND_HIGH = PA>>32,
    // PREPEND_LOW = PA_low | cbContext (low 6 bits repurposed as context id).
    // Host-side reverse (vmware-vmx symbols: SVGAWriteCommandPrependReg) + guest
    // driver strings ("No room left to prepend the command buffer...") show this is
    // the queue-front buffer used to resume a preempted/errored CB. vm3dmp keeps it
    // registered at all times, so we mirror that.
    if (kCbPrepend) {
    _cbPrependMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096, 0x3F);
    if (_cbPrependMem == nullptr) {
        IOLog(VGPU_FB_TAG ": cb: prepend buffer alloc failed (continuing)\n");
        return true;   // non-fatal: keep going without it
    }
    bzero((void *)_cbPrependMem->map()->getVirtualAddress(), 4096);
    IOByteCount ppLen = 0;
    IOVirtualAddress ppAddr = _cbPrependMem->getPhysicalSegment(0, &ppLen);
    if (ppAddr == 0 || (ppAddr & 0x3F) != 0) {
        IOLog(VGPU_FB_TAG ": cb: prepend buffer PA not 64-aligned (continuing)\n");
        return true;
    }
    UInt64 ppPA = (UInt64)ppAddr;
    // SVGA_REG_CMD_PREPEND_LOW/HIGH = 53/54. These are NOT the command doorbell -- 48/49 is,
    // and this code briefly used 53/54 for submission after §40 conflated the two.
    svgaWriteRegister(_svgaPortBase, kSvgaRegCmdPrependHigh, (UInt32)(ppPA >> 32));
    svgaWriteRegister(_svgaPortBase, kSvgaRegCmdPrependLow, (UInt32)(ppPA & 0xFFFFFFFFu) | 0u);
    IOLog(VGPU_FB_TAG ": cb: PREPEND registered @ %llx (ctx 0)\n",
          (unsigned long long)ppPA);
    }  // kCbPrepend
    else {
        IOLog(VGPU_FB_TAG ": cb: PREPEND registration disabled (kCbPrepend=0)\n");
    }

    if (kFlagsProbe) {
        static const UInt32 kProbeFlags[4] = { 0, 1, 2, 3 };
        UInt32 startstop[3] = { 1, 1, 0 };   // DC cmd 1, enable=1, context=0 (known-good)
        for (UInt32 i = 0; i < 4; i++) {
            UInt32 f = kProbeFlags[i];
            bool emptyOk = cbSubmitCtx(nullptr, 0, 0x3F, 0, f);
            IOLog(VGPU_FB_TAG ": probe: flags %u len 0  -> %s\n",
                  f, emptyOk ? "COMPLETED" : "NOT-COMPLETED");
            bool realOk = cbSubmitCtx(startstop, 3, 0x3F, 0, f);
            IOLog(VGPU_FB_TAG ": probe: flags %u len 12 -> %s\n",
                  f, realOk ? "COMPLETED" : "NOT-COMPLETED");
        }
    }
    return true;
}

// Submit one command buffer holding `wordCount` dwords and wait (poll) for the
// device to complete it. dxContext != 0 marks the payload as a DX-command stream
// (SVGA_CB_FLAG_DX_CONTEXT + the context id in the header); 0 submits a plain stream.
bool vgpuFramebuffer::cbSubmitCtx(const UInt32 *cmds, UInt32 wordCount, UInt32 cbContext,
                                  UInt32 dxContext, UInt32 flags) {
    const UInt32 byteCount = wordCount * sizeof(UInt32);
    // Every early exit below used to be silent, and a silent `false` is indistinguishable
    // from "the host said no" at the call site. The 10-29 template run printed
    // "-> refused" with NO status/timeout line, so the CB was never even submitted; the
    // only exits that can do that are these three. Name them.
    if (_cbHeaderVirt == nullptr) {
        IOLog(VGPU_FB_TAG ": cb: no header allocation (%u dwords)\n", wordCount);
        return false;
    }
    if (_pci == nullptr) {
        IOLog(VGPU_FB_TAG ": cb: no PCI device yet (%u dwords)\n", wordCount);
        return false;
    }
    if (byteCount > kCbDataBytes) {
        IOLog(VGPU_FB_TAG ": cb: payload too large (%u)\n", byteCount);
        return false;
    }
    // MOB form: payload goes into the GB MOB, not the DMA buffer.
    const bool mobMode = (kCbMobMode != 0) && _cbMobReady && (_cbMobVirt != nullptr);
    if (kCbDump) {
        IOLog(VGPU_FB_TAG ": cb: submit %u dwords ctx%u dx%u flags%u mob=%d "
              "(mobMode=%u ready=%u virt=%p hdr=%p pci=%p dataVirt=%p)\n",
              wordCount, cbContext, dxContext, flags, kCbMobMode,
              mobMode ? 1u : 0u, _cbMobReady ? 1u : 0u,
              (void *)_cbMobVirt, (void *)_cbHeaderVirt, (void *)_pci,
              (void *)_cbDataVirt);
    }
    // The payload MOB is ONE page (kGbPageBytes); the 64 KiB bound above only covers the
    // DMA form. Without this check a >4 KiB payload memcpy'd into the MOB silently walks
    // past a physically-contiguous 1-page allocation (2026-10-27, found while writing the
    // SPIN probe, which is exactly the kind of caller that would have tripped it).
    if (mobMode && byteCount > kGbPageBytes) {
        IOLog(VGPU_FB_TAG ": cb: payload %u exceeds the %u-byte CB payload MOB\n",
              byteCount, kGbPageBytes);
        return false;
    }
    if (byteCount != 0) {
        memcpy((void *)(mobMode ? _cbMobVirt : _cbDataVirt), cmds, byteCount);
    }
    // NOTE: no padding -- zero-filled padding parses as command id 0 and the host
    // reports COMMAND_ERROR right after the real command (that was the offset-12
    // "failure" of DX_DEFINE_CONTEXT, which had actually succeeded).
    __asm__ volatile ("" ::: "memory");

    volatile UInt32 *h = (volatile UInt32 *)_cbHeaderVirt;
    // status = SVGA_CB_STATUS_NONE, errorOffset = 0
    h[0] = 0;
    h[1] = 0;
    *(volatile UInt64 *)(_cbHeaderVirt + 8) = _cbId++;                    // id
    // ---- flags: what three weeks of rejected headers actually taught us ----
    // (docs §15 / §22, re-read on 10-29 after the template died at 0 ms)
    //
    //   * The header validator only accepts flags 0/1. Anything with bit1
    //     (SVGA_CB_FLAG_DX_CONTEXT) is answered with STATUS 4 / CB_HEADER_ERROR, no
    //     matter what cbContext, dxContext, PREPEND or MOB form goes with it (§15, §14).
    //     The 10-12/10-13 "flags=2 is what vm3dmp does" note was a misread of the
    //     builder at 0x140013a51 and cost two weeks.
    //   * Therefore EVERY CB we send -- DX payload included -- goes out with NO_IRQ
    //     only. Callers pass 2/3 for intent; the bit is stripped here, in one place,
    //     so a future experiment cannot silently reintroduce the header error.
    //   * dxContext STILL matters: it lives in the header and the command layer reads
    //     it to pick the DX context for cid-less commands. §22.6: same cbContext but
    //     dxContext 1 vs 0 changes whether the batch reaches STATUS 3 at all.
    //
    // 10-29 20:17 (MOB form, flags=5 → stripped to 1) and 20:22 (PA form, flags=1,
    // dxContext=1): BOTH came back STATUS 4 with a clean, minimal header. So the
    // rejection is not about MOB, not about bit1, not about the payload -- it is the
    // `dxContext != 0` path itself. Per §22.2 that path is:
    //
    //     0x14050a04e  test r8b, 2        ; DX_CONTEXT present?
    //     0x14050a052  je 0x14050a20d     ; no  -> plain command-stream branch
    //     0x14050a058  cmp [0x1414937FC], 0
    //     0x14050a072  call 0x140a17350   ; lookup-or-create(dxContext)
    //     0x14050a07e  test rax,rax ; je → give up on the batch
    //
    // If the lookup cannot resolve dxContext (because no DX device was ever created
    // -- see [dev+0x9bc] / [dev+0x71] / [dev+0x81d] in §22.2), the header layer fails
    // the batch. That matches every STATUS 4 we have: dxContext was non-zero and no
    // DX device existed. kCbKeepDxContext selects which branch the batch takes.
    if (!kCbKeepDxContext) {
        flags &= ~kCbFlagDxContext;
    }
    flags |= 1u;             // NO_IRQ -- the only guaranteed-safe bit
    *(volatile UInt32 *)(_cbHeaderVirt + 20) = byteCount;                 // length
    if (mobMode) {
        *(volatile UInt32 *)(_cbHeaderVirt + 24) = kCbMobId;              // ptr.mob.mobid
        *(volatile UInt32 *)(_cbHeaderVirt + 28) = 0;                     // ptr.mob.mobOffset
        flags |= 4;                                                       // SVGA_CB_FLAG_MOB
    } else {
        *(volatile UInt64 *)(_cbHeaderVirt + 24) = _cbDataPA;             // ptr.pa
    }
    *(volatile UInt32 *)(_cbHeaderVirt + 16) = flags;   // flags
    *(volatile UInt32 *)(_cbHeaderVirt + 32) = 0;                         // offset
    // SVGA_CB_FLAG_DX_CONTEXT requires dxContext to be a VALID DX context id;
    // 0 is invalid and trips the header validator (CB_HEADER_ERROR) -- the real
    // source of every header rejection we have seen (10-13).
    *(volatile UInt32 *)(_cbHeaderVirt + 36) = dxContext;                 // dxContext
    for (int i = 0; i < 6; i++) {
        *(volatile UInt32 *)(_cbHeaderVirt + 40 + 4 * i) = 0;             // mustBeZero
    }
    __asm__ volatile ("" ::: "memory");

    if (kCbDump) {
        volatile UInt32 *hd = (volatile UInt32 *)_cbHeaderVirt;
        IOLog(VGPU_FB_TAG ": cb[%llu] ctx%u dx%u len %u hdr: %08x %08x %08x %08x %08x %08x %08x %08x\n",
              (unsigned long long)(_cbId - 1), cbContext, dxContext, byteCount,
              hd[0], hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7]);
        IOLog(VGPU_FB_TAG ": cb[%llu] hdr2: %08x %08x %08x %08x\n",
              (unsigned long long)(_cbId - 1), hd[8], hd[9], hd[10], hd[11]);
        IOLog(VGPU_FB_TAG ": cb[%llu] pay: %08x %08x %08x %08x %08x %08x %08x %08x\n",
              (unsigned long long)(_cbId - 1),
              cmds != nullptr && wordCount > 0 ? cmds[0] : 0, wordCount > 1 ? cmds[1] : 0, wordCount > 2 ? cmds[2] : 0,
              wordCount > 3 ? cmds[3] : 0, wordCount > 4 ? cmds[4] : 0,
              wordCount > 5 ? cmds[5] : 0, wordCount > 6 ? cmds[6] : 0,
              wordCount > 7 ? cmds[7] : 0);
        if (wordCount > 8) {
            IOLog(VGPU_FB_TAG ": cb[%llu] pay2: %08x %08x %08x %08x\n",
                  (unsigned long long)(_cbId - 1),
                  cmds[8], wordCount > 9 ? cmds[9] : 0,
                  wordCount > 10 ? cmds[10] : 0, wordCount > 11 ? cmds[11] : 0);
        }
    }

    IOPCIDevice *pci = _pci;   // registered in probeFifo
    if (pci == nullptr) {
        return false;
    }
    UInt16 port = _svgaPortBase;
    // vm3dmp re-writes the PREPEND registers on every command append into a CB
    // (PA with cbContext OR'd into the low 6 bits). Mirror that.
    if (_cbPrependMem != nullptr) {
        IOByteCount ppLen = 0;
        IOVirtualAddress ppAddr = _cbPrependMem->getPhysicalSegment(0, &ppLen);
        if (ppAddr != 0) {
            UInt64 ppPA = (UInt64)ppAddr;
            svgaWriteRegister(port, kSvgaRegCommandHigh, (UInt32)(ppPA >> 32));
            svgaWriteRegister(port, kSvgaRegCommandLow,
                              (UInt32)(ppPA & 0xFFFFFFFFu) | (cbContext & 0x3F));
        }
    }
    svgaWriteRegister(port, kSvgaRegCommandHigh, (UInt32)(_cbHeaderPA >> 32));
    svgaWriteRegister(port, kSvgaRegCommandLow,
                      (UInt32)(_cbHeaderPA & 0xFFFFFFFFu) | (cbContext & 0x3F));

    // Poll for completion (NO_IRQ): status changes from 0 (NONE).
    for (UInt32 spin = 0; spin < 200000000u; spin++) {
        UInt32 status = h[0];
        if (status == 1) {          // SVGA_CB_STATUS_COMPLETED
            if (kCbDump) {
                IOLog(VGPU_FB_TAG ": cb[%llu] COMPLETED (errorOffset %u)\n",
                      (unsigned long long)(_cbId - 1), h[1]);
            }
            return true;
        }
        if (status == 2) {          // QUEUE_FULL: retry after a short spin
            IODelay(100);
            continue;
        }
        if (status >= 3) {          // COMMAND_ERROR / CB_HEADER_ERROR / ...
            IOLog(VGPU_FB_TAG ": cb: STATUS %u (errorOffset %u) -- full header dump://n",
                  status, h[1]);
            for (int d = 0; d < 4; d++) {
                IOLog(VGPU_FB_TAG ": cb hdr[%2u..%2u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                      d * 8, d * 8 + 7,
                      h[d * 8 + 0], h[d * 8 + 1], h[d * 8 + 2], h[d * 8 + 3],
                      h[d * 8 + 4], h[d * 8 + 5], h[d * 8 + 6], h[d * 8 + 7]);
            }
            return false;
        }
    }
    IOLog(VGPU_FB_TAG ": cb: timeout waiting for completion -- header dump://n");
    for (int d = 0; d < 4; d++) {
        IOLog(VGPU_FB_TAG ": cb hdr[%2u..%2u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
              d * 8, d * 8 + 7,
              h[d * 8 + 0], h[d * 8 + 1], h[d * 8 + 2], h[d * 8 + 3],
              h[d * 8 + 4], h[d * 8 + 5], h[d * 8 + 6], h[d * 8 + 7]);
    }
    return false;
}

bool vgpuFramebuffer::cbSubmit(const UInt32 *cmds, UInt32 wordCount) {
    return cbSubmitCtx(cmds, wordCount, 0, 0);   // plain stream on CONTEXT_0
}

// DX-command payload: SVGA_CB_FLAG_DX_CONTEXT (2) ONLY -- vm3dmp.sys never sets
// NO_IRQ (bit 0) on DX streams -- and the header dxContext field is 0.
bool vgpuFramebuffer::cbSubmitDx(const UInt32 *cmds, UInt32 wordCount, UInt32 dxCtxId) {
    // vm3dmp submits DX CBs on cbContext 1/2 (0x140013a68 checks cbContext 1 or 2
    // for the DX_CONTEXT flag); queue 0 is the device/paging queue.
    return cbSubmitCtx(cmds, wordCount, 1, dxCtxId);
}

void vgpuFramebuffer::fifoDump(volatile UInt32 *fifo, const char *when) {
    IOLog(VGPU_FB_TAG ": fifo[%s] min %u max %u next_cmd %u stop %u caps 0x%x busy %u\n",
          when,
          fifo[kSvgaFifoMin], fifo[kSvgaFifoMax],
          fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop],
          fifo[kSvgaFifoCapabilities], fifo[kSvgaFifoBusy]);
    IOLog(VGPU_FB_TAG ": fifo[%s] 3d hw version 0x%x\n", when, fifo[kSvgaFifo3dHwVersion]);

    UInt32 nonZero = 0;
    for (UInt32 i = 0; i < kSvgaFifo3dCapsCount; i++) {
        if (fifo[kSvgaFifo3dCaps + i] != 0) {
            nonZero++;
        }
        if (i % 8 == 0) {
            IOLog(VGPU_FB_TAG ": 3dcaps[%s %3u..%3u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                  when, i, i + 7,
                  fifo[kSvgaFifo3dCaps + i],     fifo[kSvgaFifo3dCaps + i + 1],
                  fifo[kSvgaFifo3dCaps + i + 2], fifo[kSvgaFifo3dCaps + i + 3],
                  fifo[kSvgaFifo3dCaps + i + 4], fifo[kSvgaFifo3dCaps + i + 5],
                  fifo[kSvgaFifo3dCaps + i + 6], fifo[kSvgaFifo3dCaps + i + 7]);
        }
    }
    IOLog(VGPU_FB_TAG ": 3d capabilities advertised: %u of %u non-zero\n",
          nonZero, kSvgaFifo3dCapsCount);
}

bool vgpuFramebuffer::start(IOService *provider) {
    IOLog(VGPU_FB_TAG ": start, provider=%s\n",
          provider != nullptr && provider->getMetaClass() != nullptr
              ? provider->getMetaClass()->getClassName() : "(null)");

    if (!super::start(provider)) {
        IOLog(VGPU_FB_TAG ": super::start() returned false\n");
        return false;
    }

    //
    // §38.19: arm the GuestRpc heartbeat FIRST, before touching the device.
    //
    // The host expires its Tools heartbeat timer ~20 s after the last GuestRpc it saw, then
    // declares the guest display dead and tears down the legacy RegisterMode screen in favour
    // of a 1024x512 ScreenTarget of its own making. Measured load order on this box: the
    // switch happens at boot+18 s, this start() runs at boot+19.6 s -- we have been losing
    // that race by ~1.6 s every single boot, which is why §37/§38 never saw a lit screen.
    //
    // The heartbeat uses the backdoor port, which needs no SVGA FIFO, no modesetting and no
    // aperture, so it can start right here. Everything later in start() can fail from here on
    // and the display will still stay alive.
    //
    startRpcHeartbeat();

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr) {
        IOLog(VGPU_FB_TAG ": provider is not an IOPCIDevice\n");
        return false;
    }

    UInt16 portBase = 0;
    if (!readDeviceMode(pci, &portBase)) {
        return false;
    }

    // The aperture is exactly the region the adapter scans out: VRAM, starting at
    // SVGA_REG_FB_OFFSET, as many bytes as one pitch times one screen. Offering more than
    // that would be harmless, but offering exactly this makes the mapping honest and keeps
    // the window server from believing it can address memory that is off screen.
    _apertureLength = (IOByteCount)_devicePitch * _deviceHeight;

    IODeviceMemory *vram = pci->getDeviceMemoryWithIndex(kVgpuVramBarIndex);
    if (vram == nullptr) {
        IOLog(VGPU_FB_TAG ": provider has no BAR%u\n", kVgpuVramBarIndex);
        return false;
    }
    if ((IOByteCount)_deviceFbOffset + _apertureLength > vram->getLength()) {
        IOLog(VGPU_FB_TAG ": visible region 0x%x+%llu does not fit in BAR%u (%llu bytes)\n",
              _deviceFbOffset, (unsigned long long)_apertureLength, kVgpuVramBarIndex,
              (unsigned long long)vram->getLength());
        return false;
    }

    _aperture = IODeviceMemory::withSubRange(vram, _deviceFbOffset, _apertureLength);
    if (_aperture == nullptr) {
        IOLog(VGPU_FB_TAG ": IODeviceMemory::withSubRange failed\n");
        return false;
    }

    _apertureBase = _aperture->getPhysicalAddress();
    _currentMode  = 1;

    IOLog(VGPU_FB_TAG ": inheriting %ux%u pitch %u at VRAM offset 0x%x\n",
          _deviceWidth, _deviceHeight, _devicePitch, _deviceFbOffset);
    IOLog(VGPU_FB_TAG ": aperture phys 0x%llx length %llu\n",
          (unsigned long long)_apertureBase, (unsigned long long)_apertureLength);

    // The name the system reports for this adapter. Without it system_profiler falls back to
    // whatever the firmware left in the PCI "model" property, which is nothing useful on a
    // virtual device.
    setProperty("model", "VMware SVGA 3D GPU");

    // Bring the command FIFO up and take the measured SVGA3D state. On a macOS guest no
    // driver has ever configured it; doing so here is the step that turns the 3D capability
    // (see docs/vmware-svga-capabilities.md) into something a driver can submit commands to.
    probeFifo(pci);

    setProperty("IOFramebufferBacking", "vgpu");
    IOLog(VGPU_FB_TAG ": started\n");
    return true;
}

void vgpuFramebuffer::stop(IOService *provider) {
    IOLog(VGPU_FB_TAG ": stop\n");

    if (_presentCall != nullptr) {
        thread_call_cancel(_presentCall);
        thread_call_free(_presentCall);
        _presentCall = nullptr;
    }
    _presentFifo = nullptr;
    if (_fifoMap != nullptr) {
        _fifoMap->release();
        _fifoMap = nullptr;
    }
    if (_vramMap != nullptr) {
        _vramMap->release();
        _vramMap = nullptr;
        _vramVirt = nullptr;
    }
    if (_stduPtMem != nullptr) {
        if (_stduPtPrepared) {
            _stduPtMem->complete(kIODirectionInOut);
            _stduPtPrepared = false;
        }
        _stduPtMem->release();
        _stduPtMem = nullptr;
        _stduPtVirt = nullptr;
    }
    if (_stduMem != nullptr) {
        if (_stduPrepared) {
            _stduMem->complete(kIODirectionInOut);
            _stduPrepared = false;
        }
        _stduMem->release();
        _stduMem = nullptr;
        _stduVirt = nullptr;
    }
    if (_ctxPage != nullptr) {
        _ctxPage->release();
        _ctxPage = nullptr;
    }
    if (_dxMem != nullptr) {
        _dxMem->release();
        _dxMem = nullptr;
        _dxVirt = nullptr;
    }
    if (_dxPtMem != nullptr) {
        _dxPtMem->release();
        _dxPtMem = nullptr;
    }
    if (_cotableMem != nullptr) {
        _cotableMem->release();
        _cotableMem = nullptr;
    }
    if (_cotablePtMem != nullptr) {
        _cotablePtMem->release();
        _cotablePtMem = nullptr;
    }
    if (_aperture != nullptr) {
        _aperture->release();
        _aperture = nullptr;
    }
    if (_gbMem != nullptr) {
        _gbMem->release();
        _gbMem = nullptr;
        _gbVirt = nullptr;
    }
    super::stop(provider);
}

IOReturn vgpuFramebuffer::enableController() {
    IOLog(VGPU_FB_TAG ": enableController\n");
    return kIOReturnSuccess;
}

bool vgpuFramebuffer::isConsoleDevice() {
    // Deliberate, and the point of this revision: the console is what the window server
    // renders into, so claiming it is what puts this kext on screen.
    return true;
}

IODeviceMemory *vgpuFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture) {
        return nullptr;
    }
    if (_aperture == nullptr) {
        return nullptr;
    }
    // Hand the caller a reference of its own. The aperture is created once in start() and
    // released once in stop(), so the pointer this returns has to carry an extra retain:
    // IOGraphicsFamily releases what it is given, every time a display comes up or changes
    // mode, and returning the bare member let that release consume the kext's own reference.
    // That is an over-release of an IOSubMemoryDescriptor, which panics the kernel with
    // "A kext releasing a(n) IOSubMemoryDescriptor has corrupted the registry" -- measured,
    // from WindowServer's own thread, about a minute into the first boot with the takeover
    // in place.
    //
    // (OSObject::retain() returns void, so this is two statements and not a tail call.)
    _aperture->retain();
    return _aperture;
}

const char *vgpuFramebuffer::getPixelFormats() {
    return IO32BitDirectPixels;
}

IOItemCount vgpuFramebuffer::getDisplayModeCount() {
    // Exactly one mode: the one the device is already scanning out. Advertising anything else
    // would be a lie until setDisplayMode can reprogram the device.
    return _deviceModeValid ? 1 : 0;
}

IOReturn vgpuFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (allDisplayModes == nullptr) {
        return kIOReturnBadArgument;
    }
    allDisplayModes[0] = 1;
    return kIOReturnSuccess;
}

IOReturn vgpuFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                      IODisplayModeInformation *info) {
    if (displayMode != 1 || info == nullptr || !_deviceModeValid) {
        return kIOReturnBadArgument;
    }
    bzero(info, sizeof (*info));
    info->nominalWidth  = _deviceWidth;
    info->nominalHeight = _deviceHeight;
    info->refreshRate   = 60 << 16;
    info->maxDepthIndex = 0;
    return kIOReturnSuccess;
}

UInt64 vgpuFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    // Obsolete; the reference implementation returns zero and so do we.
    (void)displayMode;
    (void)depth;
    return 0;
}

IOReturn vgpuFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                             IOPixelAperture aperture, IOPixelInformation *pixelInfo) {
    if (displayMode != 1 || depth != 0 || pixelInfo == nullptr || !_deviceModeValid) {
        return kIOReturnBadArgument;
    }
    if (aperture != kIOFBSystemAperture) {
        return kIOReturnUnsupportedMode;
    }

    bzero(pixelInfo, sizeof (*pixelInfo));
    // The device's pitch, not a computed one. They happen to agree at 1024 wide (4096), which
    // is why a mismatch here would be easy to miss and would show up as a diagonal skew.
    pixelInfo->bytesPerRow      = _devicePitch;
    pixelInfo->bitsPerPixel     = kDepth;
    pixelInfo->pixelType        = kIORGBDirectPixels;
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->componentCount   = 3;
    pixelInfo->componentMasks[0] = 0xFF0000;   // R
    pixelInfo->componentMasks[1] = 0x00FF00;   // G
    pixelInfo->componentMasks[2] = 0x0000FF;   // B
    pixelInfo->activeWidth      = _deviceWidth;
    pixelInfo->activeHeight     = _deviceHeight;
    strncpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof (pixelInfo->pixelFormat));
    return kIOReturnSuccess;
}

IOReturn vgpuFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    if (displayMode != nullptr) {
        *displayMode = _currentMode;
    }
    if (depth != nullptr) {
        *depth = 0;
    }
    return kIOReturnSuccess;
}

IOReturn vgpuFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    if (displayMode != 1 || depth != 0) {
        // A real modeset means programming SVGA_REG_WIDTH / HEIGHT / BITS_PER_PIXEL and
        // re-reading SVGA_REG_BYTES_PER_LINE and SVGA_REG_FB_OFFSET. Not yet: the point of
        // this revision is to inherit the mode, not to own it.
        return kIOReturnUnsupportedMode;
    }
    _currentMode = displayMode;
    return kIOReturnSuccess;
}

IOReturn vgpuFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
    // No hardware cursor: tell the truth and let IOFramebuffer drive it in software.
    if (attribute == kIOHardwareCursorAttribute) {
        if (value != nullptr) {
            *value = 0;
        }
        return kIOReturnSuccess;
    }
    return super::getAttribute(attribute, value);
}

IOReturn vgpuFramebuffer::setCursorImage(void *cursorImage) {
    (void)cursorImage;
    return kIOReturnUnsupported;
}

IOReturn vgpuFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
    (void)x;
    (void)y;
    (void)visible;
    return kIOReturnUnsupported;
}

void vgpuFramebuffer::flushCursor() {
}
