# CSR / SIP on the VMware guest — where it lives and how to change it

Question this documents: *the guest has no OpenCore, so how do we enter Recovery or set CSR?*

Answer, up front: **with SIP enabled there is no way to change CSR from inside the running
system — every NVRAM lever is either blocked outright or restricted to an allowlist. Recovery (or
a bootloader) is mandatory, and this VM currently has no Recovery boot entry.** The one route that
does not need the console is an offline edit of the VM's NVRAM file; §5 covers what that requires.

All measurements below were taken on `MacOS-R15` (macOS 15.7.3 / 24G419 / x86_64, model
`VMware20,1`) on 2026-09-28.

---

## 1. Where the setting lives

`csr-active-config` — a 4-byte little-endian bitfield (`0xFF000000` on disk = `0xFF`) in the Apple
NVRAM GUID namespace. The kernel reads it at boot; nothing in a running system overrides it.

Current state: **not set at all**, i.e. SIP fully enabled.

```sh
python tools/mssh.py -- "nvram -p | grep -i csr || echo '(csr-active-config absent)'"
python tools/mssh.py -- "csrutil status; csrutil authenticated-root status"
# → System Integrity Protection status: enabled.
# → Authenticated Root status: enabled
```

## 2. Measured: what macOS lets you write to NVRAM while SIP is on

| Variable | Value tried | Result |
|---|---|---|
| `csr-active-config` | `%ff%00%00%00` | ✗ `(iokit/common) not permitted` |
| `recovery-boot-mode` | `unused` | ✗ `(iokit/common) not permitted` |
| `boot-args` | `-v` | **✓ rc=0** |
| `boot-args` | `amfi_get_out_of_my_way=0x1` | ✗ rc=1 |
| `boot-args` | `vgpu` | ✗ rc=1 |

The `boot-args` results are the interesting ones: the refusal is **value-dependent, not
variable-dependent**. `-v` is accepted; an arbitrary token is not. So SIP enforces an
**allowlist of boot-args values**, and Apple's own diagnostic flags are on it while anything that
would relax security is not.

**Consequence: `amfi_get_out_of_my_way=0x1` is not reachable while SIP is on.** That was the last
remote lever, and it is closed.

## 3. And the kext-signature checks are not bypassable either

See `docs/environment-setup.md` §2.1 for the full probe. Summary: `kmutil load` of an unsigned
kext fails with `Bad code signature`; `kmutil load -z` (`--no-authorization`) **still fails** (it
only skips ownership/approval checks); ad-hoc `codesign` **still fails**. The gate is literally
`csr_check(CSR_ALLOW_UNTRUSTED_KEXTS)`, and it is evaluated in kmutil's userspace stage — before
the kernel-side AMFI is involved, which is also why the `boot-args` route above would not have
helped even if it had been writable.

## 4. Why there is no Recovery entry on this VM

Recovery **does** exist on disk:

```
/dev/disk1s3  APFS Volume  Recovery   1.3 GB   (Role: Recovery)
/Volumes/Recovery/7C8BF470-57B0-45CE-B282-D92ED71C58FF/
```

…and `7C8BF470-…` is the **Data volume** UUID — the usual APFS volume-group naming, so the
Recovery payload is present and matches this installation.

But the firmware boot entries do not offer it. Decoded from the NVRAM store:

| Entry | Name | Device path |
|---|---|---|
| `Boot0000` | EFI VMware Virtual SATA Hard Drive (0.0) | — |
| `Boot0001` | EFI VMware Virtual SATA CDROM Drive (1.0) | — |
| `Boot0002` | EFI Network | — |
| `Boot0003` | EFI Internal Shell | — |
| `Boot0080` | Mac OS X | `\7C8BF470-…\System\Library\CoreServices\boot.efi` (= normal boot, the Data volume group) |
| `Boot0081` | Mac OS X | `\E4864CFE-9FEE-40AA-B309-B5E90E8554D1\…` ← **UUID matches no volume on this machine** |
| `BootOrder` | `Boot0080` | — |

Volume UUIDs for comparison: Data `7C8BF470-…`, Preboot `83491D0F-6A7F-426C-B505-494EEC2B6E6E`,
System snapshot `2FAE8111-…`. So `Boot0081` is **stale** — almost certainly left over from the
sibling clone `MacOS-R15 - 副本.vmdk`. It is not Recovery.

Two further consequences:

- **⌘R does not apply.** Apple's firmware is what interprets ⌘R and sets `recovery-boot-mode`; a
  VMware guest runs VMware's PC-EFI implementation, not Apple's. (Same reason QEMU/OSX-KVM users
  cannot use ⌘R.)
- **`bless` cannot fix it.** `bless --info` fails outright:
  `Volume for path /dev/disk1s4 is not available` — the System volume is a **sealed APFS
  snapshot**, which `bless` will not touch.

## 5. The three routes that remain

| Route | How | Console needed | Status |
|---|---|---|---|
| **A. Boot a Recovery image** | Attach a macOS recovery/installer image as a SATA disk or CD and boot it — this is how the VM was built in the first place (`Boot0001` is a SATA CDROM entry) | **yes** (type `csrutil disable`) | viable, needs an image |
| **B. Offline NVRAM edit** | Power the VM off, write `csr-active-config` into `MacOS-R15.nvram` directly, power on | **no** | format largely decoded, see below |
| **C. Install a bootloader** | OpenCore/Clover on the EFI partition, set `csr-active-config` in `NVRAM → Add` | no | ruled out by the brief (the VM has no bootloader and we are not adding one) |

### Route B — what is known about the format

`MacOS-R15.nvram` (270 840 bytes) is a parseable EFI variable store:

- Header: magic `MRVN`, `uint32` version = 1.
- The first ~33 KB is a non-variable region; records begin around offset 33 779.
- Variable names are **UTF-16LE**, NUL-terminated, and the `uint32` immediately before each name
  is its length in bytes (`2 × chars + 2`). Verified against `SbConfigState` (28), `TlsConfigState`
  (30), `BootOrder` (20), `Boot0080` (18).
- Immediately before that length there are two more `uint32` fields; the outer one reads as EFI
  **attributes** (`7` = NV|BS|RT dominates; a few secure-boot variables use `3`). Field order and
  the meaning of the remaining `uint32` are **not yet confirmed**.
- GUIDs are **not** stored inline — the Apple GUID `7C436110-…` does not appear as raw bytes
  anywhere, so there is very likely a GUID table in the leading region referenced by index.

Two cautions before anyone writes to this file:

1. **The file is live.** It changed (same size, different SHA-256) while the guest was running,
   without us writing anything. Any offline edit must be done with the **VM powered off**.
2. **Ground truth is available and cheap.** Because `nvram boot-args=-v` *is* accepted, we can
   capture exactly one record being inserted:
   power off → back up `nvram` → power on → `nvram boot-args=-v` → power off → diff the two files.
   That yields a real, known-value record to validate the layout against, instead of guessing from
   bytes. This is the experiment to run next.

`tools/nvram-scope.py` (to be written) should aim to: parse the store, round-trip it byte-for-byte,
and only then offer an insert. **No in-place write until a round-trip is byte-identical.**

---

## 6. Practical recommendation

If the goal is simply to get SIP off once, **Route A is the shortest path**: attach a recovery
image, boot it, run

```sh
csrutil disable
csrutil authenticated-root disable
reboot
```

If the goal is a workflow where kext loading is repeatable and scriptable — which is what this
project actually needs, since the H1-b kext will be rebuilt constantly — then **Route B is worth
the investment**, because it also lets us set `recovery-boot-mode`, re-assert `csr-active-config`
after OS updates, and generally stop treating guest boot state as a manual step.

Route A does not give us that; it only unblocks the first load.
