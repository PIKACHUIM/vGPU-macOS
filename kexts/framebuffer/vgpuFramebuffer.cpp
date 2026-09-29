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
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
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
    kSvgaFifo3dHwVersion      = 7,      // dword index: host's SVGA3D protocol version
    kSvgaFifo3dCaps           = 32,     // dword index: start of the 3D capability block
    kSvgaFifo3dCapsCount      = 256,    // through SVGA_FIFO_3D_CAPS_LAST
    kSvgaFifoGuest3dHwVersion = 288,    // dword index: guest writes its version here
    kSvgaFifoBusy             = 290,

    kSvgaFifoExtendedMandatoryRegs = 288,  // registers to reserve for the extended set
};

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
    if (fifo[kSvgaFifoMin] != 0 && fifo[kSvgaFifoMin] != (IOByteCount)kSvgaFifoExtendedMandatoryRegs * 4) {
        IOLog(VGPU_FB_TAG ": FIFO configured by another driver (min %u), leaving it alone\n",
              fifo[kSvgaFifoMin]);
        map->release();
        return;
    }
    if (fifo[kSvgaFifoMin] != 0) {
        // Already ours: the SVGA device keeps its FIFO memory across a guest reboot (no
        // device reset happens unless the VM itself powers off), so a previous boot's
        // configuration is still in place. The register partition is valid; skip the
        // writes and go straight to the 3D handshake.
        IOLog(VGPU_FB_TAG ": FIFO already partitioned by us from a previous boot\n");
    }

    if (fifo[kSvgaFifoMin] == 0) {
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

    map->release();
}

// The backdoor needs the write before each read; a tiny wrapper keeps the dump loop honest.
UInt32 vgpuFramebuffer::devcapRead(UInt32 index) {
    svgaWriteRegister(_svgaPortBase, kSvgaRegDevCap, index);
    return svgaReadRegister(_svgaPortBase, kSvgaRegDevCap);
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

    if (_aperture != nullptr) {
        _aperture->release();
        _aperture = nullptr;
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
