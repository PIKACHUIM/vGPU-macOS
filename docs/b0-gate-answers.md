# Gate B0 — answers

Evidence collected 2026-09-28 against `MacOS-R15` (`PikaDeMac.local`, macOS 15.7.3 / build 24G419,
x86_64, kernel 24.6.0) running under VMware Workstation. Host model string inside the guest:
`VMware20,1`.

**Verdict so far: Q1 and Q2 PASS — H1 is alive.** Q3 (can we fabricate the device?) is now the
gating question. Everything below is raw evidence; conclusions are marked.

---

## 0. Environment baseline

| Item | Value |
|---|---|
| OS | macOS 15.7.3, build 24G419 |
| Arch | x86_64 (so no TXM — the Apple-Silicon kext wall does not apply here) |
| Board model | `VMware20,1` |
| SIP | **enabled**; Authenticated Root: **enabled** (must be relaxed before any kext work) |
| `boot-args` | not set |
| GPU as seen by macOS | VMware SVGA II, `15ad:0405`, **VRAM 3 MB**, `Kernel Extension Info: No Kext Loaded` |
| Display | 1024×768, 24-bit — software framebuffer |
| PCI devices (real) | `8086:7190` host bridge, `8086:7110/7111/7113` PIIX4, `15ad:0740`, `15ad:0405` (SVGA II), `15ad:0774`, `15ad:1977` |
| `106b:eeee` present? | **No** — confirms the paravirtual GPU device is absent, as designed |
| Loaded graphics kexts | `IOGraphicsFamily (599)`, `AppleGraphicsDeviceControl (8.1.10)` — the framework stack is alive, just unbacked |
| Command Line Tools | **NOT installed** (`xcode-select -p` fails) → `python3`, `clang`, `make` unusable until fixed |

---

## 1. Q1 — What does Apple's paravirtual GPU driver match on? **ANSWERED: a plain PCI pair.**

`com.apple.driver.AppleParavirtGPU`'s embedded Info.plist, extracted from
`/System/Library/KernelCollections/SystemKernelExtensions.kc` (hit @ offset 1241708):

```xml
<key>AppleParavirtGPUControl</key>
<dict>
  <key>AcceleratorProperties</key>
  <dict>
    <key>IOAccelDisplayPipeCapabilities</key>
    <dict>
      <key>DisplayPipeSupported</key><true/>
      <key>TransactionsSupported</key><true/>
    </dict>
    <key>IOMatchCategory</key><string>IOAccelerator</string>
    <key>MetalPluginClassName</key><string>AppleParavirtDevice</string>
    <key>MetalPluginName</key><string>AppleParavirtGPUMetal</string>
  </dict>
  <key>CFBundleIdentifier</key><string>com.apple.driver.AppleParavirtGPU</string>
  <key>FramebufferCount</key><integer>0</integer>
  <key>IOClass</key><string>AppleParavirtGPUControl</string>
  <key>IOMatchCategory</key><string>IOFramebuffer</string>
  <key>IOPCIMatch</key><string>0xEEEE106B</string>
  <key>IOPersonalityPublisher</key><string>com.apple.driver.AppleParavirtGPU</string>
  <key>IOProbeScore</key><integer>1000000</integer>
  <key>IOProviderClass</key><string>IOPCIDevice</string>
</dict>
```

Plus:

```xml
<key>OSBundleLibraries</key>
<dict>
  <key>com.apple.AppleGraphicsDeviceControl</key><string>3.0.0</string>
  <key>com.apple.iokit.IOAcceleratorFamily2</key><string>2.0.0</string>
  <key>com.apple.iokit.IOGraphicsFamily</key><string>515.3</string>
  <key>com.apple.iokit.IOPCIFamily</key><string>1.0.0b1</string>
  <key>com.apple.iokit.IOSurface</key><string>87.0</string>
  <key>com.apple.kpi.iokit</key><string>16.7</string>
  <key>com.apple.kpi.libkern</key><string>16.7</string>
  <key>com.apple.kpi.mach</key><string>9.0.0b1</string>
  <key>com.apple.kpi.private</key><string>10.0.0d3</string>
</dict>
<key>_PrelinkBundlePath</key><string>/System/Library/Extensions/AppleParavirtGPU.kext</string>
```

**Reading (against the gate's own criteria):**

- `IOPCIMatch = 0xEEEE106B` → **vendor `0x106B` (Apple), device `0xEEEE`**. A concrete PCI pair,
  not `IONameMatch`, not a platform device. → *"H1 is plausible"* per `b0-gate.md`.
- `IOProviderClass = IOPCIDevice` → a synthesised **PCI** device is a valid provider.
- No `_DSM`, no `AAPL,*`, no ACPI hint anywhere in the personality.
- `IOProbeScore = 1000000` → if the device exists, **Apple's driver wins the probe outright**.
  We do not need to out-compete it — that is the outcome we want.
- `MetalPluginName = AppleParavirtGPUMetal`, class `AppleParavirtDevice`, matched under
  `IOAccelerator` → **the Metal driver is a separate userspace plugin, supplied by Apple.**

---

## 2. Q2 — Does it exist here, and what does the stack look like? **ANSWERED: yes, and mostly provided.**

`AppleParavirtGPU` string counts: **BootKernelExtensions.kc = 0, SystemKernelExtensions.kc = 14.**

### What is on disk

| Path | Size / nature |
|---|---|
| `/System/Library/Extensions/AppleParavirtGPU.kext` | **12 KB stub** — only `Contents/Info.plist`, `version.plist`, `_CodeSignature`. **No `Contents/MacOS/`** (binary lives in the System KC) |
| `/System/Library/Extensions/AppleParavirtGPUMetal.bundle` | real userspace bundle: `Contents/MacOS/AppleParavirtGPUMetal`; `CFBundleIdentifier=com.apple.driver.AppleParavirtGPUMetal`, `NSPrincipalClass=AppleParavirtDevice`, version 40.7.1 |
| `/System/Library/Extensions/AppleParavirtGPUMetalIOGPUFamily.bundle` | arm64 sibling of the above |
| `/System/Library/Extensions/AppleParavirtGPUIOGPUFamily.kext` | arm64 sibling of the kext |
| `/System/Library/Extensions/AppleParavirtIOSurface.kext` | IOSurface bridge |
| `/System/Library/Extensions/AppleVideoToolboxParavirtualization.kext` | the video-decode channel |
| `/System/Library/Extensions/AppleM2ScalerParavirtDriver.kext`, `AppleVirtualGraphics.kext`, `AppleVirtualPlatform.kext`, `AppleVirtIO*.kext` | rest of the virtualisation family |
| `/System/Library/Frameworks/ParavirtualizedGraphics.framework` | present in the guest too |
| └ `Versions/A/Resources/AppleParavirtEFI.rom` | **16 896 bytes — the option ROM** (`PGCopyOptionROMURL()`) |
| └ `Versions/A/Resources/default.metallib` | 24 180 bytes |
| └ `Versions/A/XPCServices/com.apple.gpusw.ParavirtualizedGraphicsGPUTask.xpc` | the framework's GPU task service |

### What the kext's symbol table reveals

The System KC carries the kext's debug strings. Source project name:
**`ParavirtualizedGraphics_IOAFKext`**. Source files:

```
AppleParavirtAccelerator.cpp      AppleParavirtChannel.cpp
AppleParavirtCommandAllocator.cpp AppleParavirtDisplayMachine.cpp
AppleParavirtDisplayPipe.cpp      AppleParavirtResource.cpp
AppleParavirtResourceHeap.cpp     AppleParavirtShared.cpp
```

Classes present (OSMetaClass / symbol names) — this is essentially the protocol surface:

| Group | Symbols |
|---|---|
| Device bring-up | `AppleParavirtAccelerator` (`setupMMIO`, `setupFIFO`, `setupRoot`), `AppleParavirtInterruptServiceEntry` |
| Channels | `AppleParavirtChannel`, `AppleParavirtRootChannel`, `AppleParavirtVirtualChannel` |
| Command path | `AppleParavirtCommandAllocator` (`getCommandBytesInt`), `AppleParavirtCommandQueue`, `AppleParavirtGPUFIFOCommandID` |
| Memory | `AppleParavirtSysMemory`, `AppleParavirtVidMemory`, `AppleParavirtMemoryMap`, `AppleParavirtResourceHeap(Handle/Namespace)`, `AppleParavirtPageTable` |
| Display | `AppleParavirtDisplayMachine` (`displayModeDidChange`), `AppleParavirtDisplayPipe` (`setupSharedState`, `framebuffer_did_power_on`), `AppleParavirtDisplayPipeFence`, `AppleParavirtFramebuffer` (`setupMMIO`, `validateDetailedTiming`) |
| Userspace bridge | `AppleParavirtShared`, **`AppleParavirtSharedUserClient`** |
| Args structs | `AppleParavirtNewResourceArgs`, `AppleParavirtAddChildResourceArgs`, `AppleParavirtRemoveChildResourceArgs`, `AppleParavirtCreateComputePipelineArgs`, `AppleParavirtCreateFunctionVariantArgs`, `AppleParavirtCreateObjectArgs`, `AppleParavirtDeleteObjectArgs`, `AppleParavirtGetDeviceInfoArgs` |
| Metal side | `AppleParavirtDevice` (plugin principal class), `AppleParavirtGPUMetal` |

---

## 3. What this means for H1

**The split of labour is even more favourable than assumed.**

| Layer | Who provides it | Status under H1 |
|---|---|---|
| PCI device identity `106b:eeee` | **us** (fabricate) | must build |
| Kernel driver (`AppleParavirtGPU.kext`) | **Apple** | already in the System KC — nothing to inject |
| Metal driver plugin (`AppleParavirtGPUMetal.bundle`) | **Apple** | already on disk, loaded on demand by the kext |
| Display plumbing / framebuffers | **Apple** | in the kext |
| Option ROM | **Apple** | `AppleParavirtEFI.rom`, present locally at a known path |
| Protocol decode → real GPU | **us** (backend) | reuse `refs/reims-vgpu` `runtime::decode` + a backend |
| Userspace↔kext plumbing | **Apple** | `AppleParavirtSharedUserClient` |

In other words: **the entire macOS-side driver stack already exists on this machine.**
The only thing missing is the *device*, and the only thing we must author is the
**device-side protocol peer plus an execution backend.**

### Decomposition (important — these are two separable risks)

- **H1-a — protocol + backend.** Make something answer the fabricated device's MMIO/FIFO and execute
  the decoded stream on a real GPU. **Can be prototyped where the device legitimately exists**
  (QEMU, where reims already attaches a device nub from outside), which isolates this risk entirely
  from the fabrication problem.
- **H1-b — in-guest device fabrication.** Make a kext present `106b:eeee` to IOKit inside the guest so
  we do not depend on the VMM at all. **This is the VMware/Hyper-V/VirtualBox enabler** and the only
  genuinely new risk, because the technique is proven (`HyperVGraphicsBridge` hand-builds a fake PCI
  config space) but its *parenting* was easy there — Hyper-V's root bridge was already synthetic.

Do not let H1-b's uncertainty block H1-a.

---

## 4. Q4 — preliminary (from symbols, to be confirmed by reading reims)

Encouraging signals:

- `setupMMIO()` + `setupFIFO()` → the device interface is **MMIO registers + a command FIFO**.
  That is a software-implementable surface; no exotic host-only contract is implied.
- `AppleParavirtSysMemory` / `AppleParavirtVidMemory` / `AppleParavirtResourceHeap` / `MemoryMap`
  → suggests **guest-allocated** system and video memory that the guest *advertises* to the host,
  rather than host-supplied physical pages. If confirmed, H1 gains: there is no second party needed
  to hand over pages.
- `AppleParavirtInterruptServiceEntry` → the device raises interrupts; a software peer must be able
  to inject one (in-guest this means an `IOPCIMessagedInterruptController`-style shim or a synthetic
  interrupt source).

Still to confirm against `refs/reims-vgpu`: whether `PGPhysicalMemoryRange` / trace-range callbacks
impose a host-mapped contract, and whether VRAM is host- or guest-allocated.

---

## 5. Q3 — the open question, and the two candidate mechanisms

Target: a `106b:eeee` device visible under a PCI bridge in the guest IORegistry, with working
config space (incl. BAR sizing) and an option-ROM BAR.

| Candidate | How | Assessment |
|---|---|---|
| **(c1) Wrap the PCI root** | An `IOPCIBridge` subclass that attaches to the ACPI `PNP0A03` node, delegates config reads/writes to the real bridge itself, and enumerates one extra software device. This is structurally what `HyperVPCIRoot` + `HyperVGraphicsBridge` do. | Leading candidate. Cost: must coexist with Apple's `AppleACPIPCI` on the same ACPI node |
| **(c2) Child bridge under the real root** | Register as a PCI-PCI bridge child of Apple's root and enumerate bus N. | Probably blocked: bridge discovery on a real root comes from real config space |
| **(c3) SSDT-declared device** | Declare the device in ACPI. | Won't work — config-space access would hit non-existent hardware |

**Recommendation:** spike (c1) on the guest, with `refs/MacHyperVSupport` as the reference
implementation. Success criterion: `ioreg -rc IOPCIDevice` shows `106b,eeee`, and
`AppleParavirtGPUControl` starts against it (check with `kmutil showloaded | grep -i paravirt`).

---

## 6. Blockers to clear before any kext work

1. **Command Line Tools not installed.** Needed for `clang`/`make`/`ctool`. Install on the guest.
2. **SIP and Authenticated Root are enabled.** A third-party kext cannot load. Decide the relaxation
   strategy — compare with `MacHyperVSupport`'s documented approach (kext signing relaxed in SIP;
   main kext injected, framebuffer kext installed to `/Library/Extensions`).
3. **Snapshot the guest first.** Every one of these steps is boot-breaking if wrong.

---

## 7. Reproduce

Every command in this document was run through `tools/mssh.py`; the KC extraction uses
`tools/kcpeek.pl` (`python3` is unavailable on the guest until CLT is installed).

```sh
cd D:/Codes/vGPU-macOS
export PYTHONPATH="D:/Codes/vGPU-macOS/.pylibs"     # paramiko lives here (see tools/README)
python tools/mssh.py --put ./tools/kcpeek.pl /tmp/kcpeek.pl
python tools/mssh.py -- "perl /tmp/kcpeek.pl AppleParavirtGPUControl \
  /System/Library/KernelCollections/SystemKernelExtensions.kc 400 1500"
```
