//
//  vgpuProbe.cpp — minimal unsigned kernel extension used as an experiment carrier.
//
//  History: originally an authorization probe (can an unsigned kext load with SIP
//  disabled?). Answers: yes, once the bundle carries `_kmod_info`.
//
//  Now repurposed for §39: the SVGA legacy register window is only reachable from
//  kernel space (outl/inl at IOPL 0), so this kext is the cheapest possible vehicle
//  for testing whether *we* can own the mode instead of inheriting it.
//
//  It deliberately does NOT claim the framebuffer. It walks the PCI tree looking for
//  vendor 0x15ad / device 0x0405 on its own, reads BAR0 as an I/O port base, and
//  then performs whichever register experiment is selected at compile time.
//
//  Experiments (VGPU_PROBE_MODE):
//    0  read-only snapshot of the whole legacy register block    (default, safe)
//    1  snapshot, then write SVGA_REG_WIDTH/HEIGHT/BPP and re-read
//    2  snapshot, then write SVGA_REG_MAX_WIDTH/HEIGHT and re-read
//
//  Mode 0 is safe in every sense: no register is written, so the host cannot react.
//  Modes 1 and 2 are irreversible within a boot: SVGA has no "restore previous mode",
//  and reverting means writing the original values back and hoping the host re-samples.
//
//  Every snapshot is followed by a consistency check (_checkGeometry). That check exists
//  because the first version of this probe read plausible-looking values out of indices
//  that named different registers, and nothing about the numbers themselves gave it away.
//

// Include order matters. IOPCIDevice.h unconditionally pulls in
// <PCIDriverKit/IOPCIDevice.h> whenever that header exists in the SDK, and the
// DriverKit variant refers to OSClassLoadInformation and to a `New` virtual that only
// make sense once the IOKit class machinery is in scope. IORegistryEntry.h brings that
// in; without it the compile dies with "unknown type name 'OSClassLoadInformation'".
// The framebuffer kext gets the same effect for free because IOFramebuffer.h happens to
// reach IORegistryEntry.h first.
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/kmod.h>

#define VGPU_PROBE_TAG "vgpu-probe"

// ---------------------------------------------------------------------------
// Experiment selection.

#ifndef VGPU_PROBE_MODE
#define VGPU_PROBE_MODE 0
#endif

#ifndef VGPU_PROBE_NEW_WIDTH
#define VGPU_PROBE_NEW_WIDTH 0
#endif

#ifndef VGPU_PROBE_NEW_HEIGHT
#define VGPU_PROBE_NEW_HEIGHT 0
#endif

// ---------------------------------------------------------------------------
// SVGA legacy register block.
//
// These indices are NOT the sequential values in Linux's vmware/svga_reg.h. That header
// skips a block between SVGA_REG_BITS_PER_PIXEL and SVGA_REG_BYTES_PER_LINE, so its
// numbering runs 0..7 for the first eight registers and then jumps. The first revision of
// this probe copied the skipping layout incorrectly -- it assumed BYTES_PER_LINE was 8 --
// and the result was a snapshot where every index at or above 8 returned a plausible but
// wrong register. That cost a whole investigation: the values were blamed on I/O timing
// and on races with the firmware, when in fact the indices were simply off.
//
// The sequencing below is the one that produces self-consistent readings on this host:
// it is the only offset assignment under which FB_SIZE (0x300000), VRAM_SIZE (0x400000)
// and BYTES_PER_LINE (4096) all agree with the host's own log and with the pitch that
// the tearing boundary proved the host really scans at. The probe refuses to trust its
// own snapshot unless that three-way agreement holds -- see _snapshot().
//
// The framebuffer kext has carried this layout all along (vgpuFramebuffer.cpp), which is
// why its logs were right while this probe's were wrong.

static const UInt16 kSvgaIndexPort = 0;
static const UInt16 kSvgaValuePort = 1;

static const UInt32 kRegId            = 0x00;
static const UInt32 kRegEnable        = 0x01;
static const UInt32 kRegWidth         = 0x02;
static const UInt32 kRegHeight        = 0x03;
static const UInt32 kRegMaxWidth      = 0x04;
static const UInt32 kRegMaxHeight     = 0x05;
static const UInt32 kRegDepth         = 0x06;
static const UInt32 kRegBitsPerPixel  = 0x07;
static const UInt32 kRegBytesPerLine  = 0x0C;
static const UInt32 kRegFbStart       = 0x0D;
static const UInt32 kRegFbOffset      = 0x0E;
static const UInt32 kRegVramSize      = 0x0F;
static const UInt32 kRegFbSize        = 0x10;
static const UInt32 kRegCapabilities  = 0x11;
static const UInt32 kRegMemStart      = 0x12;
static const UInt32 kRegMemSize       = 0x13;
static const UInt32 kRegConfigDone    = 0x14;
static const UInt32 kRegSync          = 0x16;   // writes here block until the host drains
static const UInt32 kRegGuestId       = 0x17;
static const UInt32 kRegMemSizeActual = 0x1B;

// A small named table beats a loop with a comment: the log line then says which
// register produced which value, which is the whole point of the snapshot.
struct RegName {
    UInt32      index;
    const char *name;
};

// Sorted by index, because the snapshot prints in this order and an out-of-order table
// makes it look like indices were chosen at random.
static const RegName kRegisters[] = {
    { kRegId,            "ID"             },   // 0x00
    { kRegEnable,        "ENABLE"         },   // 0x01
    { kRegWidth,         "WIDTH"          },   // 0x02
    { kRegHeight,        "HEIGHT"         },   // 0x03
    { kRegMaxWidth,      "MAX_WIDTH"      },   // 0x04
    { kRegMaxHeight,     "MAX_HEIGHT"     },   // 0x05
    { kRegDepth,         "DEPTH"          },   // 0x06
    { kRegBitsPerPixel,  "BITS_PER_PIXEL" },   // 0x07
    { kRegBytesPerLine,  "BYTES_PER_LINE" },   // 0x0C
    { kRegFbStart,       "FB_START"       },   // 0x0D
    { kRegFbOffset,      "FB_OFFSET"      },   // 0x0E
    { kRegVramSize,      "VRAM_SIZE"      },   // 0x0F
    { kRegFbSize,        "FB_SIZE"        },   // 0x10
    { kRegCapabilities,  "CAPABILITIES"   },   // 0x11
    { kRegMemStart,      "MEM_START"      },   // 0x12
    { kRegMemSize,       "MEM_SIZE"       },   // 0x13
    { kRegConfigDone,    "CONFIG_DONE"    },   // 0x14
    { kRegSync,          "SYNC"           },   // 0x16
    { kRegGuestId,       "GUEST_ID"       },   // 0x17
    { kRegMemSizeActual, "MEM_SIZE_ACTUAL"},   // 0x1B
};

static const unsigned kRegisterCount = sizeof (kRegisters) / sizeof (kRegisters[0]);

// ---------------------------------------------------------------------------
// Register access.
//
// Port I/O.
//
// The legacy block is two 32-bit I/O ports: writing an index to +0 latches it, then
// reading or writing +1 transfers the value. There is no memory-mapped path for the low
// registers -- BAR1 is VRAM, BAR2 is the FIFO, and only the high registers
// (SVGA_REG_*_HIGH) live in MMIO.
//
// These are raw in/out instructions rather than <machine/io.h> helpers because that
// header does not exist in the Command Line Tools SDK.
//
// An `outl` to port 0x80 between the index write and the value access was tried as an
// ISA-style settling delay, on the theory that the high indices were returning the
// previously latched register. It changed nothing, byte for byte -- which, in hindsight,
// was the clue: the problem was never ordering, it was that those indices did not name
// the registers they were labelled with. The delay is kept anyway because it is free and
// it removes the ordering question from the next investigation.

static inline void vgpuProbeIoDelay() {
    __asm__ volatile ("outb %%al, $0x80" : : "a" (0));
}

static inline UInt32 vgpuProbeIn32(UInt16 port) {
    UInt32 value;
    __asm__ volatile ("inl %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline void vgpuProbeOut32(UInt16 port, UInt32 value) {
    __asm__ volatile ("outl %0, %1" : : "a" (value), "Nd" (port));
}

static inline UInt16 vgpuProbeIn16(UInt16 port) {
    UInt16 value;
    __asm__ volatile ("inw %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline UInt8 vgpuProbeIn8(UInt16 port) {
    UInt8 value;
    __asm__ volatile ("inb %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static UInt32 svgaReadRegister(UInt16 portBase, UInt32 index) {
    vgpuProbeOut32((UInt16)(portBase + kSvgaIndexPort), index);
    vgpuProbeIoDelay();
    return vgpuProbeIn32((UInt16)(portBase + kSvgaValuePort));
}

static void svgaWriteRegister(UInt16 portBase, UInt32 index, UInt32 value) {
    vgpuProbeOut32((UInt16)(portBase + kSvgaIndexPort), index);
    vgpuProbeIoDelay();
    vgpuProbeOut32((UInt16)(portBase + kSvgaValuePort), value);
    vgpuProbeIoDelay();
}

// Port-width self-test.
//
// The first run of this probe returned values that were plausible in the low indices and
// nonsense above them (BYTES_PER_LINE 0, FB_START 0xff0000, VRAM_SIZE 0) -- the signature
// of an index write that never reached the device, so every read returned whatever the
// previously latched index pointed at. The framebuffer kext reads the same block with the
// same instructions and gets sensible answers, so before trusting this probe the access
// width has to be confirmed rather than assumed.
static void probePortWidth(UInt16 portBase) {
    // Latch a known index with outl, then read it back with inl/inw/inb. A 32-bit latch
    // must survive all three reads (inw reads the low half, inb the low byte).
    const UInt32 index = 0x00001234;
    vgpuProbeOut32((UInt16)(portBase + kSvgaIndexPort), index);
    UInt32 back32 = vgpuProbeIn32((UInt16)(portBase + kSvgaIndexPort));
    UInt16 back16 = (UInt16)vgpuProbeIn16((UInt16)(portBase + kSvgaIndexPort));
    UInt8  back8  = vgpuProbeIn8((UInt16)(portBase + kSvgaIndexPort));
    IOLog(VGPU_PROBE_TAG ": port-width latch 0x%08x -> inl 0x%08x inw 0x%04x inb 0x%02x\n",
          index, back32, back16, back8);

    // And the value port, with a height of 768 latched: 0x300 reads back as 0x300 in 32
    // bits and 0x0 in 16, which distinguishes "value is there but truncated" from
    // "value is not there at all".
    vgpuProbeOut32((UInt16)(portBase + kSvgaIndexPort), kRegHeight);
    IOLog(VGPU_PROBE_TAG ": height via inl 0x%08x inw 0x%04x\n",
          vgpuProbeIn32((UInt16)(portBase + kSvgaValuePort)),
          (unsigned)vgpuProbeIn16((UInt16)(portBase + kSvgaValuePort)));

    // Restore a harmless index so the snapshot below starts from a defined latch.
    vgpuProbeOut32((UInt16)(portBase + kSvgaIndexPort), kRegId);
}

// ---------------------------------------------------------------------------

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

private:
    IOPCIDevice *_findDisplayDevice();
    void         _snapshot(UInt16 portBase, const char *when);
    void         _checkGeometry(UInt16 portBase, const char *when);
    void         _runExperiment();
};

OSDefineMetaClassAndStructors(vgpuProbe, IOService)

// The probe personality matches IOResources, so `provider` is the resources nub and
// `waitForMatchingService` below is the only way down to the PCI device. That is
// deliberate: matching display@F directly would put this class in the framebuffer
// driver's matching order, and a probe failure there changes which driver wins.
IOPCIDevice *vgpuProbe::_findDisplayDevice() {
    // waitForMatchingService would return the first IOPCIDevice, which on this guest is
    // the host bridge -- so walk every PCI device instead and pick vendor/device out of
    // configuration space. The device stays retained on the way out; the caller releases.
    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    if (matching == nullptr) {
        return nullptr;
    }

    IOPCIDevice *svga = nullptr;
    OSIterator *iter = IOService::getMatchingServices(matching);
    matching->release();

    if (iter == nullptr) {
        IOLog(VGPU_PROBE_TAG ": no IOPCIDevice matches in the registry\n");
        return nullptr;
    }

    unsigned seen = 0;
    OSObject *obj = nullptr;
    while ((obj = iter->getNextObject()) != nullptr) {
        IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, obj);
        if (dev == nullptr) {
            continue;
        }
        seen++;
        UInt16 vendor = dev->configRead16(kIOPCIConfigVendorID);
        UInt16 device = dev->configRead16(kIOPCIConfigDeviceID);
        if (vendor != 0x15ad || device != 0x0405) {
            continue;
        }
        IOLog(VGPU_PROBE_TAG ": found pci %04x:%04x (%u devices scanned)\n", vendor, device, seen);
        svga = dev;
        svga->retain();
    }
    iter->release();

    if (svga == nullptr) {
        IOLog(VGPU_PROBE_TAG ": no pci15ad,0405 among %u devices\n", seen);
    }
    return svga;
}

void vgpuProbe::_snapshot(UInt16 portBase, const char *when) {
    IOLog(VGPU_PROBE_TAG ": --- snapshot %s (ports 0x%x/0x%x) ---\n",
          when, (unsigned)(portBase + kSvgaIndexPort), (unsigned)(portBase + kSvgaValuePort));
    for (unsigned i = 0; i < kRegisterCount; i++) {
        // Five reads of the same index.
        //
        // The index/value pair is a single device-wide latch, not a per-CPU one, so any
        // concurrent reader -- the firmware, the NDRV driver, another vCPU -- can land
        // between our index write and our value read and leave us looking at a different
        // register. Sampling repeatedly and reporting the spread makes that visible:
        // a register that answers the same way five times is trustworthy, and one that
        // scatters tells us the value was never really ours to read in a single shot.
        UInt32 samples[5];
        for (unsigned s = 0; s < 5; s++) {
            samples[s] = svgaReadRegister(portBase, kRegisters[i].index);
        }
        bool stable = true;
        for (unsigned s = 1; s < 5; s++) {
            if (samples[s] != samples[0]) {
                stable = false;
            }
        }
        if (stable) {
            // The index is printed in hex as well as decimal: the table is sparse above
            // 0x07, and seeing "0x0C" next to a name is what makes an off-by-something
            // in the offsets obvious instead of something to recompute by hand.
            IOLog(VGPU_PROBE_TAG ":   %-16s [%2u/0x%02x] = 0x%08x (%u)\n",
                  kRegisters[i].name, (unsigned)kRegisters[i].index,
                  (unsigned)kRegisters[i].index, samples[0], samples[0]);
        } else {
            IOLog(VGPU_PROBE_TAG ":   %-16s [%2u/0x%02x] = 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x  <-- RACING\n",
                  kRegisters[i].name, (unsigned)kRegisters[i].index,
                  (unsigned)kRegisters[i].index,
                  samples[0], samples[1], samples[2], samples[3], samples[4]);
        }
    }
    _checkGeometry(portBase, when);
}

// Cross-check the geometry registers against each other.
//
// A snapshot full of numbers is not evidence. Three of the readings have to agree with
// each other for any of them to mean anything:
//
//   FB_SIZE == BYTES_PER_LINE * HEIGHT     (the host sizes the framebuffer from pitch)
//   FB_SIZE <= VRAM_SIZE                   (a framebuffer cannot exceed the card)
//   FB_START + FB_SIZE <= the VRAM bar     (a card whose framebuffer overruns the bar is
//                                           a card whose registers we are misreading)
//
// When the probe's index table was wrong, all three failed at once while the individual
// numbers still looked plausible -- 0x300000 sitting in a slot labelled SYNC. Printing the
// verdict next to the data means a wrong table can never again pass silently.
void vgpuProbe::_checkGeometry(UInt16 portBase, const char *when) {
    UInt32 pitch  = svgaReadRegister(portBase, kRegBytesPerLine);
    UInt32 height = svgaReadRegister(portBase, kRegHeight);
    UInt32 width  = svgaReadRegister(portBase, kRegWidth);
    UInt32 bpp    = svgaReadRegister(portBase, kRegBitsPerPixel);
    UInt32 fbSize = svgaReadRegister(portBase, kRegFbSize);
    UInt32 vram   = svgaReadRegister(portBase, kRegVramSize);
    UInt32 start  = svgaReadRegister(portBase, kRegFbStart);

    UInt32 rowBytes = (bpp / 8u) * width;
    UInt32 needed   = pitch * height;

    IOLog(VGPU_PROBE_TAG ": geometry %s: pitch %u, %u bpp x %u wide = %u bytes/row, fb size %u, vram %u\n",
          when, pitch, bpp, width, rowBytes, fbSize, vram);

    // FB_SIZE is the pitch-sized framebuffer rounded up to a 4 KB page, not pitch*height:
    // at 1024x768 both are 0x300000 because the product is already page aligned, and at
    // 1176x885 they differ by 2592 bytes (0x3F9000 vs 0x3F8FA0). Rounded-up-to-a-page of
    // pitch*height must equal FB_SIZE exactly, or one of the two registers is not what we
    // think it is.
    UInt32 expected = (needed + 4095u) & ~4095u;
    if (pitch != 0 && height != 0 && expected == fbSize) {
        IOLog(VGPU_PROBE_TAG ":   fb size == pitch*height rounded to a page: CONSISTENT\n");
    } else if (pitch != 0 && height != 0) {
        IOLog(VGPU_PROBE_TAG ":   expected fb size %u (pitch*height %u rounded up), got %u: INCONSISTENT\n",
              expected, needed, fbSize);
    } else {
        IOLog(VGPU_PROBE_TAG ":   pitch or height is zero, cannot cross-check\n");
    }

    // An exact match between pitch and bytes per row means the host is not padding
    // scanlines on this guest. Anything else is worth knowing before we program a mode,
    // because a padded pitch is a pitch the guest has to honour when it draws.
    if (pitch == rowBytes) {
        IOLog(VGPU_PROBE_TAG ":   pitch is exactly one row, no scanline padding\n");
    } else if (pitch > rowBytes) {
        IOLog(VGPU_PROBE_TAG ":   pitch exceeds %u bytes/row by %u (scanline padding)\n",
              rowBytes, pitch - rowBytes);
    } else if (pitch != 0) {
        IOLog(VGPU_PROBE_TAG ":   pitch is SHORTER than one row -- scanout would overlap, unlikely\n");
    }

    if (vram != 0 && fbSize > vram) {
        IOLog(VGPU_PROBE_TAG ":   fb size exceeds vram -- readings are not self-consistent\n");
    }
    // FB_START is a physical address the host publishes; on this guest it is the base of
    // the VRAM bar. Anything below 1 MB would be a bus address no BAR ever sits at, which
    // is the signature of reading a different register than the one we asked for.
    if (start != 0 && start < 0x100000) {
        IOLog(VGPU_PROBE_TAG ":   fb start 0x%08x is not a plausible address\n", start);
    }
}

void vgpuProbe::_runExperiment() {
    IOPCIDevice *dev = _findDisplayDevice();
    if (dev == nullptr) {
        return;
    }

    for (unsigned bar = 0; bar < 6; bar++) {
        UInt32 addr = dev->configRead32(kIOPCIConfigBaseAddress0 + bar * 4);
        IOLog(VGPU_PROBE_TAG ": BAR%u = 0x%08x%s\n", bar, addr,
              (addr & 1) ? " (I/O)" : (addr == 0 ? " (unused)" : " (memory)"));
    }

    // BAR0's low bit flags I/O address space rather than describing a bit of the base,
    // so the port base is the bar with the flag masked off. Using the raw value shifts
    // both ports by one and every register read comes back as zero, which looks exactly
    // like "the device is disabled" -- worth a comment, because that misdiagnosis costs
    // a reboot cycle to find.
    UInt16 portBase = (UInt16)(dev->configRead32(kIOPCIConfigBaseAddress0) & ~3u);
    IOLog(VGPU_PROBE_TAG ": mode %d, portBase = 0x%x\n", (int)VGPU_PROBE_MODE, portBase);

    IOLog(VGPU_PROBE_TAG ": pci command 0x%04x\n", dev->configRead16(kIOPCIConfigCommand));
    probePortWidth(portBase);

    _snapshot(portBase, "before");

#if VGPU_PROBE_MODE == 1
    IOLog(VGPU_PROBE_TAG ": writing %ux%u\n",
          (unsigned)VGPU_PROBE_NEW_WIDTH, (unsigned)VGPU_PROBE_NEW_HEIGHT);
    svgaWriteRegister(portBase, kRegWidth,  VGPU_PROBE_NEW_WIDTH);
    svgaWriteRegister(portBase, kRegHeight, VGPU_PROBE_NEW_HEIGHT);
    svgaWriteRegister(portBase, kRegBitsPerPixel, 32);
    IOSleep(200);
    _snapshot(portBase, "after width/height write");
#elif VGPU_PROBE_MODE == 2
    IOLog(VGPU_PROBE_TAG ": writing MAX_WIDTH/MAX_HEIGHT %ux%u\n",
          (unsigned)VGPU_PROBE_NEW_WIDTH, (unsigned)VGPU_PROBE_NEW_HEIGHT);
    svgaWriteRegister(portBase, kRegMaxWidth,  VGPU_PROBE_NEW_WIDTH);
    svgaWriteRegister(portBase, kRegMaxHeight, VGPU_PROBE_NEW_HEIGHT);
    IOSleep(200);
    _snapshot(portBase, "after max write");
#endif

    dev->release();
}

bool vgpuProbe::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    IOLog(VGPU_PROBE_TAG ": started, experiment mode %d\n", (int)VGPU_PROBE_MODE);
    _runExperiment();
    registerService();
    return true;
}

void vgpuProbe::stop(IOService *provider) {
    IOLog(VGPU_PROBE_TAG ": stopped\n");
    super::stop(provider);
}
