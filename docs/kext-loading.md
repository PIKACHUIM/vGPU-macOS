# Loading an unsigned kext on this guest

Status: **verified end to end** on 2026-09-28. An unsigned third-party kext built with
Command Line Tools alone now loads, runs, and survives a reboot.

```
kmutil showloaded
  157  0  0xffffff7f96a8c000  0xff9  0xff9  com.vgpu.probe (1.0.0) D850472E-...

ioreg -rc vgpuProbe
  +-o vgpuProbe  <class vgpuProbe, id 0x1000005de, registered, matched, active, busy 0 (2 ms)>

kernel log
  kernel: (vgpuProbe) vgpu-probe: started (unsigned kext loaded)     <- 23:15 hot load
  kernel: (vgpuProbe) vgpu-probe: started (unsigned kext loaded)     <- 23:17 boot

kernelmanagerd
  directLoadAuxiliaryExtensions: [com.vgpu.probe v1.0.0 (...) at
      /Library/Extensions/vgpuProbe.kext  signed @none  ...]
```

`signed @none` in kernelmanagerd's own listing, next to `com.highpoint-tech... signed
@team(DX6G69M9N2)`, is the point: unsigned extensions are accepted.

Three independent gates had to be opened. Each one produced a *different* error, which is
why the sequence below is worth following in order.

---

## Gate 1 — SIP (`csr_check(CSR_ALLOW_UNTRUSTED_KEXTS)`)

`kmutil load` rejects the kext in **user space, before the kernel ever sees it**:

```
Authenticating extension failed: Bad code signature        (Code=30 -> 29 once owned by root)
```

Neither documented escape hatch works, all three measured (see `docs/environment-setup.md` §2.1):

| Attempt | Result |
|---|---|
| `kmutil load -z` (`--no-authorization`) | still fails — it skips ownership/approval checks, **not** signature authentication |
| `codesign --force --sign -` (ad-hoc) | still fails — Apple-trusted kext signatures are required, not just "some signature" |
| `amfi_get_out_of_my_way=0x1` in `boot-args` | **cannot even be written** — `boot-args` is value-allowlisted while SIP is on |

Fixed by writing `csr-active-config` into the VM's `.nvram` while powered off; see
`docs/nvram-format.md`. Full off is `%ff%0f%00%00` (`0xfff`).

**Verify:** `csrutil status` shows Kext Signing: disabled, and `csrutil authenticated-root
status` shows disabled.

## Gate 2 — the kext must carry `_kmod_info`

Once SIP is off the error becomes a *linker* complaint:

```
Error occurred while building a collection:
  Could not use 'com.vgpu.probe' because: cannot be placed in kernel collection
  because: kexts must have a _kmod_info symbol
```

A kernel collection has no separate `Info.plist` to read, so each kext's identity has to
travel inside its binary as `_kmod_info`. Two things are needed, and the first is not
sufficient on its own:

1. **Embed the plist** — `ld64` does *not* derive `kmod_info` from it, but a collection
   needs the section:
   ```
   -Wl,-sectcreate,__TEXT,__info_plist,$BUNDLE/Contents/Info.plist
   ```
2. **Declare the symbol explicitly.** `KMOD_EXPLICIT_DECL` from `<mach/kmod.h>`
   (`KMOD_INFO_NAME` is defined as `kmod_info`, so the symbol comes out as `_kmod_info`):
   ```cpp
   #include <mach/kmod.h>

   extern "C" kern_return_t vgpuProbeKmodStart(kmod_info_t *, void *) { return KERN_SUCCESS; }
   extern "C" kern_return_t vgpuProbeKmodStop (kmod_info_t *, void *) { return KERN_SUCCESS; }

   extern "C" {
   KMOD_EXPLICIT_DECL(com.vgpu.probe, "1.0.0", vgpuProbeKmodStart, vgpuProbeKmodStop)
   }
   ```
   The identifier is stringified by the macro and **must match `CFBundleIdentifier`**.

`build.sh` now prints a self-check (`OK: _kmod_info present`) so a regression here is loud
rather than surfacing three steps later as an opaque collection error.

## Gate 3 — user consent, and a path that survives reboot

With `_kmod_info` present the error finally leaves the signature/format domain:

```
Error Domain=KMErrorDomain Code=27
  "Extension with identifiers com.vgpu.probe not approved to load.
   Please approve using System Settings."
```

That is the kernel-extension user-consent gate. `spctl kext-consent status` reports
`DISABLED`, so the gate is not in the way — the problem is that a headless VM has no GUI
session to raise the approval prompt.

`-z` / `--no-authorization` exists for exactly this. It both skips the prompt and records
the approval:

```
$ sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy 'select * from kext_policy;'
|com.vgpu.probe|1|Unidentified - vgpuProbe|0
```

Then:

```
Error Domain=KMErrorDomain Code=28 "Loading extension(s): com.vgpu.probe requires a reboot"
```

Two traps follow, both hit during the first attempt:

- **A `/tmp` bundle does not survive.** The staged entry records the path it came from;
  after a reboot `/tmp` was empty and kernelmanagerd logged
  `Installation check: missing bundle for com.vgpu.probe at /private/tmp/vgpu-probe/...`.
  Install to `/Library/Extensions` instead.
- **A stale staging entry wins over a new one.** The old `/tmp` entry had to be dropped
  with `kmutil clear-staging` before the `/Library/Extensions` path would be used.

**`kmutil install --update-all` and `kmutil rebuild` do not work here**, and this is worth
knowing before trying them:

```
Missing Developer Kit: As of macOS 13.0, you will need to install a KDK matching your
build 24G419 to rebuild kernel collections.
```
and `kmutil rebuild` additionally blocks on
`com.apple.LocalAuthentication Code=-6 "Biometry is not available on this device"`
(rc 250) — it wants Touch ID confirmation for the approval, which a headless Intel VM
cannot give.

`kmutil load -z -p /Library/Extensions/<name>.kext` needs neither.

---

## The working sequence

```sh
# on the guest, as root (tools/mssh.py --sudo)
cd /tmp/vgpu-probe
sh build.sh                    # needs Command Line Tools only
sudo sh install.sh             # /Library/Extensions + clear-staging + kmutil load -z
```

`install.sh` also hot-loads the kext, so **no reboot is required to check it worked**:

```sh
kmutil showloaded | grep -i vgpuProbe
ioreg -rc vgpuProbe
log show --last 5m --predicate 'eventMessage CONTAINS "unsigned kext loaded"'
```

## Toolchain notes

- **Comment Line Tools are sufficient.** No Xcode, no KDK.
- `clang -kext` was removed; pass `-Wl,-kext` through to ld64.
- A subclass needs `typedef IOService super;` or `super::start()` fails to compile.
- **Rebuilding needs a writable build directory.** After `chown -R 0:0` for loading, a
  non-root `sh build.sh` fails with `rm: ... Permission denied`; chown back first.
- `/Library/Extensions` is writable **without** touching SIP — it lives on the Data volume.

## Leaf kexts load; anything that subclasses a family class does not

Measured 2026-09-28 evening, after the B0 stage-1 kext (`kexts/framebuffer/`) was built.

| Kext | Class | `kmutil load -z -p` | After a reboot |
|---|---|---|---|
| `com.vgpu.probe` | `IOService`, kpi deps only | **loads immediately** — silently, no output | direct-loaded again (`directLoadAuxiliaryExtensions`) |
| `com.vgpu.framebuffer` | `IOFramebuffer` + IOGraphicsFamily | `Code=28 "requires a reboot"` | **never loads**; kernelmanagerd never even mentions it |

The probe loads and runs, repeatedly, with `signed @none`. So unsigned kexts are fine. The
difference is what the kext's class derives from.

### The mechanism

`kmutil load` will hot-load a kext whose dependencies are already resident **and** which does
not introduce a class into a prelinked family's hierarchy. The probe qualifies. A kext that
subclasses `IOFramebuffer` — a class from `IOGraphicsFamily`, which lives in the System Kernel
Collection — has to be linked **into a kernel collection**, so `kmutil load` returns
`kKernelRequiresReboot` instead.

The reboot does not deliver it. kernelmanagerd direct-loads only kexts already in the
auxiliary collection or in a pending request list, and a never-committed kext never enters
either. Committing it means rebuilding the auxiliary collection, and every route to that is
closed here:

```
kmutil create --update-all
  -> Code=71 "Missing Developer Kit: As of macOS 13.0, you will need to install a KDK
     matching your build 24G419 to rebuild kernel collections."
kmutil install --update-all
  -> same Code=71 (and it warns that --update-all is deprecated in favour of create)
kmutil rebuild
  -> takes NO options at all (not even -z), and blocks on
     com.apple.LocalAuthentication Code=-6 "Biometry is not available on this device" (rc 250)
--kdk <path>
  -> pointed at the Command Line Tools SDK, still Code=71
```

And there is no KDK to point at: `/Library/Developer/KDKs/` does not exist. `/System/Library/
Kernels/kernel` (18 MB) is present but does not satisfy the check.

Two side notes that cost time to learn:

- **`kmutil load -z` hides the real error.** After wrongly removing the IOGraphicsFamily
  declaration it reported `Code=28 "requires a reboot"` — the same message as for a kext that
  legitimately needs a collection. `kextutil -v <kext>` is a thin wrapper over
  `kmutil load --bundle-path` and prints the actual failure (there: `Cannot find symbol for
  metaclass pointed to by '__ZN15vgpuFramebuffer10superClassE'`). **When a load "just needs a
  reboot", re-run it through `kextutil`.**
- **A pending request is not durable across a rebuild.** After a boot in which
  kernelmanagerd tried and failed to install a kext (`Installation check: missing bundle ...`),
  the request disappears. So a bundle path that does not survive a reboot (`/tmp`) poisons the
  one chance the request had.

### Two unblocks

| Route | Cost | Notes |
|---|---|---|
| **A. Install the KDK for 24G419** | ~1.5 GB download from Apple's developer downloads, needs an Apple ID | then `kmutil create --update-all` can build the auxiliary collection containing our kexts; keeps the current `/Library/Extensions` flow |
| **B. OpenCore `Kernel -> Add`** | no download; change the boot path (ESP) | injects at prelink into the Boot KC — no KDK, no collection, no approval prompt. This is what `MacHyperVSupport` documents for `MacHyperVFramebuffer`, and it requires `IOGraphicsFamily` to be injected with `Force`. It also gives us `NVRAM -> Add` for `csr-active-config`, so the offline-NVRAM trick becomes unnecessary |

B is the one the evidence points at: it is the route the reference implementation actually
ships, and it removes the collection dependency entirely.

## What this unblocks

To be precise about where B0 stands. Stage 1 of the gate asked "can a framebuffer we wrote
be published through `IOFramebuffer`?" — that is now **built and validated but not yet
running**:

- `kexts/framebuffer/` compiles with Command Line Tools alone, carries `_kmod_info`, embeds
  `__TEXT,__info_plist`, declares IOGraphicsFamily, and passes the collection builder's
  link check (364 undefined symbols resolve against the resident families).
- It implements the 8 pure virtuals — `getApertureRange`, `getPixelFormats`,
  `getDisplayModeCount`, `getDisplayModes`, `getInformationForDisplayMode`,
  `getPixelFormatsForDisplayMode`, `getPixelInformation`, `getCurrentDisplayMode` — plus
  `setDisplayMode`, `enableController`, `isConsoleDevice`, `getAttribute` and the cursor trio,
  and offers a wired contiguous physical block (4 modes up to 1920x1080x32) as the system
  aperture.
- It has not executed on the machine yet: `isConsoleDevice()` returns false, and it is
  waiting on unblock A or B above before its `start()` can ever run.

So the binding assumption of risk R1 is "yes, the interface is understood"; what remains is an
environment problem, not a code problem.
