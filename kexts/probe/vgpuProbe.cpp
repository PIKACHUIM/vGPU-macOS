//
//  vgpuProbe.cpp — minimal unsigned kernel extension used as an authorization probe.
//
//  Purpose: answer two questions.
//
//   1. With SIP fully enabled, can a third-party, unsigned kext be loaded at all — and is
//      `kmutil load -z` (skip approval checks) enough on its own?  Answer: no and no.
//   2. With SIP disabled, does an unsigned kext load?  Answer: yes, once the bundle carries
//      the `_kmod_info` symbol a kernel collection demands.
//
//  Behaviour is deliberately trivial: match the IOResources nub, log on start, log on stop.
//  It claims no hardware and changes nothing.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <mach/kmod.h>

#define VGPU_PROBE_TAG "vgpu-probe"

// A kernel collection has no separate Info.plist to read, so each kext's identity has to
// travel inside its binary as `_kmod_info`. Nothing synthesises this for a hand-built
// bundle -- ld64 embeds `__TEXT,__info_plist` but does not derive kmod_info from it -- so
// it has to be declared. KMOD_EXPLICIT_DECL stringifies the identifier and picks up the
// version, and `name` must match CFBundleIdentifier in Info.plist.
extern "C" kern_return_t vgpuProbeKmodStart(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t vgpuProbeKmodStop(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

extern "C" {
KMOD_EXPLICIT_DECL(com.vgpu.probe, "1.0.0", vgpuProbeKmodStart, vgpuProbeKmodStop)
}

class vgpuProbe : public IOService {
    OSDeclareDefaultStructors(vgpuProbe)
    typedef IOService super;

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(vgpuProbe, IOService)

bool vgpuProbe::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    IOLog(VGPU_PROBE_TAG ": started (unsigned kext loaded)\n");
    registerService();
    return true;
}

void vgpuProbe::stop(IOService *provider) {
    IOLog(VGPU_PROBE_TAG ": stopped\n");
    super::stop(provider);
}
