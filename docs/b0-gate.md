# Gate B0 — feasibility checklist

Purpose: answer five questions that decide whether this project's architecture is H1 (fabricate
Apple's paravirtual device inside the guest, let Apple's driver attach) or falls back to Track B
(write our own macOS graphics accelerator).

Run everything through `tools/mssh.py`. Questions 1 and 2 are nearly free; **do not start backend
work until they are answered.**

---

## Q1 — What does Apple's paravirtual GPU driver match on?

```sh
# loaded kexts mentioning paravirt / gpu
python tools/mssh.py -- "kmutil showloaded | grep -iE 'paravirt|agx|gpu' "

# Bundle identifiers in the kernel collections
python tools/mssh.py -- "kmutil showloaded --list-only | grep -i paravirt"

# Look for the driver on disk (may be absent on macOS 13+; binaries moved into the KC)
python tools/mssh.py -- "ls -d /System/Library/Extensions/*Paravirt* /System/Library/DriverExtensions/*Paravirt* 2>&1"

# Strings in the kernel collections — this is where the personality ends up on modern macOS
python tools/mssh.py -- "ls -la /System/Library/KernelCollections/"
python tools/mssh.py -- "strings -a /System/Library/KernelCollections/BootKernelExtensions.kc | grep -i -C2 paravirt | head -60"
python tools/mssh.py -- "strings -a /System/Library/KernelCollections/SystemKernelExtensions.kc | grep -i -C2 paravirt | head -60"
```

**Looking for:** the `IOPCIMatch` value or `IONameMatch`, plus any extra keys the personality
requires — `IOProviderClass`, `IOMatchCategory`, `IOProbeScore`, and any `AAPL,*` or `_DSM`
properties. If `IOPCIMatch` is a concrete vendor/device pair, H1 is plausible.

**H1 fails early if:** the driver matches a platform device (`vmapple`) rather than a PCI device, or
requires properties only a hypervisor can supply.

## Q2 — Does it exist on this guest, and where does it live?

```sh
python tools/mssh.py -- "sw_vers; uname -m"
python tools/mssh.py -- "kmutil showloaded | grep -i -E 'AppleParavirt|IOGraphicsFamily'"
python tools/mssh.py -- "kmutil showloaded --list-only | wc -l"

# Is IOGraphicsFamily in the System KC? (this is the PR #600 / Force-inject question)
python tools/mssh.py -- "strings -a /System/Library/KernelCollections/SystemKernelExtensions.kc | grep -c IOGraphicsFamily"
```

**Looking for:** whether `AppleParavirtGPU` is present on x86_64 at all, and whether
`IOGraphicsFamily` resolves from the System KC (which is what makes `Kernel → Force` necessary).

**Record:** version + build, arch, and which collection each family comes from. This baseline is
needed by every later milestone.

## Q3 — Can a synthesised device win the probe race?

Deferred until Q1/Q2 pass. Requires a minimal kext. On the guest:

```sh
python tools/mssh.py -- "xcode-select -p; clang --version | head -2"
python tools/mssh.py -- "csrutil status; nvram boot-args"
```

**Looking for:** whether we can build and load an unsigned kext here at all.

## Q4 — Does the protocol assume host-mapped memory?

Reading only, against `refs/reims-vgpu/crates/reims-vgpu-wire`:

- does the device model depend on physical ranges supplied by a hypervisor
  (`PGPhysicalMemoryRange`, trace-range callbacks)?
- is the framebuffer/VRAM host-allocated, or guest-allocated and merely advertised?

**H1 weakens if:** the contract is "the host hands the guest physical pages", because inside the
guest there is no second party to hand them over.

## Q5 — How complete is the existing decoder?

```sh
ls refs/reims-vgpu/crates/reims-vgpu/src/runtime/
grep -rn "decline" refs/reims-vgpu/crates/reims-vgpu/src | wc -l
```

**Looking for:** the ratio of opcode families covered versus declined. Apple's driver requires
conformance, not "the desktop lights up", so this ratio is the real cost driver for H1.

---

## Environment baseline (run once, record in docs/)

```sh
python tools/mssh.py -- "sw_vers && uname -a && csrutil status && nvram boot-args"

# What graphics does the guest actually see?
python tools/mssh.py -- "system_profiler SPDisplaysDataType"
python tools/mssh.py -- "ioreg -l | grep -i IOAccelerator"
python tools/mssh.py -- "ioreg -rc IOPCIDevice | grep -iE 'IOName|vendor-id|device-id|model' | head -40"

# Confirm the VMware SVGA II device and VMX settings
python tools/mssh.py -- "ioreg -rc IOPCIDevice -l | grep -iE 'svga|15ad|0405' | head -20"
python tools/mssh.py -- "ls -la '/Library/Application Support/VMware Tools/' 2>&1 | head"
```

Expected: a VMware SVGA II adapter at `15ad:0405`, no `IOAccelerator` instance, and
`MTLCreateSystemDefaultDevice()` returning nil if probed. That is the "no acceleration" baseline
this project exists to change.

---

## Decision tree

```
Q1 + Q2 answered
├── driver matches a PCI vendor/device pair, and runs on x86_64
│     └── H1 alive → Q3 (can we synthesise that device?) → Q4 → Q5
│           ├── all pass  → architecture = in-guest paravirt device + pluggable backend
│           └── any fail  → fall back to Track B (own accelerator; SVGA3D backend first)
└── driver matches a platform device / requires hypervisor-only properties
      └── H1 dead → Track B, and the VMware SVGA3D backend becomes the primary target
```

Either way the first backend is the same: **SVGA3D**, because the device already exists on this
guest, needs no hypervisor changes, and its protocol is open source.
