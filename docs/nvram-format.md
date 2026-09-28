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

## 6. What is still unverified

**No checksum over the variable region was found** — the header carries none that is obvious,
and the 16 bytes right after the region signature are `"VMWNVRAM"` + version + length. But
this was **not confirmed by experiment**: the plan was to write `boot-args` from inside the
guest, power off, and diff the file to see whether any header field moved as well.

That experiment failed to run — the guest wedged before the write landed (see
`docs/incident-2026-09-28-guest-hang.md`). The current file was restored byte-exact from
`.nvram-backups/A-powered-off.bin`, and no `boot-args` was ever written.

So the first offline write is still a **two-outcome experiment**: either the firmware honours
the appended record, or it rejects/ignores the store. Mitigations already in place:

- byte-exact backups of two states in `D:\Codes\vGPU-macOS\.nvram-backups\`
- a VM snapshot, `pre-kext-dev-20260928`
- the guest boots from a snapshot-backed APFS volume, so a bad NVRAM does not touch the disk

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

`edit.json`:

```json
{"sets": [{"name": "csr-active-config",
           "guid": "7c436110-ab2a-4bbb-a880-fe41995c9f82",
           "attributes": 7,
           "value_hex": "ff000000"}]}
```

The value byte order is the one thing to get right. `%ff%00%00%00` is the canonical
`nvram csr-active-config=...` spelling and is read little-endian, giving `CSR_ALLOW_UNTRUSTED_KEXTS`
(bit 0) plus every other allow bit. If `csrutil status` still reports enabled after the first
attempt, try `7f000000` (what `csrutil disable` writes) before assuming the store was rejected —
that distinguishes "record not honoured" from "wrong bit".
