# Incident: guest hang, 2026-09-28 22:35–22:48

**Symptom (as reported):** the VM stopped booting — the window was stuck on the boot screen.

**Outcome:** recovered. Hard power cycle; the guest booted in **56 seconds** and came up clean.
Nothing was lost. Root cause **not** confirmed — this document separates what was measured
from what is inferred.

---

## 1. Timeline (local time, GMT+8)

| Time | Event |
|---|---|
| 21:35 | Snapshot `pre-kext-dev-20260928` taken |
| 22:15:54 | Guest logs a `shutdown_stall` (the shutdown *before* this session) |
| 22:18:09 | VM powered on (`vmrun -T ws start … nogui`) |
| 22:26:19 | MKS window created — someone opened the VM window |
| 22:28–22:31 | `AHCI-VMM: sata0:1 Port COMRESET requested` starts, with `check condition 02 3a 00` |
| 22:33 | SSH works. `macOS 15.7.3 (24G419)`, `uptime 15 mins`, `csrutil status` = enabled |
| ~22:35 | First SSH command read timeout (`nvram boot-args` under sudo) |
| 22:38 | The guest writes the `.nvram` file |
| 22:43 | User reports the VM is not booting |
| 22:44:46 | **WindowServer is still processing HID events** — mouse-scroll telemetry, USB re-enumeration |
| 22:45:00 | Delta disk still being written |
| ~22:48 | Hard power off, `.nvram` restored byte-exact from backup |
| 22:49 | Power on |
| 22:50 | SSH back; `up 56 secs`; `WindowServer` (159) and `loginwindow` (170) both running |

## 2. What was measured

**The VM never crashed.** `vmware.log` for the whole window contains:

- no `-ERROR`, no kernel panic, no VM reset, no suspend/sleep record
- `AHCI-USER: Already in check condition 02 3a 00` repeating — sense key `02` (NOT READY),
  ASC `3a` (MEDIUM NOT PRESENT). The guest's `IOATAPI` retrying an empty optical drive.
- `AHCI-VMM: sata0:1 Port COMRESET requested` roughly every 1–4 minutes
- `SVGA3dCaps: host, at power on (3d disabled)`
- `VIX_E_TOOLS_NOT_RUNNING` — no VMware Tools, so no guest operations (no screenshot, no
  in-guest commands) are available at all

**The guest was half-alive.** From inside, after recovery:

- `corebrightnessd` repeatedly logged `Unable to create and lookup port
  "com.apple.windowserver.active" => 1102`, blocked in `WaitForWindowServerPort()` inside
  `dispatch_sync`
- `airportd`: `Did NOT find matching WiFi interface`
- `WindowServer[159]` was **still handling HID events at 22:44:46**, i.e. during the window
  when the user considered the machine dead

**The host NVRAM file was never written by this agent.** `git`-tracked backups:

- `D:\Codes\vGPU-macOS\.nvram-backups\A-powered-off.bin` — byte-exact copy of the powered-off
  file, taken before power-on
- `D:\Codes\vGPU-macOS\.nvram-backups\B-live-during-boot.bin` — the same file while running

Diff of the two: 1012 bytes, spread over the CMOS counters and offsets 38774→40032. Parsed at
the record level, the differences are **entirely routine firmware bookkeeping**:

| Variable | A (powered off) | B (during boot) |
|---|---|---|
| `MTC` | `07000000` | `08000000` |
| `BootOrder` (last record) | `8000` | `80000000010002000300` |
| `MemoryTypeInformation` | `…b31b0000` | `…cd1b0000` |
| CMOS counters @0x14, @0x24 | `03` | `01` |

**No `boot-args` was ever written.** `tools/nvramdump.py --find boot-args` reports it absent in
both files. The `nvram boot-args=…` command that was issued **never completed** — its SSH
channel timed out, and the record is not in the store. So the `nvram` hang was a **symptom of
the wedge, not its cause**.

After recovery, `nvram` reads return in **0.015 s**, confirming there is nothing wrong with the
NVRAM path itself.

## 3. What was ruled out

- **Host-side `.nvram` corruption** — never written by the agent; restored byte-exact anyway.
- **`boot-args`** — never written (verified by parsing the store).
- **Kernel panic** — no panic file, no `-ERROR` in the VM log, no reset.
- **Guest sleep** — no sleep/suspend entries in the VM log; the guest kernel stayed active.
- **Disk space / jetsam** — the only jetsam report is from 00:11 the previous night.

## 4. Suspected contributing factors (not confirmed)

1. **`sata0:1` is a raw host optical-drive passthrough with autodetect.**
   ```
   sata0:1.deviceType   = "cdrom-raw"
   sata0:1.fileName     = "auto detect"
   sata0:1.autodetect   = "TRUE"
   sata0:1.startConnected = "FALSE"
   ```
   That produces the COMRESET/check-condition storm. `IOATAPI` and the boot disk's
   `IOAHCIBlockStorage` **share one AHCI controller instance and its workloop**, so a driver
   spinning on one port is a plausible way to stall I/O on the other. The storm was present
   during the successful part of the boot too, so this is suspicion, not proof.
2. **The display stack is unaccelerated.** 3 MB of reported VRAM, `No Kext Loaded`, 3D disabled
   at the hypervisor, software framebuffer at 1024×768, and no VMware Tools. The screen shows
   the **last frame the guest rendered** — so any WindowServer stall appears to the user as a
   frozen boot screen, regardless of what the machine is actually doing underneath.
3. **No VMware Tools** means no `captureScreen`, no `runProgramInGuest`, no VIX at all — which
   is why this incident had to be diagnosed entirely from the host-side log and the NVRAM image.

## 5. Recovery runbook

**Do this the moment the guest stops responding.**

```sh
VMRUN="/c/Program Files/VMware/VMware Workstation/vmrun.exe"
VMX="E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx"       # NB: drive-letter form, see §6

# 1. confirm it is really stuck, not just slow
"$VMRUN" -T ws list
ping -n 2 192.168.9.131

# 2. hard power off
"$VMRUN" -T ws stop "$VMX" hard

# 3. restore the NVRAM image if any NVRAM work was in flight
cp "D:/Codes/vGPU-macOS/.nvram-backups/A-powered-off.bin" "E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.nvram"

# 4. power on and poll
"$VMRUN" -T ws start "$VMX" nogui
python tools/mssh.py -c 45 -r 6 -- "uname -n; uptime"
```

**Last resort:** revert to the snapshot.

```sh
"$VMRUN" -T ws revertToSnapshot "$VMX" pre-kext-dev-20260928
"$VMRUN" -T ws start "$VMX"
```

The snapshot predates the Command Line Tools install, so reverting means reinstalling CLT
(`tools/guest-install-clt.sh`, fully headless, ~10 min).

## 6. Tooling notes worth keeping

- **`vmrun` and Git Bash paths.** `vmrun` is a Windows binary; MSYS rewrites `/e/Vboxs/…` into
  `c:\e\Vboxs\…` and reports "VM not found". Always pass **drive-letter form**:
  `E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx`.
- **A wedged guest wedges paramiko too.** `Error reading SSH protocol banner` and channel
  `PipeTimeout` are ambiguous — they mean "unreachable" as often as "wrong credentials".
  `tools/mssh.py` now takes `-c/--connect-timeout` and `-r/--retries` so a booting guest can be
  polled instead of producing a traceback.
- **A running VM rewrites its `.nvram`.** Same size, different hash, no host-side writes. Any
  offline edit needs the VM powered off; always `cp` a backup first.
- **`vmware.log` rotates.** `vmware.log` is the current session, `vmware-0.log` the previous
  one. Diagnosing a past incident means reading the rotated file, not the current one.

## 7. Recommended hardening (each needs one power cycle)

| # | Change | Rationale |
|---|---|---|
| 1 | Set `sata0:1.present = "FALSE"` (or at least `autodetect = "FALSE"`) | removes the optical retry storm and its shared-workloop risk |
| 2 | Enable VMware's built-in VNC (`RemoteDisplay.vnc.enabled = "TRUE"`, port 5901) | gives host-side screenshots of the guest framebuffer **without VMware Tools** — this incident could have been resolved in one look |
| 3 | Snapshot before every kext experiment | already the practice; keeps the blast radius at one reboot |
