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
// skips a block between SVGA_REG_BITS_PER_PIXEL and SVGA_REG_BYTES_PER_LINE, so reading it
// as "0, 1, 2, ..." mislabels every register above 7. The first revision of this probe
// copied the skipping layout incorrectly -- it assumed BYTES_PER_LINE was 8 -- and every
// index at or above 8 returned a plausible but wrong register. That misdiagnosis cost
// several reboots, spent blaming I/O timing and races with the firmware.
//
// The numbering below is taken from VMware's own Windows miniport (driver/vm3dmp.sys),
// where the register accessors are plain enough to read: see docs/svga-register-map.md for
// the disassembly that establishes each entry. Two of the labels the framebuffer kext
// carries are wrong and are corrected here:
//
//   0x14 is RESET, not CONFIG_DONE -- VM3DCtrlResetDevice writes ENABLE=1 then RESET=0.
//   0x15 is SYNC (write-only) and 0x16 is its status -- one call site writes 1 and
//        returns (flush), another writes 2 and polls 0x16 back to zero (flush and wait).
//   CONFIG_DONE is 0x19.
//
// The display register block also starts at 0x20, not 0x22: the mode setter brackets a
// mode change with writes to 0x20 on both sides, which only makes sense if 0x20 is the
// enable for the block.

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
static const UInt32 kRegReset         = 0x14;   // write 0 to reset the device -- do not
static const UInt32 kRegSync          = 0x15;   // write 1 = flush, 2 = flush and wait
static const UInt32 kRegBusy          = 0x16;   // polled to zero after a flush-and-wait
static const UInt32 kRegConfigDone    = 0x19;   // set once the FIFO is configured
static const UInt32 kRegMemSizeActual = 0x1B;
static const UInt32 kRegMemRegs       = 0x1E;   // how many FIFO registers the host publishes
static const UInt32 kRegDisplayBase   = 0x20;   // display block enable / base
static const UInt32 kRegDisplayMax    = 0x21;   // block size; writing 0 masks interrupts

// 0x14 is deliberately left out of kRegisters below when it is also being written: a read
// of RESET is harmless, but a stray write of it is a device reset. See _runExperiment.

// A small named table beats a loop with a comment: the log line then says which
// register produced which value, which is the whole point of the snapshot.
struct RegName {
    UInt32      index;
    const char *name;
};

// Sorted by index, because the snapshot prints in this order and an out-of-order table
// makes it look like indices were chosen at random.
//
// 0x14 (RESET) is included in the read pass only -- reading it is harmless, and seeing it
// sit at 0 is the cheapest confirmation that the device is not being held in reset.
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
    { kRegReset,         "RESET"          },   // 0x14
    { kRegSync,          "SYNC"           },   // 0x15
    { kRegBusy,          "BUSY"           },   // 0x16
    { kRegConfigDone,    "CONFIG_DONE"    },   // 0x19
    { kRegMemSizeActual, "MEM_SIZE_ACTUAL"},   // 0x1B
    { kRegMemRegs,       "MEM_REGS"       },   // 0x1E
    { kRegDisplayBase,   "DISPLAY_BASE"   },   // 0x20
    { kRegDisplayMax,    "DISPLAY_MAX"    },   // 0x21
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
    void         _dumpFifoHeader(IOPCIDevice *dev, UInt16 portBase);
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

    _dumpFifoHeader(dev, portBase);

    dev->release();
}

// Read the FIFO register block itself, without touching it.
//
// This exists because of a contradiction that DEVCAP alone cannot settle: on a boot where
// the framebuffer driver configured the FIFO, svga_reg_devcap(0x34) reported DEVCAP_3D = 1
// while every 3D capability dword in the FIFO read zero. Either the capabilities live at a
// different index than the upstream header says (which is exactly what happened to the
// register indices in §40), or this host does not publish them and only DEVCAP is real.
//
// Printing the raw dwords around the interests lets the two hypotheses be told apart: if
// the host writes its capabilities somewhere, the non-zero runs will show up in the dump
// even at an unexpected offset.
void vgpuProbe::_dumpFifoHeader(IOPCIDevice *dev, UInt16 portBase) {
    UInt32 memStart = svgaReadRegister(portBase, kRegMemStart);
    UInt32 memSize  = svgaReadRegister(portBase, kRegMemSize);
    UInt32 memRegs  = svgaReadRegister(portBase, kRegMemRegs);
    if (memStart == 0 || memSize == 0) {
        IOLog(VGPU_PROBE_TAG ": fifo: no mem region (start 0x%x size 0x%x)\n", memStart, memSize);
        return;
    }

    IODeviceMemory *region = nullptr;
    for (UInt32 index = 0; index < 6 && region == nullptr; index++) {
        IODeviceMemory *bar = dev->getDeviceMemoryWithIndex(index);
        if (bar != nullptr && bar->getPhysicalAddress() == memStart) {
            region = bar;
        }
    }
    if (region == nullptr) {
        IOLog(VGPU_PROBE_TAG ": fifo: mem start 0x%x is not a BAR\n", memStart);
        return;
    }
    IOMemoryMap *map = region->map();
    if (map == nullptr) {
        IOLog(VGPU_PROBE_TAG ": fifo: could not map 0x%x\n", memStart);
        return;
    }

    volatile UInt32 *fifo = (volatile UInt32 *)map->getVirtualAddress();
    IOLog(VGPU_PROBE_TAG ": fifo: mem regs %u, dump of dwords 0..15 and 284..295\n", memRegs);
    for (UInt32 i = 0; i + 7 < 16; i += 8) {
        IOLog(VGPU_PROBE_TAG ": fifo[%3u..%3u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
              i, i + 7,
              fifo[i], fifo[i + 1], fifo[i + 2], fifo[i + 3],
              fifo[i + 4], fifo[i + 5], fifo[i + 6], fifo[i + 7]);
    }
    // The tail covers the guest-version / busy slots at 288..290 and one past them, so an
    // off-by-one in the extended set is visible rather than inferred.
    for (UInt32 i = 284; i + 3 < 296; i += 4) {
        IOLog(VGPU_PROBE_TAG ": fifo[%3u..%3u] %08x %08x %08x %08x\n",
              i, i + 3, fifo[i], fifo[i + 1], fifo[i + 2], fifo[i + 3]);
    }

    // Count non-zero dwords across the whole register area the host says exists, so the
    // "capabilities are zero" claim is quantified rather than sampled.
    UInt32 limit = memRegs != 0 && memRegs < 4096 ? memRegs : 291;
    UInt32 nonZero = 0, firstNonZero = limit;
    for (UInt32 i = 0; i < limit; i++) {
        if (fifo[i] != 0) {
            nonZero++;
            if (i < firstNonZero) {
                firstNonZero = i;
            }
        }
    }
    IOLog(VGPU_PROBE_TAG ": fifo: %u of %u dwords non-zero (first at %u)\n",
          nonZero, limit, firstNonZero);

    map->release();
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
