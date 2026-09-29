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
};

// FIFO registers are dwords inside the device memory region that SVGA_REG_MEM_START points
// at. Indices, not byte offsets. The 3D capability block is what a driver reads to decide
// which SVGA3D features the host will accept, so it is the definitive answer to "what can
// this virtual GPU accelerate" -- measured rather than taken from a datasheet.
enum {
    kSvgaFifoMin         = 0,
    kSvgaFifoMax         = 1,
    kSvgaFifoNextCmd     = 2,
    kSvgaFifoStop        = 3,
    kSvgaFifoBusy        = 5,
    kSvgaFifo3dHwVersion = 28,
    kSvgaFifo3dCaps      = 34,
    kSvgaFifo3dCapsCount = 64,
};

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
// Read the command FIFO's state and the host's SVGA3D capability block.
//
// The FIFO lives in the device memory region SVGA_REG_MEM_START points at. On this guest it
// is BAR2, 8 MB, and the boot-time driver has already initialised it -- the device is enabled
// and the cursor works, so the FIFO must be running. Everything here is a read: writing
// SVGA_FIFO_MIN/MAX or SVGA_REG_CONFIG_DONE would re-initialise a FIFO that is already in
// service, and that is a separate step to be taken only when a driver wants to own the
// command stream.
//
// The 3D capability block (SVGA_FIFO_3D_CAPS) is the same table Mesa's svga driver reads to
// decide which SVGA3D features exist. Dumping it here is what turns "VMware says the guest
// gets OpenGL 4.3" into a measured list of what this particular device will accept.
//
void vgpuFramebuffer::probeFifo(IOPCIDevice *pci) {
    UInt32 memStart = svgaReadRegister(_svgaPortBase, kSvgaRegMemStart);
    UInt32 memSize  = svgaReadRegister(_svgaPortBase, kSvgaRegMemSize);
    IOLog(VGPU_FB_TAG ": svga mem start 0x%x size 0x%x\n", memStart, memSize);
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

    IOLog(VGPU_FB_TAG ": fifo min %u max %u next_cmd %u stop %u busy %u\n",
          fifo[kSvgaFifoMin], fifo[kSvgaFifoMax],
          fifo[kSvgaFifoNextCmd], fifo[kSvgaFifoStop], fifo[kSvgaFifoBusy]);
    IOLog(VGPU_FB_TAG ": fifo 3d hw version 0x%x\n", fifo[kSvgaFifo3dHwVersion]);

    // Count the capabilities the host advertises, and log the whole block. A capability
    // value of zero terminates the list in some revisions; here it is a fixed-size block,
    // so print all of it and let the reader match it against the SVGA3D device capability
    // enum in Mesa's svga driver.
    UInt32 nonZero = 0;
    for (UInt32 i = 0; i < kSvgaFifo3dCapsCount; i++) {
        UInt32 value = fifo[kSvgaFifo3dCaps + i];
        if (value != 0) {
            nonZero++;
        }
        // One line per capability is 64 log lines; compress to a run of eight per line.
        if (i % 8 == 0) {
            IOLog(VGPU_FB_TAG ": 3dcaps[%2u..%2u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                  i, i + 7,
                  fifo[kSvgaFifo3dCaps + i],     fifo[kSvgaFifo3dCaps + i + 1],
                  fifo[kSvgaFifo3dCaps + i + 2], fifo[kSvgaFifo3dCaps + i + 3],
                  fifo[kSvgaFifo3dCaps + i + 4], fifo[kSvgaFifo3dCaps + i + 5],
                  fifo[kSvgaFifo3dCaps + i + 6], fifo[kSvgaFifo3dCaps + i + 7]);
        }
    }
    IOLog(VGPU_FB_TAG ": 3d capabilities advertised: %u of %u non-zero\n",
          nonZero, kSvgaFifo3dCapsCount);

    map->release();
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

    // Read the command FIFO's state and the host's SVGA3D capability block. Read-only: the
    // FIFO is already in service, and owning the command stream is the next stage, not this
    // one. The capability block is the measured answer to what this device can accelerate,
    // which is what a Metal driver on top of it has to be written against.
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
