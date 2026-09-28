# Environment setup — MacOS-R15 guest

Runbook for the test guest. Everything is executed from the Windows host through
`tools/mssh.py`; nothing here requires touching the VM console **except the one step in §3**,
which macOS makes console-only by design.

| Item | Value |
|---|---|
| Guest | `MacOS-R15` — macOS 15.7.3 (24G419), x86_64, model `VMware20,1` |
| Host path | `E:\Vboxs\MacOSX\MacOS-R15\MacOS-R15.vmx` |
| SSH | `192.168.9.131`, user `pika` (admin), creds in `.env.local` |
| Host tooling | `C:\Program Files\VMware\VMware Workstation\vmrun.exe` |
| Sibling guests | `MacOS-R11` … `MacOS-R14`, `MacOS-R26` under `E:\Vboxs\MacOSX\` |

> All commands below assume `cd D:/Codes/vGPU-macOS` and
> `export PYTHONPATH="D:/Codes/vGPU-macOS/.pylibs"`.

---

## 0. Snapshot first — always

Every step that follows can leave the guest unbootable. Take a snapshot before each one and
name it after the reason, so rollback is unambiguous.

```sh
VMRUN="/c/Program Files/VMware/VMware Workstation/vmrun.exe"
VMX="E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx"      # drive-letter form; /e/... gets mangled by MSYS

"$VMRUN" -T ws listSnapshots "$VMX"
"$VMRUN" -T ws snapshot "$VMX" "<reason>-<YYYYMMDD>"
```

Rollback:

```sh
"$VMRUN" -T ws revertToSnapshot "$VMX" "<name>"
"$VMRUN" -T ws start "$VMX" nogui
```

Current snapshots: `pre-kext-dev-20260928`.

---

## 1. Command Line Tools — headless

`xcode-select --install` only opens a **GUI dialog on the console** and cannot be accepted over
SSH. Apple's "install on demand" flag makes `softwareupdate --list` advertise the package
instead, so it can be installed fully headlessly:

```sh
python tools/mssh.py --put ./tools/guest-install-clt.sh /tmp/guest-install-clt.sh
python tools/mssh.py --sudo -- "nohup sh /tmp/guest-install-clt.sh > /dev/null 2>&1 & echo started"
```

Progress: `python tools/mssh.py -- "tail -5 /var/tmp/clt-install.log"`
Success marker: the log ends with `CLT_OK`, or:

```sh
python tools/mssh.py -- "/Library/Developer/CommandLineTools/usr/bin/clang --version"
```

Offered versions on this guest: **16.2** (734 MB) and **16.4** (841 MB). The script defaults to 16.4.

---

## 2. What SIP actually protects (measured, not assumed)

Measured on this guest with SIP fully enabled:

| NVRAM variable | Writable from inside macOS? |
|---|---|
| `boot-args` | **yes** (`nvram boot-args=...` → rc=0) |
| `csr-active-config` | **no** → `(iokit/common) not permitted` |
| `recovery-boot-mode` | **no** → `(iokit/common) not permitted` |

Consequences:

- **SIP cannot be disabled from inside the running OS.** Writing `csr-active-config` is blocked,
  and so is the `recovery-boot-mode=unused` shortcut that would otherwise boot Recovery
  automatically. Full disable therefore requires booting Recovery and running `csrutil` there —
  which is console-only, because Recovery has no sshd.
- **`/Library/Extensions` is writable without touching SIP** (it is a firmlink onto the Data
  volume). So installing a kext there needs no SIP change; only *loading* it might.
- **`kmutil load` has `-z / --no-authorization`** ("Skip approval checks for this action").
  It looks like a developer escape hatch, but it is **not** one for signatures — measured, it only
  skips ownership/approval checks. See §2.1.
- `boot-args` being writable matters independently: it is the only NVRAM lever we have, and
  `amfi_get_out_of_my_way=0x1` remains available — though §2.1 explains why it is not expected to
  be enough on its own.

**Ordering constraint:** the §3 reboot would interrupt a running CLT download. Do §1 to
completion first.

### 2.1 Authorization probes — measured, all negative

Run against `kexts/probe` (a trivial unsigned `IOService` subclass that logs on start).
Build: `sh kexts/probe/build.sh` on the guest — CLT alone is sufficient, no Xcode project.

| # | Attempt | Result |
|---|---|---|
| A | `kmutil load -p vgpuProbe.kext` | `Error Domain=KMErrorDomain Code=30` … `Invalid ownership (501:0) should be (0:0)` and `Authenticating extension failed: Bad code signature` |
| A′ | after `chown -R 0:0` | ownership errors gone; **`Authenticating extension failed: Bad code signature`** |
| B | `kmutil load -z -p …` (`--no-authorization`) | **still fails**, `Code=29`, `Bad code signature`. `-z` skips the *ownership/approval* checks, **not** the signature authentication |
| C | `codesign --force --sign -` (ad-hoc), then load | **still fails**, `Code=29`, `Bad code signature` — kmutil wants an Apple-trusted kext signature, not merely *a* signature |

So on this guest, with SIP enabled, there is **no cheap way in**:

- `kmutil` performs the signature gate in **userspace, before the kernel is involved**, which is
  also why `boot-args`/`amfi_get_out_of_my_way=0x1` is not expected to help (untested — it acts on
  kernel-side AMFI, downstream of where we are being rejected).
- `-z` is worth remembering for *ownership* problems only.
- The gate is exactly `csr_check(CSR_ALLOW_UNTRUSTED_KEXTS)` — i.e. **the SIP bit**, and only the
  SIP bit.

Two ways to set that bit, and only two:

| Path | How | Trade-off |
|---|---|---|
| **Recovery** (§3) | `csrutil disable` at the console | Console-bound, two reboots. Does **not** give us a kext-injection mechanism |
| **OpenCore** | Put OpenCore on the EFI partition and set `csr-active-config` in its `NVRAM → Add` section | **Fully remote**, no console. Sets SIP at boot *and* provides the boot-time kext injection that the H1-b plan already calls for. Risk: a bad `config.plist` can stop the guest booting (snapshot first) |

---

## 3. Disabling SIP — the console step

Requires physical interaction with the VM window. Two reboots.

1. Snapshot (`pre-sip-disable-<date>`), then reboot the guest:
   `python tools/mssh.py --sudo -- "shutdown -r now"`
2. At the VMware window, hold **⌘ R** during boot to enter Recovery. (If VMware's EFI splash
   appears first, tap ⌘R repeatedly; if it fails, reboot and retry — the window is short.)
3. In Recovery: **Utilities → Terminal**, then:
   ```sh
   csrutil disable
   csrutil authenticated-root disable
   reboot
   ```
4. Back in macOS, verify from the host:
   ```sh
   python tools/mssh.py -- "csrutil status; csrutil authenticated-root status"
   ```
   Expect `System Integrity Protection status: disabled.` and `Authenticated Root status: disabled.`

Record the outcome in `docs/b0-gate-answers.md` — the SIP state is a precondition for every
later milestone.

---

## 4. Baseline facts to re-check after any environment change

```sh
python tools/mssh.py -- "sw_vers; uname -m; csrutil status; csrutil authenticated-root status"
python tools/mssh.py -- "system_profiler SPDisplaysDataType | head -20"        # expect 15ad:0405, 3 MB
python tools/mssh.py -- "ioreg -rc IOPCIDevice | grep -c ."                     # device count
python tools/mssh.py -- "ioreg -rc IOPCIDevice -l | grep -c 106b,eeee"          # expect 0 until H1-b lands
python tools/mssh.py -- "kmutil showloaded | grep -i paravirt"                  # expect empty until H1-b lands
```

---

## 5. Toolchain notes (learned the hard way)

- **`--target`/paths with a drive letter must use `D:/...`, not `/d/...`.** MSYS rewrites `/d/x`
  into `D:\d\x` when handing it to a native binary.
- **`vmrun` needs the drive-letter form too**, and its `-T ws` type is required.
- **zsh's `nomatch` aborts the whole command line** if any glob fails, e.g.
  `ls dir/*a* dir/*b*` prints nothing at all. Use `find` or `setopt nullglob`.
- **No `python3` on the guest until CLT is installed** — `/usr/bin/python3` is a CLT shim.
  That is why `tools/kcpeek.pl` is written in Perl.
