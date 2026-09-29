# Loading an unsigned kext on this guest

> **2026-09-29 — a graphics kext now loads.** For the current, verified result and the
> recipe behind it, read *The working recipe* below first. The sections after it are the
> account of how each gate was found, including two conclusions that later turned out to
> be wrong.

Status: **verified end to end** on 2026-09-28 for a leaf kext. An unsigned third-party kext
built with Command Line Tools alone loads, runs, and survives a reboot.

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

Three independent gates had to be opened for that kext. Each one produced a *different*
error, which is why the sequence below is worth following in order. **A kext that
subclasses a family class needs a fourth thing**, and it is a different problem entirely
— see the next section.

---

## The working recipe (2026-09-29): a kext inside the auxiliary collection

Status: **verified, and it survives reboots.** `vgpuFramebuffer` now loads at every boot,
attaches to the real VMware SVGA PCI device, and builds its aperture on the adapter's own
VRAM:

```
kmutil showloaded
  134  0  0xffffff7f96a8c000  0x1ff5  0x1ff5  com.vgpu.framebuffer (1.0.0) 5CA4A9D4-...

kernel log
  (vgpuFramebuffer) vgpu-fb: start, provider=IOPCIDevice
  (vgpuFramebuffer) vgpu-fb: BAR0 phys 0x2040      length 16
  (vgpuFramebuffer) vgpu-fb: BAR1 phys 0xf0000000  length 134217728
  (vgpuFramebuffer) vgpu-fb: BAR2 phys 0xfb800000  length 8388608
  (vgpuFramebuffer) vgpu-fb: BAR3 phys 0xe8000     length 32768
  (vgpuFramebuffer) vgpu-fb: aperture is the adapter's own VRAM, 9216000 bytes @ 0xf0000000
  (vgpuFramebuffer) vgpu-fb: started
```

### Why gates 1 to 3 are not enough

`IOGraphicsFamily` lives in the **pageable** System KC. A pageable dependency cannot be
resolved for a kext that is not itself in a pageable collection, so this kext can never be
hot-loaded — `kmutil load -z` answers `Code=28 "requires a reboot"` no matter what else is
fixed. It has to be built into the **auxiliary kernel collection** by `kernelmanagerd`.

Only `kernelmanagerd` may build that collection, and building it requires approval. The
approval machinery keys on the **team identifier from the code signature** (Apple TN2459:
*"Approved KEXTs are tracked in a system-wide policy database through the team identifier
in the KEXT's code signature and the bundle identifier from the KEXT's Info.plist"*).
An unsigned kext has no team identifier, so there is nothing to record the approval
against. That is what `syspolicyd` means by:

```
Kernel Extension BLOCKED: Kext ((null), com.vgpu.framebuffer)
syspolicyd: Unsigned kext not present in the legacy kext list
```

Pressing **Allow** in System Settings does not help, and neither does hand-inserting a
`kext_policy` row with an empty `team_id`. Both were tried.

### The three things that have to be true at once

**1. The certificate must carry an OU, so the signature has a team identifier.**
`tools/guest-make-signing-identity.sh` creates a self-signed code-signing identity with
`OU=VGPU000001`, imports it, and — this part matters — marks it trusted for the `codeSign`
policy. An imported but untrusted identity is not a *valid* identity, and `codesign` then
fails with `The specified item could not be found in the keychain` even though the import
reported success. Note that `codesign -dvvv` still prints `TeamIdentifier=not set`, because
codesign only fills that field for certificates that chain to Apple. `syspolicyd` reads the
OU from the certificate anyway:

```
canLoadKernelExtension - direct, evaluate, Kext (VGPU000001, com.vgpu.framebuffer)
```

**2. The approval has to be recorded against that team identifier.**

```sh
sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy \
  "insert or replace into kext_policy (team_id,bundle_id,allowed,developer_name,flags)
   values ('VGPU000001','com.vgpu.framebuffer',1,'vgpu-macos',0);"
```

With that row in place `syspolicyd` reports `Kernel Extension ALLOWED` and `kernelmanagerd`
reports `Validate approval for /Library/Extensions/vgpuFramebuffer.kext in auxKC: approved`.
`spctl kext-consent` is irrelevant here: it is `DISABLED` on this guest and cannot be
changed outside Recovery OS.

**3. The `Info.plist` must be well-formed XML.**

This is the one that cost the most time, and it has nothing to do with signing.
`syspolicyd` logs this on *every* load attempt, including when the kext was unsigned:

```
Kext Classification: cannot create OSKext: <private>
Unsupported kext due to unsupported architectures: <private>, <private>
```

which reads like an architecture or entitlements problem and surfaces as `Code=71
"unsupported to load"`. It was a malformed plist. Two XML comments in the personality
dictionary contained `--`, which is not legal inside an XML comment:

```
... using the same category would evict it -- and a system with no console ...
... kextutil is a thin wrapper over `kmutil load --bundle-path`, so ...
```

`plutil -lint` reports `OK` because CFPropertyList is lenient, so this survived every
check made against it. Python's `plistlib` uses expat and rejects it outright:

```
$ python3 -c "import plistlib; plistlib.load(open('.../Info.plist','rb'))"
ExpatError: not well-formed (invalid token): line 31, column 49
```

With a clean plist the same kext went from `Code=71 unsupported` to `Code=27 not approved`,
and then, once the approval row was in place, to loading. **Validate every Info.plist with
a strict XML parser, not just `plutil`.** The explanations that used to live in those
comments are in `kexts/framebuffer/NOTES.md`.

### The sequence, in order

```sh
# 1. build (note -DKERNEL)
cd /var/tmp/vgpu-fb && sh build.sh

# 2. install and sign
rm -rf /Library/Extensions/vgpuFramebuffer.kext
cp -R build/vgpuFramebuffer.kext /Library/Extensions/
chown -R 0:0 /Library/Extensions/vgpuFramebuffer.kext
codesign --force --keychain /var/tmp/vgpu.keychain \
         --sign <identity-sha1> /Library/Extensions/vgpuFramebuffer.kext

# 3. record approval against the team identifier from the signature
sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy \
  "insert or replace into kext_policy (team_id,bundle_id,allowed,developer_name,flags)
   values ('VGPU000001','com.vgpu.framebuffer',1,'vgpu-macos',0);"

# 4. ask for it to be built in; then reboot when it says so
kmutil load -p /Library/Extensions/vgpuFramebuffer.kext    # Code=28 requires a reboot
shutdown -r now
```

`kernelmanagerd` builds the collection at the next boot and logs what went in:

```
CollectionBuild: trying to build:
        /Library/Extensions/vgpuFramebuffer.kext
        /Library/Extensions/vgpuProbe.kext
Already have Kext com.apple.iokit.IOGraphicsFamily v599 in system kext collection, skipping...
```

That last line is worth keeping: it shows the auxiliary collection links against the
**system** KC for `IOGraphicsFamily`, which is why the family never needed to be supplied
by hand.

### What this makes unnecessary

Everything else that was tried for this kext, all of it measured:

| Approach | Why it is not needed |
|---|---|
| Injecting `IOGraphicsFamily` (extracted from the System KC with `tools/kcextract.py`) | The auxiliary collection resolves it from the System KC. The extracted copy was removed from `/Library/Extensions` |
| OpenCore `Kernel -> Add` | The auxiliary collection route works, so the boot path was never changed |
| The KDK | Only `kmutil create -n boot` or `-n sys` need it, and neither is on this path |
| Editing `com.apple.kcgen.instructions.plist` by hand (`tools/guest-patch-kcinstructions.py`) | `kernelmanagerd` computes its own build list, and once approval is valid it includes the kext by itself. The tool is kept only as a record of the mechanism, and because it proved the file is not the trigger |
| Editing `com.apple.kcgen.uakl.plist` | Same |
| `kmutil create -z -n aux -B <boot> -S <system> -r ...` | This does produce an auxiliary collection containing the kext, but `kernelmanagerd` overwrites it on the next boot because its own build list disagrees. Useful as a diagnostic, not as a delivery mechanism |

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

### Correction (2026-09-29): `-z` does not convince kernelmanagerd

The account above holds for a kext that can be **hot-loaded**, which is what the probe is. It
does **not** hold for one that requires **rebuilding the auxiliary collection** — and any
`IOFramebuffer` subclass does, because it introduces a new class into a prelinked family's
hierarchy. Same commands, same `-z`, different outcome:

```
$ sudo kmutil load -z -p /Library/Extensions/vgpuFramebuffer.kext
Code=28 "Loading extension(s): com.vgpu.framebuffer requires a reboot"     # queued, looks fine

# at the next boot, kernelmanagerd -- a separate daemon -- decides again:
kernelmanagerd: failed to load/rebuild extensions:
    Extension with identifiers com.vgpu.framebuffer not approved to load.
    Please approve using System Settings.
kernelmanagerd: library rebuild request failed: ... not approved to load.
```

Compare the probe in the *same* boot. It is already in the collection, so it is merely
re-validated:

```
kernelmanagerd: Validate approval for /Library/Extensions/vgpuProbe.kext in auxKC: approved
kernelmanagerd: installation check: bundle for com.vgpu.probe is at /Library/Extensions/...
kernelmanagerd: Received kext load notification: com.vgpu.probe
```

So **`-z` skips the front end's check only.** kernelmanagerd re-checks for itself when it is
asked to rebuild, and it ignores `-z` entirely.

### What that rules out

| Candidate | Result |
|---|---|
| `kext_policy` row missing or malformed | Row present and identical in shape to the probe's (`allowed=1`, same empty `team_id`, same `flags`) |
| `spctl kext-consent` | `DISABLED` — not the gate |
| Link failure | Fixed by `-DKERNEL`; the same command now reaches `Code=27`, not `Code=31` |
| `kmutil create -z -n aux` | `Cannot build pageableKC/auxKC without baseKC` — it wants the boot collection as a base, and `-k` pointing at a kernel file does not satisfy that |
| `kmutil rebuild` | Takes no options at all; says `No changes detected` because a rejected request leaves no pending change behind |
| `kmutil install --update-all` / `create --update-all` | Insists on rebuilding the boot and system collections on the sealed read-only System volume |

**What remains is a logged-in GUI session**, so kernelmanagerd has somewhere to raise the
approval prompt. This guest currently has none — `stat -f %Su /dev/console` reports `root`,
i.e. nobody is logged in at the console.

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

## The real blocker was `-DKERNEL`, not a missing library

**Established 2026-09-29. This supersedes the KDK and OpenCore routes in the two sections
below** — those were chased under a wrong assumption, and the wrong assumption came from a
misleading error message. The material in them is all measured and still true; it just turns
out not to be needed.

The symptom was:

```
Code=31 ... Failed to bind '__ZN14IODeviceMemory9withRangeEjj' in 'com.vgpu.framebuffer'
          ... as could not find a kext which exports this symbol
```

That reads as "a library is missing from the link". It is not. **The symbol we were asking for
does not exist.** `IOKit/IOTypes.h` chooses the width of the IOKit address typedefs
conditionally:

    #if !defined(__arm__) && !defined(__i386__) && !(defined(__x86_64__) && !defined(KERNEL))
    typedef IOPhysicalAddress64      IOPhysicalAddress;
    #define IOPhysSize      64
    #else
    typedef IOPhysicalAddress32      IOPhysicalAddress;
    #endif

On x86_64 **without** `KERNEL` defined, `IOPhysicalAddress` and `IOByteCount` silently become
32-bit, so `IODeviceMemory::withRange` mangles as `...withRangeEjj` instead of `...withRangeEyy`.
And `clang -mkernel -fapple-kext` **does not define `KERNEL`** — a kext has to pass it.

The right symbol was available all along, from a library we were already declaring, and in the
Boot collection rather than the System one:

```
$ kmutil libraries -p /Library/Extensions/vgpuFramebuffer.kext | grep IODeviceMemory
__ZN14IODeviceMemory9withRangeEyy in BootKernelExtensions.kc: com.apple.kpi.iokit (24.6.0)
```

Note the second line of `kmutil libraries` output for the same kext:

```
__ZN13IOFramebuffer10gMetaClassE in SystemKernelExtensions.kc: com.apple.iokit.IOGraphicsFamily (599)
```

So the collection builder **can** resolve a family symbol out of the System KC. The whole
premise that `IOGraphicsFamily` had to be injected (OpenCore) or supplied as an extracted
binary was wrong.

With `-DKERNEL` added to `kexts/framebuffer/build.sh`, the very same command goes from a link
failure to the ordinary consent gate:

```
Code=27 "Extension with identifiers com.vgpu.framebuffer not approved to load.
         Please approve using System Settings."
```

### What this makes unnecessary

- No KDK. The one installed during this investigation is not on the path that matters.
- No OpenCore, no `Kernel -> Add`, no `Force`-injecting `IOGraphicsFamily`.
- No extracting `IOGraphicsFamily` from the System KC. A copy was briefly installed into
  `/Library/Extensions` during the investigation and has been removed.
- `tools/kcextract.py` is therefore not needed for this gate. It is kept because it works and
  is independently useful (it is the only way to get a kext image out of a collection), and
  because the same technique will be needed to inspect anything else that now lives only in a KC.

### Why `kmutil load -z` sent us the wrong way

Three genuinely different failures all surfaced as `Code=28 "requires a reboot"`, or as nothing:

| Real problem | `kmutil load -z -p` | `kmutil load -p` (no `-z`) | `kextutil -v` |
|---|---|---|---|
| no `_kmod_info` symbol | not useful | not useful | names the missing symbol |
| wrong `withRange` mangling | `Code=28` — **hides it** | `Code=31` + the symbol | `Code=31` + the symbol |
| genuinely needs a collection | `Code=28` (accurate) | `Code=27` / `Code=28` | `Code=28` |

**Rule: never trust `-z`'s output for diagnosis. Run `kmutil load -p` (without `-z`) to get the
link error, and `kextutil -v` to get it again in a different spelling.** Adopting that habit
earlier would have avoided this entire detour.

## (Superseded) Leaf kexts load; anything that subclasses a family class does not

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

### Route A (KDK) was executed, and it does not solve this

Measured 2026-09-29. The KDK was obtained and installed, so every "Missing Developer Kit"
error is gone — and it changed nothing, because the KDK is not on the path that matters.

**What was done.** `Kernel_Debug_Kit_15.7.3_build_24G419.dmg` (924 MB) from the
`dortania/KdkSupportPkg` GitHub releases, which mirrors KDKs without an Apple ID.
**SHA-256 verified** against the release's own `SHA256SUM` before use:

```
39653d7ea22c24a5eaeccedde1ba5bcd0599be0c415e3e0b732b228f09bf184a
```

then `hdiutil attach` + `installer -pkg` → `/Library/Developer/KDKs/KDK_15.7.3_24G419.kdk/`.

**What it did not do.** Every route to the auxiliary collection is still closed:

| Attempt | Result |
|---|---|
| `kmutil create --update-all` | `Code=30 Read-only file system` — it wants to rebuild the **boot and system** collections, which live on the sealed System volume |
| `kmutil install --update-all` | same, for each variant; debug/development/research/kasan additionally fail with `Could not find: kernel mach-o at //System/Library/Kernels/kernel.<variant>` (the KDK ships release only) |
| `kmutil create -z -n aux` | `Code=31 Cannot build pageableKC/auxKC without baseKC` |
| `kmutil create -z -n boot -n aux` | same `without baseKC` |
| `kmutil create -z -n boot aux` | `aux` is parsed as the kernel path → `Invalid argument: kernel path` |
| `kmutil configure-boot --help` | recovery-mode only, and it installs a *custom boot object* (a kernel). Not a policy bypass |
| after the KDK, `kmutil load -z` + reboot | `com.vgpu.framebuffer` **still** never reaches the auxiliary collection, and kernelmanagerd never mentions it, while `com.vgpu.probe` remains in the loaded auxiliary collection |

**The reason is in `kmutil(8)` itself:**

> **create**: This command should only be used by developers investigating custom kernels or
> replacing the contents of the boot kext collection or system kext collection. **As of
> macOS 13.0, a KDK is required to create a new boot or system kext collection.**
>
> **INSTALLING**: a kext is only loadable once it has been built into the auxiliary kext
> collection by `kernelmanagerd(8)` … If `kmutil load`, `kextload(8)`, or any invocation of a
> KextManager function attempts to load a kext that is not yet loadable, `kernelmanagerd(8)`
> will stage the kext into a protected location, validate it, and **prompt the user to
> approve a rebuild** of the auxiliary kext collection.

So the KDK unlocks only `create -n boot/sys`. Third-party kexts go through kernelmanagerd, and
the documented step is **a user approval**:

```
$ sudo kmutil rebuild
Checking the auxiliary kernel collection...
Attempting to add the following extensions (1):
[+] com.vgpu.probe	1.0.0	/Library/Extensions/vgpuProbe.kext
Attempting to remove the following extensions (1):
[-] com.vgpu.probe	1.0.0	/private/tmp/vgpu-probe/build/vgpuProbe.kext
Requesting user approval (times out in 60 seconds)...
Error Domain=com.apple.LocalAuthentication Code=-6 "Biometry is not available on this device."  (rc 250)
```

`kmutil rebuild` takes **no options at all** — not even `-z` — so there is no way to skip it.
Its plan is correct (add the `/Library/Extensions` bundle, drop the stale `/tmp` one) and it
then dies waiting for an approval a headless guest cannot give.

**Conclusion.** The only remaining blocker is a single interactive approval that needs a
logged-in GUI session. Everything else is now ruled out with evidence: not SIP, not consent
(`spctl kext-consent status` is `DISABLED`), not the consent database (rows exist for both
kexts), not `_kmod_info`, not a missing library declaration, and not the KDK.

### (Superseded) The two routes that were recorded here

Both were predicated on `IOGraphicsFamily` not being reachable from the link. It is reachable,
through the ordinary `OSBundleLibraries` declaration — see "The real blocker" above.

| Route | Status now |
|---|---|
| **A′. Console login once and approve** | Not needed for *linking*. Consent is still a separate gate for `kmutil load -p` (`Code=27`), but `-z` exists precisely to skip it, and that is how the probe got in. |
| **B. OpenCore `Kernel -> Add`** | Not needed. The tree `tools/oc-build.py` staged on the guest's ESP is harmless where it is and requires no boot entry; leave it for any future need. Its former open question — that `IOGraphicsFamily` has no on-disk binary to inject — is moot. |

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
- It compiles **and links**, including the collection builder's own link check: with `-DKERNEL`
  the 364 undefined symbols all resolve against resident libraries (see "The real blocker").
  `kmutil load -p` now gets as far as `Code=27 not approved to load`, which is the ordinary
  consent gate and not a link error.
- **It has still never executed.** No build of it has ever entered a kernel collection —
  `grep -a -c vgpuFramebuffer /Library/KernelCollections/AuxiliaryKernelExtensions.kc` is `0`
  after a boot in which kernelmanagerd rebuilt that collection — so `start()` has never run.
  What remains is one administrative step, not a code problem: `kmutil load -z -p ...` queues
  the request and returns `Code=28`, and kernelmanagerd is meant to pick it up on the next boot.
  That is exactly how the probe got in.
- Note also that its `IOResources` match is unproven and suspicious: `MacHyperVFramebuffer`
  matches a concrete PCI device (`0x53531414`), while this kext matches `IOResources`, the root
  IOKit resource object, so it probes at the earliest possible moment. Its 4 MB
  `IOMallocContiguous` in `start()` would then run during early boot. Treat the match as
  provisional and revisit it before drawing conclusions from any first `start()`.

So the binding assumption of risk R1 is "yes, the interface is understood"; what remains is an
environment problem, not a code problem.
