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
    kSvgaRegConfigDone   = 0x14,   // 20: guest sets 1 when the FIFO is configured
    kSvgaRegMemRegs      = 0x1E,   // 30: number of FIFO registers the host expects
    kSvgaRegDevCap       = 0x34,   // 52: write a SVGA3D_DEVCAP index, read its value
};

//
// FIFO registers are dwords inside the device memory region that SVGA_REG_MEM_START points
// at. The first four (MIN/MAX/NEXT_CMD/STOP) are BYTE offsets into that region; every other
// index is a dword index. The register block occupies the first SVGA_FIFO_MIN bytes, so the
// guest chooses how many registers exist by where it starts the command data: leaving room
// for SVGA_FIFO_EXTENDED_MANDATORY_REGS (288) dwords is what makes the extended set --
// including the 3D hardware version and the 256-entry 3D capability block -- exist at all.
//
// The offsets below are taken from the SVGA II register definitions shared by Linux's
// vmwgfx (drivers/gpu/drm/vmwgfx/device_include/svga_reg.h) and Xorg's vmware driver, and
// they differ from an earlier revision of this file, which carried 3D_HWVERSION=28 and
// 3D_CAPS=34 from a misremembered pre-extended-FIFO layout. With those values everything
// read as zero regardless of what the host published -- which is exactly what we saw.
//
enum {
    kSvgaFifoMin              = 0,      // byte offset: start of the command data area
    kSvgaFifoMax              = 1,      // byte offset: end of the FIFO
    kSvgaFifoNextCmd          = 2,      // byte offset: host read position
    kSvgaFifoStop             = 3,      // byte offset: guest write position
    kSvgaFifoCapabilities     = 4,      // dword index: SVGA_FIFO_CAP_* bits
    kSvgaFifoFlags            = 5,
    kSvgaFifoFence            = 6,      // dword index: last completed fence value
    kSvgaFifo3dHwVersion      = 7,      // dword index: host's SVGA3D protocol version
    kSvgaFifoReserved         = 14,     // dword index: bytes past NEXT_CMD being written
    kSvgaFifo3dCaps           = 32,     // dword index: start of the 3D capability block
    kSvgaFifo3dCapsCount      = 256,    // through SVGA_FIFO_3D_CAPS_LAST
    kSvgaFifoGuest3dHwVersion = 288,    // dword index: guest writes its version here
    kSvgaFifoBusy             = 290,

    kSvgaFifoExtendedMandatoryRegs = 288,  // registers to reserve for the extended set
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
    kSvga3dCmdDefineGbSurface = 1097,
    kSvga3dCmdDxDefineContext = 1143,
    kSvga3dCmdDefineGbScreenTarget = 1124,
    kSvga3dCmdDestroyGbScreenTarget = 1125,
    kSvga3dCmdBindGbScreenTarget   = 1126,
    kSvga3dCmdUpdateGbScreenTarget = 1127,
    kSvga3dCmdSurfaceDestroy       = 1041,
    kSvga3dCmdDestroyGbMob         = 1094,
    kSvga3dCmdDxBindContext        = 1145,
    kSvga3dCmdDxSetCotable         = 1207,
    kSvga3dCmdDxDefineRenderTargetView = 1187,
    kSvga3dCmdDxSetRenderTargets   = 1161,
    kSvga3dCmdDxSetViewports       = 1174,
    kSvga3dCmdDxSetScissorRects    = 1175,
    kSvga3dCmdDxClearRenderTargetView = 1176,
    kSvga3dCmdReadbackGbSurface    = 1104,
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

// kFbStage gates the FIFO bring-up itself (bisecting the boot-freeze):
//   0 = read-only probe: register reads and dumps only, no FIFO writes, no fence
//   2 = FIFO init only (MIN/MAX/CONFIG_DONE), no guest commands at all
//   3 = init + one fence command
//   4 = init + fence + full-screen UPDATE probe
//   5 = init + fence + screen-object present path (DEFINE_SCREEN, DEFINE_GMRFB over the
//       framebuffer, BLIT_GMRFB_TO_SCREEN) + the periodic present timer
//
// MEASURED (2026-10-06 bisect): with 3D enabled, stage 2 alone freezes presentation --
// the moment the guest writes CONFIG_DONE, the MKS switches to the screen-target
// presentation model and the legacy framebuffer auto-scanout is disabled. SVGA_CMD_UPDATE
// (acked!) does not restore it. So FIFO ownership REQUIRES implementing the screen-target
// present path (DEFINE_SCREEN / GB screen target / blits) before it can be safe.
// Stage 5 is that present path.
// 2026-10-06 evening: stage 5 (legacy DEFINE_SCREEN + GMRFB blits over the framebuffer)
// runs fully -- DEFINE_SCREEN/DEFINE_GMRFB/BLIT_GMRFB_TO_SCREEN all fence-acked, the 33 ms
// re-present thread_call fires and the host consumes every blit -- yet the display keeps
// showing the last legacy frame. Conclusion: on this vGPU10 device the legacy screen-object
// path does not feed the display pipeline; presentation must go through GB screen targets
// (STDU) -- see docs/vmware-svga-capabilities.md. Ship kFbStage=0 (working display) until
// that path exists.
// kFbStage gates the FIFO bring-up itself (bisecting the boot-freeze):
//   0 = read-only probe: register reads and dumps only, no FIFO writes, no fence
//   2 = FIFO init only (MIN/MAX/CONFIG_DONE), no guest commands at all
//   3 = init + one fence command
//   4 = init + fence + full-screen UPDATE probe
//   5 = init + fence + legacy screen-object present path (DEFINE_SCREEN, DEFINE_GMRFB over
//       the framebuffer, BLIT_GMRFB_TO_SCREEN) + the periodic present timer
static const UInt32 kFbStage = 2;

// kGbStage gates the GB object stack (see gbBringUp); 5 adds the STDU present path.
static const UInt32 kGbStage = 4;

// kDxTest runs the DX clear/readback round-trip experiment after the present path is up.
// Measured 2026-10-06: with the test present, the host aborts FIFO processing at
// DEFINE_GB_SCREENTARGET (deterministically, next/stop frozen mid-stream) even though the
// identical byte stream passed in earlier boots and the test itself runs only afterwards.
// Root cause unknown -- bisect with this switch next session.
static const UInt32 kDxTest = 1;

// STDU object teardown before the defines (see the bisect note in stduBringUp).
static const UInt32 kStduTeardown = 0;

// MOB id of the DX context state block.
static const UInt32 kCtxMobId = 3;

// Periodic full-screen re-present interval, in milliseconds. The GMRFB points straight at
// the framebuffer in BAR1, so each present is a host-side DMA with no guest copy; the cost
// is bounded and dirty-tracking can replace this later.
static const UInt32 kPresentIntervalMs = 33;

// One 4 KiB page of PPN64 entries describes up to 512 pages = 2 MiB.
static const IOByteCount kGbPageBytes        = 4096;
static const UInt32     kGbPageShift         = 12;
static const UInt32     kGbDataPages         = 4;    // 16 KiB data MOB
static const UInt32     kGbMobTableEntries   = 1024; // 16 KiB
static const UInt32     kGbSurfaceTableEnts  = 64;   // 64 * 72 = 4608 bytes

// Sizes derived from the SVGAOTable*Entry structures (packed, 8-byte trailing alignment on
// the mob entry; the surface entry is 72 bytes exactly as vmwgfx's headers define it).
static const UInt32 kGbMobEntryBytes         = 16;
static const UInt32 kGbSurfaceEntryBytes     = 72;

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
static const UInt32     kGbDxCtxTableEnts    = 256;  // 2 KiB
static const UInt32     kGbDxCtxEntryBytes   = 8;

//
// Screen target (STDU) present path. The screen target's content is a GB surface of the
// full display size; its data lives in a dedicated 3 MB MOB (PT64_1: one root PT page
// pointing at two second-level PT pages covering 768 pages). The present loop copies the
// framebuffer (VRAM) into that memory and issues UPDATE_GB_SCREENTARGET -- the host reads
// the surface data straight out of guest RAM.
//
static const UInt32     kStduWidth           = 1024;
static const UInt32     kStduHeight          = 768;
static const IOByteCount kStduBytes          = (IOByteCount)kStduWidth * kStduHeight * 4;
static const UInt32     kStduPages           = (UInt32)(kStduBytes / kGbPageBytes); // 768
static const UInt32     kStduL1Pages         = (kStduPages + 511) / 512;            // 2
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

static UInt32 svgaReadRegister(UInt16 portBase, UInt32 index) {
    vgpuOut32((UInt16)(portBase + kSvgaIndexPort), index);
    return vgpuIn32((UInt16)(portBase + kSvgaValuePort));
}

static void svgaWriteRegister(UInt16 portBase, UInt32 index, UInt32 value) {
    vgpuOut32((UInt16)(portBase + kSvgaIndexPort), index);
    vgpuOut32((UInt16)(portBase + kSvgaValuePort), value);
}

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
    void dxReadbackTest(volatile UInt32 *fifo);

    void presentSetup(volatile UInt32 *fifo);
    void presentTick(void);
    static void presentTickC(thread_call_param_t param0, thread_call_param_t param1);
    void schedulePresent(void);
    thread_call_t _presentCall = nullptr;
    UInt32 _presentTicks = 0;
    volatile UInt32 *_presentFifo = nullptr;
    IOMemoryMap *_fifoMap = nullptr;

    // STDU present state.
    bool _stduActive = false;
    IOBufferMemoryDescriptor *_stduMem = nullptr;   // 3 MB surface backing store
    void                    *_stduVirt = nullptr;
    UInt64                   _stduPhys = 0;
    IOBufferMemoryDescriptor *_stduPtMem = nullptr; // 16 KiB contiguous: root + 2 L1 PT pages
    void                    *_stduPtVirt = nullptr;
    UInt64                   _stduPtPhys = 0;
    IOMemoryMap             *_vramMap = nullptr;     // kernel mapping of the framebuffer
    void                    *_vramVirt = nullptr;
    UInt32                   _surfaceId = 1;
    UInt32                   _stduMobId = 2;
    UInt32                   _stduStid = 0;
    IOBufferMemoryDescriptor *_ctxPage = nullptr;   // DX context state MOB (64 KiB)
    UInt32                   _ctxMobId = 3;
    IOBufferMemoryDescriptor *_dxMem = nullptr;     // DX test surface memory
    void                    *_dxVirt = nullptr;
    IOBufferMemoryDescriptor *_dxPtMem = nullptr;   // DX test surface PT page

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
    UInt32 memStart = svgaReadRegister(_svgaPortBase, kSvgaRegMemStart);
    UInt32 memSize  = svgaReadRegister(_svgaPortBase, kSvgaRegMemSize);
    UInt32 memRegs  = svgaReadRegister(_svgaPortBase, kSvgaRegMemRegs);
    IOLog(VGPU_FB_TAG ": svga mem start 0x%x size 0x%x regs %u\n", memStart, memSize, memRegs);
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
    if (kFbStage == 0) {
        // Read-only bisect mode: no FIFO writes at all.
        IOLog(VGPU_FB_TAG ": probeFifo: read-only (kFbStage=0)\n");
        map->release();
        return;
    }
    if (fifo[kSvgaFifoMin] != 0 && fifo[kSvgaFifoMin] != kSvgaFifoExtendedMandatoryRegs * 4) {
        IOLog(VGPU_FB_TAG ": FIFO configured by another driver (min %u), leaving it alone\n",
              fifo[kSvgaFifoMin]);
        map->release();
        return;
    }
    // min == 0 is a fresh device; min == 1152 is ours surviving from a previous boot (the
    // SVGA device is not reset unless the VM powers off). Either way the command stream is
    // empty (NEXT_CMD == STOP), so the init sequence below is safe to run again -- and it
    // has to be run: the host republishes its FIFO capabilities on the CONFIG_DONE
    // transition, and they read zero on a later boot until it does.

    if (fifo[kSvgaFifoMin] == 0 || fifo[kSvgaFifoMin] == kSvgaFifoExtendedMandatoryRegs * 4) {
        // 288 registers * 4 bytes of register space, plus the spec's minimum 10 KB of data.
        UInt32 regBytes  = kSvgaFifoExtendedMandatoryRegs * sizeof (UInt32);
        UInt32 fifoBytes = (memSize != 0 && memSize <= (UInt32)map->getLength()) ? memSize
                                                                                 : (UInt32)map->getLength();
        if (fifoBytes < regBytes + 10 * 1024) {
            IOLog(VGPU_FB_TAG ": FIFO area %u bytes too small for the extended registers\n", fifoBytes);
            map->release();
            return;
        }

        svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 0);
        fifo[kSvgaFifoMin]     = regBytes;
        fifo[kSvgaFifoMax]     = fifoBytes;
        fifo[kSvgaFifoNextCmd] = regBytes;
        fifo[kSvgaFifoStop]    = regBytes;
        __asm__ volatile ("" ::: "memory");

        // Display topology BEFORE taking the FIFO: a real driver declares its displays
        // through these registers before creating screen targets, and writing them while
        // the MKS is processing our FIFO wedges its command processing (2026-10-07
        // matrix). Deselect the window afterwards, like vmwgfx does.
        svgaWriteRegister(_svgaPortBase, 35, 0);                  // SVGA_REG_DISPLAY_ID
        svgaWriteRegister(_svgaPortBase, 36, 1);                  // SVGA_REG_DISPLAY_IS_PRIMARY
        svgaWriteRegister(_svgaPortBase, 37, 0);                  // SVGA_REG_DISPLAY_POSITION_X
        svgaWriteRegister(_svgaPortBase, 38, 0);                  // SVGA_REG_DISPLAY_POSITION_Y
        svgaWriteRegister(_svgaPortBase, 39, _deviceWidth);       // SVGA_REG_DISPLAY_WIDTH
        svgaWriteRegister(_svgaPortBase, 40, _deviceHeight);      // SVGA_REG_DISPLAY_HEIGHT
        svgaWriteRegister(_svgaPortBase, 35, 0xFFFFFFFF);         // SVGA_ID_INVALID: deselect

        svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 1);
        IOLog(VGPU_FB_TAG ": FIFO configured: regs %u bytes (%u dwords), data %u..%u, CONFIG_DONE=1\n",
              regBytes, kSvgaFifoExtendedMandatoryRegs, regBytes, fifoBytes);
        fifoDump(fifo, "after ");
    }

    // Announce the guest protocol version. On a vGPU10 (GBOBJECTS) device the legacy
    // SVGA_FIFO_3D_HWVERSION register stays zero -- the authoritative 3D check there is
    // the DEV_CAP backdoor below, exactly as Linux's vmwgfx does in vmw_fifo_have_3d().
    fifo[kSvgaFifoGuest3dHwVersion] = kSvga3dHwVersionCurrent;
    __asm__ volatile ("" ::: "memory");
    IOLog(VGPU_FB_TAG ": guest 3D version announced as 0x%x; legacy 3D_HWVERSION reads 0x%x "
          "(expected zero on a vGPU10 device)\n",
          kSvga3dHwVersionCurrent, fifo[kSvgaFifo3dHwVersion]);

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
        dxReadbackTest(fifo);
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

void vgpuFramebuffer::presentTick(void) {
    if (_presentFifo == nullptr || !_deviceModeValid) {
        return;
    }
    _presentTicks++;
    if (_presentTicks <= 3 || _presentTicks % 300 == 0) {
        IOLog(VGPU_FB_TAG ": present tick %u (next %u stop %u)\n", _presentTicks,
              _presentFifo[kSvgaFifoNextCmd], _presentFifo[kSvgaFifoStop]);
    }
    if (_stduActive && kDxTest && _presentTicks == 600) {
        IOLog(VGPU_FB_TAG ": present tick 600: running the DX round-trip test\n");
        dxReadbackTest(_presentFifo);
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
            memcpy(_stduVirt, _vramVirt, kStduBytes);
        }

        // Belt and braces: SURFACE_DMA (guest mobid 2 -> host surface sid 2) now that the
        // MOB page tables are PT64_2, followed by UPDATE_GB_IMAGE (re-read the MOB), and
        // then the screen-target update below. Any one of these being the effective
        // content path should light the display up.
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
        dma[9] = 0;                     // box.x
        dma[10] = 0;                    // box.y
        dma[11] = 0;                    // box.z
        dma[12] = kStduWidth;           // box.w
        dma[13] = kStduHeight;          // box.h
        dma[14] = 1;                    // box.d
        dma[15] = 0;                    // box.srcx
        dma[16] = 0;                    // box.srcy
        dma[17] = 0;                    // box.srcz
        dma[18] = 12;                   // suffix.suffixSize
        dma[19] = (UInt32)kStduBytes;   // suffix.maximumOffset
        dma[20] = 0;                    // suffix.flags
        fifoSubmitWords(_presentFifo, dma, 21);

        UInt32 img[11];
        img[0] = 1101;                  // SVGA_3D_CMD_UPDATE_GB_IMAGE
        img[1] = 36;
        img[2] = _surfaceId;            // image.sid
        img[3] = 0;                     // image.face
        img[4] = 0;                     // image.mipmap
        img[5] = 0;                     // box.x
        img[6] = 0;                     // box.y
        img[7] = 0;                     // box.z
        img[8] = kStduWidth;            // box.w
        img[9] = kStduHeight;           // box.h
        img[10] = 1;                    // box.d
        fifoSubmitWords(_presentFifo, img, 11);

        UInt32 upd[7];
        upd[0] = kSvga3dCmdUpdateGbScreenTarget;
        upd[1] = 20;
        upd[2] = _stduStid;
        upd[3] = 0;
        upd[4] = 0;
        upd[5] = kStduWidth;
        upd[6] = kStduHeight;
        fifoSubmitWords(_presentFifo, upd, 7);
        return;
    }

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
// Submission follows the FIFO's reserve protocol (SVGA_FIFO_CAP_RESERVE is set on this
// host): announce the byte count in SVGA_FIFO_RESERVED, write the command dwords at
// NEXT_CMD, then advance NEXT_CMD and clear RESERVED. The command stream is empty at this
// point (NEXT_CMD == STOP), so no wrap handling is needed for the first submission; a
// command that would straddle FIFO_MAX is refused rather than wrapped.
//
void vgpuFramebuffer::fenceTest(volatile UInt32 *fifo) {
    const UInt32 caps = fifo[kSvgaFifoCapabilities];
    if ((caps & kSvgaFifoCapFence) == 0) {
        IOLog(VGPU_FB_TAG ": fence test skipped: host does not advertise SVGA_FIFO_CAP_FENCE\n");
        return;
    }

    const UInt32 next = fifo[kSvgaFifoNextCmd];
    const UInt32 stop = fifo[kSvgaFifoStop];
    const UInt32 fmax = fifo[kSvgaFifoMax];
    const UInt32 bytesFree = (next >= stop) ? (fmax - next) : (stop - next);
    if (bytesFree < 8) {
        IOLog(VGPU_FB_TAG ": fence test skipped: only %u bytes free before FIFO_MAX (wrap not implemented)\n",
              bytesFree);
        return;
    }

    const UInt32 fenceValue = 0x31415926;
    const bool  canReserve  = (caps & kSvgaFifoCapReserve) != 0;
    if (canReserve) {
        fifo[kSvgaFifoReserved] = 8;
    }
    fifo[(next / sizeof (UInt32))]     = kSvgaCmdFence;
    fifo[(next / sizeof (UInt32)) + 1] = fenceValue;
    __asm__ volatile ("" ::: "memory");
    fifo[kSvgaFifoNextCmd] = next + 8;
    if (canReserve) {
        fifo[kSvgaFifoReserved] = 0;
    }
    IOLog(VGPU_FB_TAG ": fence 0x%x submitted at offset %u (reserve=%d), polling...\n",
          fenceValue, next, canReserve);

    // The host acks by writing the value into SVGA_FIFO_FENCE once it has processed every
    // command up to and including ours. Poll with a generous bound; IOLog inside the loop
    // would slow the success path, so only the outcome is logged.
    const UInt64 spinBound = 400000000ULL;
    for (UInt64 spins = 0; spins < spinBound; spins++) {
        if (fifo[kSvgaFifoFence] == fenceValue) {
            IOLog(VGPU_FB_TAG ": FIFO command stream VERIFIED: fence 0x%x acked "
                  "(next_cmd %u stop %u, %llu spins)\n",
                  fenceValue, fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop], spins);
            return;
        }
    }
    IOLog(VGPU_FB_TAG ": fence NOT acked within %llu spins (next_cmd %u stop %u fence 0x%x)\n",
          spinBound, fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop], fifo[kSvgaFifoFence]);
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
    const UInt32 next = fifo[kSvgaFifoNextCmd];
    const UInt32 stop = fifo[kSvgaFifoStop];
    const UInt32 fmax = fifo[kSvgaFifoMax];
    const UInt32 fmin = fifo[kSvgaFifoMin];
    const UInt32 bytesFree = (next >= stop) ? (fmax - next) : (stop - next);
    const IOByteCount byteCount = (IOByteCount)wordCount * sizeof (UInt32);
    if (byteCount > bytesFree) {
        IOLog(VGPU_FB_TAG ": fifo submit refused: %u bytes needed, %u free\n",
              (unsigned)byteCount, bytesFree);
        return false;
    }
    const UInt32 caps = fifo[kSvgaFifoCapabilities];
    const bool canReserve = (caps & kSvgaFifoCapReserve) != 0;
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
        fifo[kSvgaFifoNextCmd] = next + byteCount;
        if (canReserve) {
            fifo[kSvgaFifoReserved] = 0;
        }
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
    fifo[kSvgaFifoNextCmd] = fmin + byteCount;
    return true;
}

bool vgpuFramebuffer::fenceAck(volatile UInt32 *fifo, UInt32 *seq) {
    *seq = *seq + 1;
    UInt32 words[2] = { kSvgaCmdFence, *seq };
    if (!fifoSubmitWords(fifo, words, 2)) {
        return false;
    }
    const UInt64 spinBound = 400000000ULL;
    for (UInt64 spins = 0; spins < spinBound; spins++) {
        if (fifo[kSvgaFifoFence] == *seq) {
            return true;
        }
    }
    IOLog(VGPU_FB_TAG ": fence %u NOT acked (next_cmd %u stop %u)\n",
          *seq, fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop]);
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
    words[7] = kSvga3dMobFmtPt64_0;
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
    words[7] = kSvga3dMobFmtPt64_0;
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
    UInt32 mobDepth = kSvga3dMobFmtPt64_0;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &dataBytes, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &ppnDataPt, 8);
    __asm__ volatile ("" ::: "memory");

    // DEFINE_GB_MOB64: {mobid, ptDepth, base PPN64, sizeInBytes}
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = dataMobId;
    words[3] = kSvga3dMobFmtPt64_0;
    memcpy(&words[4], &ppnDataPt, 8);
    words[6] = dataBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DEFINE_GB_MOB64 refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: data MOB %u defined (ppn %llu, %u bytes, PT64_0)\n",
          dataMobId, (unsigned long long)ppnDataPt, dataBytes);
    }

    // DEFINE_GB_SURFACE: {sid, surfaceFlags(64), format, numMipLevels, multisampleCount,
    //                     autogenFilter, size{w,h,d}} -- 40 bytes, and the header size
    // must say so exactly: the host aborts FIFO processing on a size mismatch.
    if (kGbStage >= 3) {
    const UInt64 surfaceFlags = kSvga3dSurfaceHintDynamic | kSvga3dSurfaceHintTexture;
    const UInt32 width = 256, height = 256, depth = 1;
    words[0] = kSvga3dCmdDefineGbSurface;
    words[1] = 40;
    words[2] = surfaceId;
    memcpy(&words[3], &surfaceFlags, 8);
    words[5] = kSvga3dSurfaceFormatX8R8G8B8;
    words[6] = 1;    // numMipLevels
    words[7] = 1;    // multisampleCount
    words[8] = 0;    // autogenFilter = SVGA3D_TEX_FILTER_NONE
    words[9] = width;
    words[10] = height;
    words[11] = depth;
    if (!fifoSubmitWords(fifo, words, 12) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DEFINE_GB_SURFACE refused\n");
        return;
    }

    // The guest fills the surface's OTable entry too.
    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)surfaceId * kGbSurfaceEntryBytes;
    UInt32 fmt = kSvga3dSurfaceFormatX8R8G8B8;
    UInt32 mips = 1, ms = 1, filter = 0;
    memcpy((void *)(entry + 0), &fmt, 4);
    memcpy((void *)(entry + 4), &surfaceFlags, 8);
    memcpy((void *)(entry + 12), &mips, 4);
    memcpy((void *)(entry + 16), &ms, 4);
    memcpy((void *)(entry + 20), &filter, 4);
    memcpy((void *)(entry + 24), &width, 4);
    memcpy((void *)(entry + 28), &height, 4);
    memcpy((void *)(entry + 32), &depth, 4);
    memcpy((void *)(entry + 36), &dataMobId, 4);
    memcpy((void *)(entry + 40), &mips, 4);           // arraySize
    UInt32 pitch = width * 4;
    memcpy((void *)(entry + 44), &pitch, 4);          // mobPitch
    UInt64 zero64 = 0;
    memcpy((void *)(entry + 48), &zero64, 8);         // surface2Flags
    bzero((void *)(entry + 56), 16);                  // multisamplePattern, quality, pads
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
    words[0] = kSvga3dCmdSetOtableBase64;
    words[1] = 24;
    words[2] = kSvgaOtableDxContext;
    memcpy(&words[3], &ppnDxCtxPt, 8);
    words[5] = dxCtxTableBytes;
    words[6] = 0;
    words[7] = kSvga3dMobFmtPt64_0;
    if (!fifoSubmitWords(fifo, words, 8) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DXCONTEXT object table base refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: DXCONTEXT otable base set (ppn %llu, %u bytes)\n",
          (unsigned long long)ppnDxCtxPt, dxCtxTableBytes);

    // Context state MOB: the DX context only becomes live once its state mob is bound
    // with DX_BIND_CONTEXT. 64 KiB scattered, PT64_1 (one PT page).
    _ctxPage = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kGbPageBytes * 16, 0xFFFULL);
    if (_ctxPage == nullptr) {
        IOLog(VGPU_FB_TAG ": gb: could not allocate the context MOB\n");
        return;
    }
    bzero((void *)_ctxPage->map()->getVirtualAddress(), kGbPageBytes * 16);
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
    for (UInt32 i = 0; i < 16; i++) {
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
    words[6] = kGbPageBytes * 16;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: context MOB define refused\n");
        return;
    }

    // SVGA_3D_CMD_DX_DEFINE_CONTEXT: {uint32 cid} -- 4 bytes.
    words[0] = kSvga3dCmdDxDefineContext;
    words[1] = 4;
    words[2] = dxCtxId;
    if (!fifoSubmitWords(fifo, words, 3) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DX_DEFINE_CONTEXT refused\n");
        return;
    }

    // DX_BIND_CONTEXT {cid, mobid, validContents=0} -- the activation step. Without it
    // every DX command is silently ignored (CLEAR/readback results invisible, 10-07).
    words[0] = kSvga3dCmdDxBindContext;
    words[1] = 12;
    words[2] = dxCtxId;
    words[3] = kCtxMobId;
    words[4] = 0;                   // validContents: fresh context
    if (!fifoSubmitWords(fifo, words, 5) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": gb: DX_BIND_CONTEXT refused\n");
        return;
    }
    IOLog(VGPU_FB_TAG ": gb: DX CONTEXT %u created and BOUND to MOB %u -- DX command set "
          "is OPEN\n", dxCtxId, kCtxMobId);
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
void vgpuFramebuffer::stduBringUp(volatile UInt32 *fifo) {
    // The 256x256 experiment surface (kGbStage>=3) owns sid 1; the STDU surface takes a
    // fresh id so no redefinition is involved.
    _surfaceId = 2;

    // 1. Backing store (pages may be scattered; the MOB page tables handle that) and page
    //    tables (three independent pages: root + two second-level).
    _stduMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, kStduBytes, 0xFFFULL);
    if (_stduMem == nullptr) {
        IOLog(VGPU_FB_TAG ": stdu: could not allocate the %llu-byte backing store\n",
              (unsigned long long)kStduBytes);
        return;
    }
    _stduVirt = (void *)_stduMem->map()->getVirtualAddress();
    bzero(_stduVirt, kStduBytes);

    _stduPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 3 * kGbPageBytes, 0xFFFULL);
    if (_stduPtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": stdu: could not allocate the page-table pages\n");
        return;
    }
    _stduPtVirt = (void *)_stduPtMem->map()->getVirtualAddress();

    // Root page: PPNs of the two second-level pages (pages 1 and 2 of the PT allocation).
    volatile UInt64 *root = (volatile UInt64 *)_stduPtVirt;
    for (UInt32 p = 0; p < 3; p++) {
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
    volatile UInt64 *l1 = (volatile UInt64 *)((char *)_stduPtVirt + kGbPageBytes);
    for (UInt32 i = 0; i < kStduPages; i++) {
        IOVirtualAddress addr = 0;
        IOByteCount len = 0;
        addr = _stduMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &len);
        if (addr == 0 || len < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": stdu: backing-store page %u not resolvable\n", i);
            return;
        }
        l1[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");

    IOLog(VGPU_FB_TAG ": stdu: backing store %llu bytes at pt ppn %llu (PT64_2)\n",
          (unsigned long long)kStduBytes, (unsigned long long)rootPpn);

    UInt32 words[16];

    // The MOB's OTable entry FIRST -- the host snapshots the entry when it processes the
    // DEFINE, so a zero entry (write-after-define) gives it an empty object.
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = _stduMobId * kGbMobEntryBytes;
    UInt32 mobDepth = kSvga3dMobFmtPt64_2;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &kStduBytes, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &rootPpn, 8);
    __asm__ volatile ("" ::: "memory");

    // 2. DEFINE_GB_MOB64 for the backing store.
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = _stduMobId;
    words[3] = kSvga3dMobFmtPt64_2;
    memcpy(&words[4], &rootPpn, 8);
    words[6] = (UInt32)kStduBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_MOB64 refused\n");
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
    UInt32 fmt = kSvga3dSurfaceFormatX8R8G8B8;
    const UInt64 surfaceFlags = kSvga3dSurfaceHintDynamic | kSvga3dSurfaceHintTexture |
                                kSvga3dSurfaceScreentarget;
    UInt32 mips = 1, ms = 1, filter = 0;
    UInt32 depth = 1;
    UInt32 pitch = kStduWidth * 4;
    memcpy((void *)(entry + 0), &fmt, 4);
    memcpy((void *)(entry + 4), &surfaceFlags, 8);
    memcpy((void *)(entry + 12), &mips, 4);
    memcpy((void *)(entry + 16), &ms, 4);
    memcpy((void *)(entry + 20), &filter, 4);
    memcpy((void *)(entry + 24), &kStduWidth, 4);
    memcpy((void *)(entry + 28), &kStduHeight, 4);
    memcpy((void *)(entry + 32), &depth, 4);
    memcpy((void *)(entry + 36), &_stduMobId, 4);
    memcpy((void *)(entry + 40), &mips, 4);           // arraySize
    memcpy((void *)(entry + 44), &pitch, 4);          // mobPitch
    UInt64 zero64 = 0;
    memcpy((void *)(entry + 48), &zero64, 8);         // surface2Flags
    bzero((void *)(entry + 56), 16);
    __asm__ volatile ("" ::: "memory");

    words[0] = kSvga3dCmdDefineGbSurface;
    words[1] = 40;
    words[2] = _surfaceId;
    memcpy(&words[3], &surfaceFlags, 8);
    words[5] = kSvga3dSurfaceFormatX8R8G8B8;
    words[6] = 1;                   // numMipLevels
    words[7] = 1;                   // multisampleCount
    words[8] = 0;                   // autogenFilter
    words[9] = kStduWidth;
    words[10] = kStduHeight;
    words[11] = 1;                  // depth
    if (!fifoSubmitWords(fifo, words, 12) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_SURFACE refused\n");
        return;
    }

    // 5. The screen target's OTable entry (its `image` field names the content surface --
    //    this is what was missing when the screen stayed black), then
    //    DEFINE_GB_SCREENTARGET stid 0.
    volatile UInt8 *stTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffStTable);
    volatile UInt8 *stEntry = stTable + (IOByteCount)_stduStid * kGbStEntryBytes;
    memcpy((void *)(stEntry + 0), &_surfaceId, 4);    // image.sid
    memcpy((void *)(stEntry + 4), &zero64, 8);        // image.face, image.mipmap
    memcpy((void *)(stEntry + 12), &kStduWidth, 4);
    memcpy((void *)(stEntry + 16), &kStduHeight, 4);
    UInt32 zero32 = 0;
    memcpy((void *)(stEntry + 20), &zero32, 4);       // xRoot
    memcpy((void *)(stEntry + 24), &zero32, 4);       // yRoot
    memcpy((void *)(stEntry + 28), &kSvgaStflagPrimary, 4);
    memcpy((void *)(stEntry + 32), &zero32, 4);       // dpi
    bzero((void *)(stEntry + 36), 28);                // pad[7]
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

    words[0] = kSvga3dCmdDefineGbScreenTarget;
    words[1] = 28;
    words[2] = _stduStid;
    words[3] = kStduWidth;
    words[4] = kStduHeight;
    words[5] = 0;                   // xRoot
    words[6] = 0;                   // yRoot
    words[7] = kSvgaStflagPrimary;
    words[8] = 0;                   // dpi
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: DEFINE_GB_SCREENTARGET refused\n");
        // Dump the whole command stream for offline diffing: 4 dwords per line.
        for (UInt32 o = kSvgaFifoExtendedMandatoryRegs * 4; o < fifo[kSvgaFifoNextCmd]; o += 16) {
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
    memcpy(_stduVirt, _vramVirt, kStduBytes);

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
    words[6] = kStduHeight;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": stdu: UPDATE_GB_SCREENTARGET refused\n");
        return;
    }

    _stduActive = true;
    IOLog(VGPU_FB_TAG ": stdu: screen target 0 (%ux%u) bound to surface %u on MOB %u -- "
          "STDU present path is UP\n", kStduWidth, kStduHeight, _surfaceId, _stduMobId);

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
void vgpuFramebuffer::dxReadbackTest(volatile UInt32 *fifo) {
    const UInt32 cid = 1;             // created in gbBringUp (kGbStage >= 4)
    const UInt32 cotableMobId = 1;    // the 16 KiB data MOB
    const UInt32 rtvId = 0;
    const UInt32 invalidId = 0xFFFFFFFF;
    const UInt32 surfId = 3;
    const UInt32 mobId = 4;
    const UInt32 dxW = 256, dxH = 256;
    const UInt32 dxBytes = dxW * dxH * 4;          // 256 KB
    const UInt32 dxPages = dxBytes / kGbPageBytes; // 64
    const UInt32 expected = 0xFFFF00FF;            // X8R8G8B8 magenta
    UInt32 words[16];

    // Backing store: 64 pages. PT64_1 needs one PT page (64 * 8 B = 512 B fits a page).
    _dxMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, dxBytes, 0xFFFULL);
    if (_dxMem == nullptr) {
        IOLog(VGPU_FB_TAG ": dx: could not allocate the %u-byte surface memory\n", dxBytes);
        return;
    }
    _dxVirt = (void *)_dxMem->map()->getVirtualAddress();
    bzero(_dxVirt, dxBytes);

    _dxPtMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kGbPageBytes, 0xFFFULL);
    if (_dxPtMem == nullptr) {
        IOLog(VGPU_FB_TAG ": dx: could not allocate the PT page\n");
        return;
    }
    volatile UInt64 *pt = (volatile UInt64 *)_dxPtMem->map()->getVirtualAddress();
    IOByteCount segLen = 0;
    IOVirtualAddress ptAddr = _dxPtMem->getPhysicalSegment(0, &segLen);
    if (ptAddr == 0) {
        IOLog(VGPU_FB_TAG ": dx: PT page not resolvable\n");
        return;
    }
    for (UInt32 i = 0; i < dxPages; i++) {
        IOVirtualAddress addr = _dxMem->getPhysicalSegment((IOByteCount)i * kGbPageBytes, &segLen);
        if (addr == 0 || segLen < kGbPageBytes) {
            IOLog(VGPU_FB_TAG ": dx: surface page %u not resolvable\n", i);
            return;
        }
        pt[i] = (UInt64)addr >> kGbPageShift;
    }
    __asm__ volatile ("" ::: "memory");
    const UInt64 ptPpn = (UInt64)ptAddr >> kGbPageShift;

    // The MOB's OTable entry (data MOB table, mobid 4) -- before the define.
    volatile UInt8 *mobTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffMobTable);
    UInt32 mobEntryOff = mobId * kGbMobEntryBytes;
    UInt32 mobDepth = kSvga3dMobFmtPt64_1;
    memcpy((void *)(mobTable + mobEntryOff), &mobDepth, 4);
    memcpy((void *)(mobTable + mobEntryOff + 4), &dxBytes, 4);
    memcpy((void *)(mobTable + mobEntryOff + 8), &ptPpn, 8);
    __asm__ volatile ("" ::: "memory");

    // DEFINE_GB_MOB64 mobid 4.
    words[0] = kSvga3dCmdDefineGbMob64;
    words[1] = 20;
    words[2] = mobId;
    words[3] = kSvga3dMobFmtPt64_1;
    memcpy(&words[4], &ptPpn, 8);
    words[6] = dxBytes;
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DEFINE_GB_MOB64 refused\n");
        return;
    }

    // The surface's OTable entry -- before the define.
    volatile UInt8 *surfTable = (volatile UInt8 *)((char *)_gbVirt + kGbOffSurfaceTable);
    volatile UInt8 *entry = surfTable + (IOByteCount)surfId * kGbSurfaceEntryBytes;
    UInt32 fmt = kSvga3dSurfaceFormatX8R8G8B8;
    UInt64 sflags = kSvga3dSurfaceHintRenderTarget | kSvga3dSurfaceBindRenderTarget;
    UInt32 mips = 1, ms = 1, filter = 0, depth = 1;
    UInt32 pitch = dxW * 4;
    memcpy((void *)(entry + 0), &fmt, 4);
    memcpy((void *)(entry + 4), &sflags, 8);
    memcpy((void *)(entry + 12), &mips, 4);
    memcpy((void *)(entry + 16), &ms, 4);
    memcpy((void *)(entry + 20), &filter, 4);
    memcpy((void *)(entry + 24), &dxW, 4);
    memcpy((void *)(entry + 28), &dxH, 4);
    memcpy((void *)(entry + 32), &depth, 4);
    memcpy((void *)(entry + 36), &mobId, 4);
    memcpy((void *)(entry + 40), &mips, 4);
    memcpy((void *)(entry + 44), &pitch, 4);
    UInt64 zero64 = 0;
    memcpy((void *)(entry + 48), &zero64, 8);
    bzero((void *)(entry + 56), 16);
    __asm__ volatile ("" ::: "memory");

    // DEFINE_GB_SURFACE sid 3 (256x256 X8R8G8B8).
    words[0] = kSvga3dCmdDefineGbSurface;
    words[1] = 40;
    words[2] = surfId;
    memcpy(&words[3], &sflags, 8);
    words[5] = kSvga3dSurfaceFormatX8R8G8B8;
    words[6] = 1;
    words[7] = 1;
    words[8] = 0;
    words[9] = dxW;
    words[10] = dxH;
    words[11] = 1;
    if (!fifoSubmitWords(fifo, words, 12) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DEFINE_GB_SURFACE refused\n");
        return;
    }

    // DX state setup: COTABLE bind, RTV define, cotable resize, render targets,
    // viewport + scissor (the SVGA clear may clip to them).
    words[0] = kSvga3dCmdDxSetCotable;
    words[1] = 16;
    words[2] = cid;
    words[3] = cotableMobId;
    words[4] = 0;                   // SVGA_COTABLE_RTVIEW
    words[5] = 0;
    if (!fifoSubmitWords(fifo, words, 6) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_COTABLE refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxDefineRenderTargetView;
    words[1] = 28;
    words[2] = rtvId;
    words[3] = surfId;
    words[4] = kSvga3dSurfaceFormatX8R8G8B8;
    words[5] = kSvga3dResourceTexture2D;
    words[6] = 0;
    words[7] = 0;
    words[8] = 1;
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DX_DEFINE_RENDERTARGET_VIEW refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetCotable;
    words[1] = 16;
    words[2] = cid;
    words[3] = cotableMobId;
    words[4] = 0;
    words[5] = 32;                  // one RTView entry
    if (!fifoSubmitWords(fifo, words, 6) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_COTABLE resize refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetRenderTargets;
    words[1] = 8;
    words[2] = invalidId;
    words[3] = rtvId;
    if (!fifoSubmitWords(fifo, words, 4) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_RENDERTARGETS refused\n");
        return;
    }

    words[0] = kSvga3dCmdDxSetViewports;
    words[1] = 4 + 24;
    words[2] = 0;
    const float viewport[6] = { 0.0f, 0.0f, (float)dxW, (float)dxH, 0.0f, 1.0f };
    memcpy(&words[3], viewport, 24);
    if (!fifoSubmitWords(fifo, words, 9) || !fenceAck(fifo, &_fenceSeq)) {
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
    if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: DX_SET_SCISSORRECTS refused\n");
        return;
    }

    // ---- DMA round-trip experiment, independent of the DX clear ----
    // 1. fill the guest MOB with magenta
    volatile UInt32 *fill = (volatile UInt32 *)_dxVirt;
    for (UInt32 i = 0; i < dxBytes / 4; i++) {
        fill[i] = expected;
    }
    __asm__ volatile ("" ::: "memory");

    // 2. SURFACE_DMA WRITE_HOST_VRAM: mobid 4 -> host surface sid 3
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

    // 3. fill the guest MOB with a sentinel
    for (UInt32 i = 0; i < dxBytes / 4; i++) {
        fill[i] = 0x11223344;
    }
    __asm__ volatile ("" ::: "memory");

    // 4. SURFACE_DMA READ_HOST_VRAM: host surface sid 3 -> mobid 4
    dma[8] = 2;                     // SVGA3D_READ_HOST_VRAM
    if (!fifoSubmitWords(fifo, dma, 21) || !fenceAck(fifo, &_fenceSeq)) {
        IOLog(VGPU_FB_TAG ": dx: SURFACE_DMA download refused\n");
        return;
    }
    __asm__ volatile ("" ::: "memory");

    // 5. verdict
    volatile UInt32 *px = (volatile UInt32 *)_dxVirt;
    UInt32 p0 = px[0];
    UInt32 pMid = px[(dxH / 2) * dxW + (dxW / 2)];
    UInt32 pLast = px[(dxH - 1) * dxW + (dxW - 1)];
    if (p0 == expected && pMid == expected && pLast == expected) {
        IOLog(VGPU_FB_TAG ": dx: DMA ROUND TRIP VERIFIED -- magenta survived upload+download\n");
        // Now the real thing: clear the render target through the host engine and pull
        // the result back with the same, proven DMA path.
        words[0] = kSvga3dCmdDxClearRenderTargetView;
        words[1] = 20;
        words[2] = rtvId;
        const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
        memcpy(&words[3], magenta, 16);
        if (!fifoSubmitWords(fifo, words, 7) || !fenceAck(fifo, &_fenceSeq)) {
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
        _stduPtMem->release();
        _stduPtMem = nullptr;
        _stduPtVirt = nullptr;
    }
    if (_stduMem != nullptr) {
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
