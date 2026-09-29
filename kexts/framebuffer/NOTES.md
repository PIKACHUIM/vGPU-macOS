# vgpuFramebuffer — design notes

The reasoning behind the settings in `Info.plist` lives here rather than in plist
comments, for a reason worth recording.

## XML comments in Info.plist must not contain `--`

The first revision of this file explained each key in XML comments. Two of those
comments contained `--`:

```
using the same category would evict it -- and a system with no console
kextutil is a thin wrapper over `kmutil load --bundle-path`, so ...
```

`--` is not legal inside an XML comment, and a strict parser rejects the whole
document. `plutil -lint` reports `OK` because CFPropertyList is lenient, so this
went unnoticed for a long time while everything downstream failed in ways that
pointed somewhere else. Python's `plistlib`, which uses expat, rejects it
outright:

```
$ plutil -lint /Library/Extensions/vgpuFramebuffer.kext/Contents/Info.plist
... OK
$ python3 -c "import plistlib; plistlib.load(open('.../Info.plist','rb'))"
ExpatError: not well-formed (invalid token): line 31, column 49
```

The way it surfaced: `syspolicyd` logged `Kext Classification: cannot create
OSKext` on every load attempt, from which it concluded `Unsupported kext due to
unsupported architectures` and refused with `Code=71`. That reads like a
signing or architecture problem and sent the investigation off in that
direction. It was a malformed plist.

Keep this file comment-free. Explanations go in this document or in the source.

## IOKitPersonalities

`IOMatchCategory` is deliberately a private string, not `IOFramebuffer`.

`IONDRVFramebuffer` already sits on `display@F` and provides the console, so
claiming the same category would evict it, and a system with no console device
is not something to try on the first run. A distinct category lets both attach:
the boot-provided framebuffer keeps the screen while this kext starts alongside
it and reports what it can see. What makes IOGraphicsFamily treat a service as a
framebuffer is the `IOFramebuffer` base class, not this string.

`IOProbeScore` is 0 on purpose, for the same reason — to lose rather than to win
while coexistence is being established.

`IOProviderClass` is `IOPCIDevice` with `IOPCIMatch = 0x040515AD`, which is
vendor `0x15AD` / device `0x0405`, VMware SVGA II. On this guest that is the
IORegistry node `display@F`, and it is the device that actually drives the
screen. Measured there: `BAR1 = 0xf0000000`, length `0x08000000` (128 MB), which
is the adapter's VRAM. Earlier revisions matched `IOResources`, which is the
root IOKit resource object: a framebuffer built on that can start but can never
reach real VRAM, because it probes at the earliest possible moment.

Note what is already on this guest: there is no graphics kext at all.
`system_profiler` reports `No Kext Loaded`, and the `IOAccelerator` count is 0.
The display runs on `IONDRVFramebuffer`, which `boot.efi` installs through the
legacy NDRV mechanism rather than through an IOKit match, so there is no
competing kext.

## OSBundleLibraries

`com.apple.iokit.IOGraphicsFamily` must be listed. `OSDefineMetaClassAndStructors`
makes our metaclass point at `IOFramebuffer`'s, and the collection builder only
searches libraries that have been named. Omitting it fails with:

```
Cannot find symbol for metaclass pointed to by
'__ZN15vgpuFramebuffer10superClassE'. Expected symbol
'__ZN13IOFramebuffer10gMetaClassE' to be defined in another kext
```

Note how unevenly that surfaces:

```
kextutil -v <kext>   -> Code=31 with the message above (useful)
kmutil load -z -p    -> Code=28 "requires a reboot" (message swallowed)
```

`kextutil` is a thin wrapper over `kmutil load --bundle-path`, so whenever a
load appears to "just need a reboot", re-run it through `kextutil` for the real
error.

That dependency is also why this kext cannot be hot-loaded. `IOGraphicsFamily`
lives in the pageable System KC, and a pageable dependency cannot be resolved
for a kext that is not itself in a pageable collection, so the kext has to be
built into the auxiliary kernel collection by `kernelmanagerd`.

## Building

`build.sh` needs `-DKERNEL`. Without it `IOKit/IOTypes.h` selects the 32 bit
width for `IOPhysicalAddress` and `IOByteCount`, because of this condition:

```c
#if !defined(__arm__) && !defined(__i386__) && !(defined(__x86_64__) && !defined(KERNEL))
typedef IOPhysicalAddress64  IOPhysicalAddress;
#else
typedef IOPhysicalAddress32  IOPhysicalAddress;
#endif
```

So `IODeviceMemory::withRange` mangles to `...Ejj` instead of `...Eyy`, and the
collection builder then reports a symbol nobody exports. `clang -mkernel
-fapple-kext` does not define `KERNEL`.

## Loading

See `docs/kext-loading.md` for the full account. In short: SIP must be off, the
kext must be staged from `/Library/Extensions` rather than `/tmp`, and because
this kext subclasses a family class it must be built into the auxiliary kernel
collection, which requires user approval recorded against a team identifier from
the code signature.
