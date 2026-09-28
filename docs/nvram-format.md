# The VMware `.nvram` file: decoded

Status: the variable-record format is **fully decoded and verified against four known
variables**. One question stays open (see §6) and it is the only thing blocking an offline
write of `csr-active-config`.

Tooling: `tools/nvramdump.py` (host side, plain Python 3, no dependencies).

---

## 1. Why this file matters

The guest is a VMware Workstation macOS VM with no bootloader. `csr-active-config` cannot be
written from inside the running system (`not permitted`), `boot-args` refuses any value that
weakens security, and Recovery cannot be entered because VMware's PC-EFI does not interpret
⌘R and the NVRAM boot table has no Recovery entry. See `docs/nvram-and-sip.md`.

That leaves writing the variable store on the host while the VM is powered off. Hence this
reverse engineering.

## 2. File layout

`<vm>.nvram` is a flat image, 270 840 bytes on this VM. It is **not** a fixed-size flash
dump — free space is `0xFF` filled and the used length is tracked in a header.

```
0x0000  "MRVN" + u32 version=1
        CMOS checksum / index table (names CMOStimA, CMOStimB, CMOS)
        ...  ~8.5 KB of firmware state ...
0x21F8  "VMWNVRAM"                        <- EFI variable region signature
0x2200  u32 0                             <- version/flags
0x2204  u32 31337 (0x7A69)                <- REGION LENGTH, counted from 0x21F8
0x2208  first variable record
  ...
0x9C61  end of records, start of 0xFF free space (230 807 bytes)
```

The arithmetic closes exactly, which is how the length field was identified:

```
0x21F8 + 31337 = 0x9C61        (region length covers the 16-byte header + all records)
0x2208 + 31321 = 0x9C61        (record data alone)
31337 - 31321  = 16
```

Two u32s in the CMOS table (near offsets 0x14 and 0x24) also change across a boot cycle
(observed `3 -> 1`). They are inside the 0x0000–0x21F8 header and are **not** touched by a
variable write, but they should be left alone rather than reconstructed.

## 3. Variable record format

Records are packed back to back with no padding, no alignment, and **no per-record checksum**:

```
u8   guid[16]        EFI mixed-endian (first three fields little-endian)
u32  attributes      EFI_VARIABLE_* bits: NV=1, BS=2, RT=4  (7 is the common value)
u32  total_len       name_len + value_len
u32  name_len        byte count of the name, INCLUDING its 2-byte NUL terminator
u8   name[name_len]  UTF-16LE, U+0000 terminated
u8   value[total_len - name_len]
```

Record size on disk is `28 + total_len`.

### Verified samples

| Variable | GUID namespace | name_len | total_len | value | `28+total` |
|---|---|---|---|---|---|
| `Lang` | EFI global `8be4df61-93ca-11d2-aa0d-00e098032b8c` | 10 | 14 | `"eng\0"` (4 B) | 42 ✔ |
| `platform-uuid` | Apple boot `7c436110-ab2a-4bbb-a880-fe41995c9f82` | 28 | 44 | 16 B | 72 ✔ |
| `SystemAudioVolumeDB` | Apple boot | 40 | 41 | 1 B | 69 ✔ |
| `TlsConfigState` | EFI global | 30 | 31 | 1 B | 59 ✔ |

GUIDs are stored in **EFI mixed-endian**, i.e. `uuid.UUID(g).bytes_le`, not the string byte
order. This is why an earlier search for `7c436110ab2a...` found nothing while
`1061437c2aabbb4ba880fe41995c9f82` matches six times.

The whole VM has **42 records** on this guest. `tools/nvramdump.py --list` prints them all
with offsets, sizes and decoded values.

## 4. Namespaces in use

Only two GUIDs appear in the variable region on this guest:

- `8be4df61-93ca-11d2-aa0d-00e098032b8c` — EFI global (`Boot####`, `BootOrder`, `ConIn`,
  `ConOut`, `ErrOut`, `Lang`, `PlatformLang`, `KEK`, `PK`, `MemoryTypeInformation`, …)
- `7c436110-ab2a-4bbb-a880-fe41995c9f82` — Apple boot variables (`platform-uuid`,
  `previous-system-uuid`, `fmm-computer-name`, `auto-boot`, `SystemAudioVolume*`,
  `boot-args`, …)

**`csr-active-config` belongs to the Apple boot GUID**, the same one as `platform-uuid` and
`boot-args`, so the GUID bytes to write are already known from a live sample.

## 5. Observations that constrain the writer

- **Records for the same name can coexist.** `BootOrder` appears twice (58 B and 50 B, values
  `80000000010002000300` and `8000`). So the store behaves like an append log with a
  most-recent-wins lookup; a writer must not assume uniqueness.
- **The firmware rewrites the store in place, keeping the file size fixed.** A boot cycle
  changed 1012 bytes spread over the header counters and from offset 38774 to the end of the
  records, while the file stayed 270 840 bytes.
- **`csr-active-config` is absent**, so the required operation is an append, not a replace.
  That is the easy case: append the new record at `0x9C61`, then add its size to the u32 at
  `0x2204`.
- **The file is live while the VM runs.** It changed (same size, different SHA-256) with no
  writes from the host. **Any offline edit requires the VM to be fully powered off.**

## 6. Confirmed working (2026-09-28)

The open question in the first draft of this document was whether the firmware validates
anything else — a checksum, a generation counter, a copy in the CMOS area. **It does not.**
Two offline writes were made and both were accepted on the first boot:

| # | Operation | Before | After | Result |
|---|---|---|---|---|
| 1 | append `csr-active-config` (variable absent) | region len 31337, records end 0x9C61 | 31405 / 0x9CA5, record written over erased `0xFF` | accepted |
| 2 | replace it in place, same 68-byte record | value `ff000000` | value `ff0f0000` | accepted |

Evidence that the firmware took them, in increasing order of strength:

- `nvram csr-active-config` **inside the guest** returned `%ff%00%00%00`, then `%ff%0f%00%00`.
  The running system reads the value back out, so the record round-trips through firmware.
- `csrutil status` flipped from `enabled` to a full custom configuration with every
  protection off, and `csrutil authenticated-root status` flipped to `disabled` once bit 11
  was included — so the kernel read and interpreted the value.
- On the next boot the firmware **re-compacted the store**: our record moved from offset
  40033 to 39222, then to 38848, keeping the value and attributes. Offsets are therefore
  *not* stable across boots; a writer must always re-walk the store rather than remember a
  position.

The image size stayed 270840 bytes throughout, and only 68 bytes differed from the source
file in the append case.

**A caution this confirmed:** because the firmware rewrites the whole store on every boot,
`--rebuild` must always be run against a **fresh dump of a powered-off VM**, never against a
stale backup. Writing an old image back would silently revert whatever the guest had since
persisted.

## 7. Procedure for the first offline write

```sh
# 0. VM must be powered off
vmrun -T ws list                       # expect "Total running VMs: 0"

# 1. back up the live file
cp "E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.nvram" .nvram-backups/before-csr.bin

# 2. append csr-active-config (Apple boot GUID, 4-byte value 0xff000000)
python tools/nvramdump.py "E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.nvram" \
    --rebuild out.nvram edit.json

# 3. power on and check
vmrun -T ws start "E:/Vboxs/MacOSX/MacOS-R15/MacOS-R15.vmx" nogui
# ... ssh in, run: csrutil status
```

Ready-made edit scripts live in `tools/nvram-edits/`:

| File | Value | Effect |
|---|---|---|
| `csr-disable.json` | `ff000000` (0xff) | the `csrutil disable` set — bits 0–7, kext signing off, authenticated root untouched |
| `csr-disable-full.json` | `ff0f0000` (0xfff) | every `CSR_ALLOW_*` bit, including `UNAUTHENTICATED_ROOT` (0x800) and `ANY_RECOVERY_OS` (0x100) |

The byte order is the one thing to get right: the kernel reads the four bytes **little-endian**.
`%ff%0f%00%00` is 0x00000fff, i.e. bits 0 through 11. Measured breakdown of the two values:

| | 0xff | 0xfff |
|---|---|---|
| Kext Signing | disabled | disabled |
| Filesystem Protections / Debugging / DTrace / NVRAM | disabled | disabled |
| BaseSystem Verification | enabled | disabled |
| Authenticated Root | **enabled** | disabled |
| `csrutil status` headline | unknown (Custom Configuration) | unknown (Custom Configuration) |

Both values report as "unknown (Custom Configuration)" rather than plain "disabled", because
they set `CSR_ALLOW_DEVICE_CONFIGURATION` (0x80), which `csrutil disable` does not. That label
is cosmetic; the individual lines are what matter.

If a write ever appears to be ignored, try `7f000000` (exactly what `csrutil disable` writes)
before concluding the store was rejected — that distinguishes "record not honoured" from
"wrong bit set".
