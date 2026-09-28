//
//  vgpuFramebuffer.cpp — the smallest possible IOFramebuffer subclass.
//
//  This is stage 1 of the B0 gate (docs/plan.md). The question it answers is narrow:
//
//      Can a framebuffer we wrote ourselves be published through IOFramebuffer at all,
//      on a modern macOS, without a real GPU behind it?
//
//  It deliberately stops short of that question's follow-up. No hardware cursor, no
//  accelerator, no PCI device, no display connection -- just a wired block of physical
//  memory offered up as the system aperture and a table of modes. If IOKit's graphics
//  family accepts this, the interface is understood well enough to push further; if it
//  refuses, the logs say where.
//
//  Modelled on refs/MacHyperVSupport/MacHyperVFramebuffer, minus everything to do with
//  Hyper-V: no VMBus, no callPlatformFunction into a service, no HyperV.hpp. What is left
//  is the 8 pure virtuals IOFramebuffer demands plus the handful it calls during start.
//
//  isConsoleDevice() returns false on purpose. The VMware SVGA framebuffer owns the
//  console on this guest and taking it away is a separate, deliberate step.
//

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <mach/kmod.h>

#define VGPU_FB_TAG "vgpu-fb"

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

class vgpuFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(vgpuFramebuffer)
    typedef IOFramebuffer super;

public:
    struct Mode {
        UInt32 width;
        UInt32 height;
    };

private:
    static const Mode         kModes[];
    static const IOItemCount  kModeCount;

    void                     *_fbVirt   = nullptr;
    IOPhysicalAddress         _fbPhys   = 0;
    IOByteCount               _fbLength = 0;
    IODeviceMemory           *_aperture = nullptr;

    IODisplayModeID           _currentMode = 1;
    IOByteCount               modeBytesPerRow(UInt32 width) const { return width * (kDepth / kBitsPerByte); }

    static const UInt32 kDepth       = 32;
    static const UInt32 kBitsPerByte = 8;

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

const vgpuFramebuffer::Mode vgpuFramebuffer::kModes[] = {
    { 1024,  768 },
    { 1280, 1024 },
    { 1600, 1200 },
    { 1920, 1080 },
};
const IOItemCount vgpuFramebuffer::kModeCount =
    sizeof (vgpuFramebuffer::kModes) / sizeof (vgpuFramebuffer::kModes[0]);

//
// The aperture has to cover the largest mode we advertise, and it has to be a real
// physical range: getApertureRange() hands the address to the window server, which maps
// it directly. IOMallocContiguous gives wired, DMA-capable, physically contiguous pages,
// which is what a framebuffer needs and what a plain IOMalloc would not.
//
static void *allocFramebuffer(IOByteCount length, IOPhysicalAddress *physOut) {
    IOPhysicalAddress phys = 0;
    void *virt = IOMallocContiguous(length, PAGE_SIZE, &phys);
    if (virt == nullptr || phys == 0) {
        IOLog(VGPU_FB_TAG ": IOMallocContiguous(%llu) failed\n", (unsigned long long)length);
        return nullptr;
    }
    *physOut = phys;
    IOLog(VGPU_FB_TAG ": framebuffer %llu bytes, phys 0x%llx virt %p\n",
          (unsigned long long)length, (unsigned long long)phys, virt);
    return virt;
}

bool vgpuFramebuffer::start(IOService *provider) {
    IOLog(VGPU_FB_TAG ": start, provider=%s\n",
          provider != nullptr && provider->getMetaClass() != nullptr
              ? provider->getMetaClass()->getClassName() : "(null)");

    if (!super::start(provider)) {
        IOLog(VGPU_FB_TAG ": super::start() returned false\n");
        return false;
    }

    // Cover the biggest mode in the table.
    UInt32 maxWidth = 0, maxHeight = 0;
    for (IOItemCount i = 0; i < kModeCount; i++) {
        if (kModes[i].width > maxWidth)  maxWidth  = kModes[i].width;
        if (kModes[i].height > maxHeight) maxHeight = kModes[i].height;
    }
    _fbLength = (IOByteCount)maxWidth * maxHeight * (kDepth / kBitsPerByte);

    _fbVirt = allocFramebuffer(_fbLength, &_fbPhys);
    if (_fbVirt == nullptr) {
        return false;
    }

    _aperture = IODeviceMemory::withRange(_fbPhys, _fbLength);
    if (_aperture == nullptr) {
        IOLog(VGPU_FB_TAG ": IODeviceMemory::withRange failed\n");
        return false;
    }

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
    if (_fbVirt != nullptr) {
        IOFreeContiguous(_fbVirt, _fbLength);
        _fbVirt = nullptr;
    }
    super::stop(provider);
}

IOReturn vgpuFramebuffer::enableController() {
    IOLog(VGPU_FB_TAG ": enableController\n");

    IOReturn status = setDisplayMode(_currentMode, 0);
    if (status != kIOReturnSuccess) {
        IOLog(VGPU_FB_TAG ": enabling failed to set mode %u (0x%x)\n", _currentMode, status);
        return status;
    }
    return kIOReturnSuccess;
}

bool vgpuFramebuffer::isConsoleDevice() {
    // Deliberate: the SVGA framebuffer owns the console here.
    return false;
}

IODeviceMemory *vgpuFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture) {
        return nullptr;
    }
    IOLog(VGPU_FB_TAG ": getApertureRange -> %llu bytes @ 0x%llx\n",
          (unsigned long long)_fbLength, (unsigned long long)_fbPhys);
    return _aperture;
}

const char *vgpuFramebuffer::getPixelFormats() {
    return IO32BitDirectPixels;
}

IOItemCount vgpuFramebuffer::getDisplayModeCount() {
    return kModeCount;
}

IOReturn vgpuFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    // Mode IDs are index + 1; 0 is reserved as "invalid".
    for (IOItemCount i = 0; i < kModeCount; i++) {
        allDisplayModes[i] = (IODisplayModeID)(i + 1);
    }
    return kIOReturnSuccess;
}

IOReturn vgpuFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                       IODisplayModeInformation *info) {
    if (displayMode == 0 || displayMode > kModeCount || info == nullptr) {
        return kIOReturnBadArgument;
    }
    const Mode &mode = kModes[displayMode - 1];

    bzero(info, sizeof (*info));
    info->nominalWidth  = mode.width;
    info->nominalHeight = mode.height;
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
    if (displayMode == 0 || displayMode > kModeCount || depth != 0 || pixelInfo == nullptr) {
        return kIOReturnBadArgument;
    }
    if (aperture != kIOFBSystemAperture) {
        return kIOReturnUnsupportedMode;
    }
    const Mode &mode = kModes[displayMode - 1];

    bzero(pixelInfo, sizeof (*pixelInfo));
    pixelInfo->bytesPerRow      = modeBytesPerRow(mode.width);
    pixelInfo->bitsPerPixel     = kDepth;
    pixelInfo->pixelType        = kIORGBDirectPixels;
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->componentCount   = 3;
    pixelInfo->componentMasks[0] = 0xFF0000;   // R
    pixelInfo->componentMasks[1] = 0x00FF00;   // G
    pixelInfo->componentMasks[2] = 0x0000FF;   // B
    pixelInfo->activeWidth      = mode.width;
    pixelInfo->activeHeight     = mode.height;
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
    if (displayMode == 0 || displayMode > kModeCount || depth != 0) {
        return kIOReturnBadArgument;
    }
    const Mode &mode = kModes[displayMode - 1];
    IOLog(VGPU_FB_TAG ": setDisplayMode %u -> %ux%u@%u\n",
          displayMode, mode.width, mode.height, kDepth);
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
