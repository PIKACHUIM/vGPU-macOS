//
//  vgpuProbe.cpp — minimal unsigned kernel extension used as an authorization probe.
//
//  Purpose: answer one question. With SIP fully enabled, can a third-party, unsigned kext
//  be loaded at all — and is `kmutil load -z` (skip approval checks) enough on its own?
//
//  Behaviour is deliberately trivial: match the IOResources nub, log on start, log on stop.
//  It claims no hardware and changes nothing.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

#define VGPU_PROBE_TAG "vgpu-probe"

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
