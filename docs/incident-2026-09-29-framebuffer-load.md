# Incident 2026-09-29 — the guest froze after the first framebuffer load attempt

**Status: recovered. Cause not confirmed, and the kext is not implicated by the evidence.**

## What happened

After `-DKERNEL` fixed the link, the sequence was the ordinary one that had worked for the
probe kext:

1. install `vgpuFramebuffer.kext` into `/Library/Extensions`
2. `kmutil load -z -p /Library/Extensions/vgpuFramebuffer.kext` → `Code=28 "requires a reboot"`
3. `shutdown -r now`

The guest came back up but was unreachable: `ping` answered, **port 22 was refused** rather
than timing out, and no host anywhere on `192.168.9.0/24` was listening on 22. Two VNC captures
nine minutes apart were byte-identical, with the menu-bar clock stopped at 12:02 — the screen
was frozen on its last frame, the same presentation as the 2026-09-28 incident.

## Why the kext is not the cause

The obvious suspect was the new kext, and it is wrong:

```
$ grep -a -c 'vgpuFramebuffer' /Library/KernelCollections/AuxiliaryKernelExtensions.kc
0
$ grep -a -c 'vgpuProbe'      /Library/KernelCollections/AuxiliaryKernelExtensions.kc
12
```

`AuxiliaryKernelExtensions.kc` was **rebuilt at 12:02** — the same minute the screen froze —
and the rebuilt collection does **not contain `com.vgpu.framebuffer`**. It contains the probe,
as before. So:

- the kext never entered a kernel collection,
- therefore it was never loaded,
- therefore its `start()` never ran, and
- **it cannot have been what froze the system.**

The 12:02 collection rebuild and the freeze are simultaneous, which may mean the rebuild is
involved, or may be coincidence. This is the second freeze on this guest (see
`incident-2026-09-28-guest-hang.md`) and the first one had no third-party kext involved at all.
The working hypothesis is now that **this guest has an intermittent hang that is unrelated to
kext work**, and that both incidents are instances of it. That is not proven.

Both incidents share the same signature, which is worth remembering:

| Observation | Value |
|---|---|
| `ping` to the guest | replies |
| TCP 22 | **refused**, not timed out |
| TCP 5901 on the guest IP | refused (VMware's VNC is on the *host*, at 127.0.0.1:5901) |
| VNC capture | frozen frame; two captures minutes apart are identical |
| `vmrun` | still reports the VM as running |

"A frozen framebuffer over a live VM" is the tell. The hypervisor keeps running, so the VM looks
alive from outside while the guest's WindowServer is gone.

## Recovery

The guest could not be reached over SSH, so nothing could be undone from inside it. Recovery
used the offline-NVRAM path built on 2026-09-28 (`docs/nvram-format.md`), which until now had
only been exercised for `csr-active-config`:

```
# 1. power off hard (no SSH, so no graceful shutdown is possible)
vmrun -T ws stop E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx hard

# 2. write boot-args = "-x" into the powered-off .nvram
cp E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.nvram .nvram-backups/H-before-safe-mode.bin
python tools/nvramdump.py .nvram-backups/H-before-safe-mode.bin \
    --rebuild .nvram-backups/I-safe-mode.bin tools/nvram-edits/safe-mode.json
cp .nvram-backups/I-safe-mode.bin E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.nvram

# 3. boot; safe mode skips third-party kexts
vmrun -T ws start E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx nogui
```

The write was 51 bytes appended at offset 40101, region length 31405 → 31456, image size
unchanged — the same shape the firmware recompacts away on the next boot. On the way in:

```
$ nvram boot-args
-x
$ sysctl -n kern.safeboot
1
```

SSH was back in about two minutes. Then:

```
rm -rf /Library/Extensions/vgpuFramebuffer.kext
kmutil clear-staging
kmutil rebuild                 # -> "Checking the auxiliary kernel collection... No changes detected"

nvram -d boot-args
shutdown -r now                # normal boot: up in 1 minute, safeboot 0, en0 = 192.168.9.131
```

**Nothing was lost.** SIP is still fully off (`csr-active-config = %ff%0f%00%00`), the Command
Line Tools are still installed, `kern.safeboot` is back to 0.

## A note on writing `boot-args` offline

EFI string variables are stored **NUL-terminated** on this firmware. `boot-args = "-x"` is
therefore `2d7800`, not `2d78`. Read the encoding off an existing variable before writing a new
one — `prev-lang-diags:kbd` shows `7a682d48616e7300` = `"zh-Hans\0"`.

## Lessons

1. **A frozen framebuffer over a live VM is not a boot failure.** `vmrun` says running, `ping`
   answers, and the screen is a still image. Check whether two captures differ before concluding
   anything about what is on screen.
2. **`ping` answering while TCP is refused is a specific signature** — something is up, but no
   services are. It is the opposite of "the VM is still coming up", where the port times out.
3. **Offline NVRAM editing is now proven as a recovery mechanism, not just a research trick.**
   Safe mode through `boot-args = -x` removes third-party kexts from the equation and restores
   SSH without touching the data volume.
4. **Verify the causal story before acting on it.** The kext was the obvious suspect and the
   evidence says it was never even loaded. Grepping the rebuilt collection for the bundle ID
   took one command and prevented a rewrite of working code.
