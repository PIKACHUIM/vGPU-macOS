# SVGA legacy register block: the numbering for this host

The indices in Linux's `vmware/svga_reg.h` are not sequential — the header skips a block of
values after `SVGA_REG_BITS_PER_PIXEL`, so reading it as "0, 1, 2, …" silently mislabels
every register above 7. This file records the numbering actually in use on this guest,
derived from VMware's own Windows miniport (`driver/vm3dmp.sys`, 9.17.11.0003) plus readings
taken live from our own kext.

**Do not infer indices from the upstream header. Read them off the driver, or off this table.**

## How the table below was established

Three independent sources, and a name only goes in the table when they agree:

1. **The driver's port instructions.** `vm3dmp.sys` has two register accessors:
   - `0x140016a30` — write. `out dx, eax` for the index, then `inc dx; out dx, eax` for the
     value. Called as `write(controller, index, value)` with the index in `edx`.
   - `0x140015f54` — read. `out dx, eax` for the index, then `in eax, dx` on `portBase + 1`.
   - `0x140015fe0` — write and read back in one call. Used for the sync poll.
   Scanning every call site and resolving the immediate `mov edx, imm` that feeds it yields
   the complete set of indices the driver ever touches, and whether it reads or writes each.

2. **Register-ordering topology.** `SVGA_REG_MAX_HEIGHT` includes every framebuffer register
   in its range. `MEM_SIZE` follows `MEM_START` directly. `FENCE_GOAL` follows `FENCE`.
   These adjacencies, which the live readings confirm, pin the block down without guessing.

3. **Live readings from `vgpuProbe` and `vgpuFramebuffer`.** Values have to be mutually
   consistent: `FB_SIZE == round_up(pitch * height, 4096)`, `FB_SIZE <= VRAM_SIZE`,
   `FB_START` must look like a BAR address, `MAX` must be at or above the current mode.
   See `kexts/probe/vgpuProbe.cpp`'s `_checkGeometry`.

## The table

| Index | Name | R/W seen in vm3dmp | Live value on this host | Evidence |
|---|---|---|---|---|
| `0x00` | `ID` | W (renegotiate) | `0x90000002` | `0x14001255b` writes 1 before device reset; reads back as `SVGA_ID_2` |
| `0x01` | `ENABLE` | — | `0x00000001` | set by the firmware before we boot; host reports enabled |
| `0x02` | `WIDTH` | W | `0x400` (1024) | written by the mode setter, `0x14001b216` |
| `0x03` | `HEIGHT` | W | `0x300` (768) | written by the mode setter, `0x14001b226` |
| `0x04` | `MAX_WIDTH` | W (`0x140012d33`) | `0x498` (1176) | host clamps to VRAM; VMware writes `0` = no limit |
| `0x05` | `MAX_HEIGHT` | W (`0x140012d42`) | `0x375` (885) | as above |
| `0x06` | `DEPTH` | — | `0x18` (24) | — |
| `0x07` | `BITS_PER_PIXEL` | W | `0x20` (32) | written by the mode setter, `0x14001b236` |
| `0x08` | `PSEUDOCOLOR` | — | — | part of the MAX_HEIGHT range |
| `0x09` | `RED_MASK` | — | — | part of the MAX_HEIGHT range |
| `0x0A` | `GREEN_MASK` | — | — | part of the MAX_HEIGHT range |
| `0x0B` | `BLUE_MASK` | — | — | part of the MAX_HEIGHT range |
| `0x0C` | `BYTES_PER_LINE` | — | `0x1000` (4096) | **read-only to the guest**; host recomputes on every WIDTH write |
| `0x0D` | `FB_START` | — | `0xf0000000` | equals the VRAM BAR base |
| `0x0E` | `FB_OFFSET` | — | `0x00000000` | the scanout starts at the start of VRAM |
| `0x0F` | `VRAM_SIZE` | R | `0x00400000` (4 MB) | read by the driver at load, `0x140015be2` |
| `0x10` | `FB_SIZE` | — | `0x00300000` (3 MB) | `round_up(4096 * 768, 4096)` |
| `0x11` | `CAPABILITIES` | R | `0xfdffc3e2` | read three places, e.g. `0x140018065` |
| `0x12` | `MEM_START` | — | `0xfb800000` | equals the FIFO BAR base |
| `0x13` | `MEM_SIZE` | R | `0x00040000` (256 KB) | read at `0x140015cbe` |
| `0x14` | `RESET` | R | `0` | **write 0 to reset the device**; done by `VM3DCtrlResetDevice` at `0x14001256f` |
| `0x15` | `SYNC` | W | `0` | writes `1` (flush) at `0x140013da3`, `2` (flush + wait for `FENCE`) at `0x14001a241` |
| `0x16` | `BUSY` | R | `0` | polled until zero after a sync-2, `0x140013de9` |
| `0x17` | `IRQ_STATUS` | — | — | scratch on this host (reads 0); the real IRQ path uses `0x21` |
| `0x18` | `IRQ_MASK` | — | — | — |
| `0x19` | `CONFIG_DONE` | W | — | set after the FIFO is configured; the flag §38.22 identified as the screen-target gate |
| `0x1A` | `SYNC_STAMP_*` | W | — | — |
| `0x1B` | `MEM_SIZE_ACTUAL` | W | `0` | — |
| `0x1C` | `MEM_START_HIGH` | W | — | — |
| `0x1D` | `MEM_SIZE_HIGH` | — | — | — |
| `0x1E` | `MEM_REGS` | R | `0` | read at `0x1400162f4`, clamped against `0x123` (291) |
| `0x1F` | `NUM_DISPLAYS` | R | `1` | read at `0x14001638c`, then used to size the display array |
| `0x20` | `DISPLAY_*` base | W | — | **display register block begins here**, not at `0x22` |
| `0x21` | `DISPLAY_MAX` | R, W | — | host publishes the block size; writing 0 masks and silences interrupts (`0x14001444e`) |
| `0x22` | `MAX_PRIM_IDS` | — | — | — |
| `0x23` | `MAX_GB_OBJECTS` | — | — | — |
| `0x24` | `MEM_SIZE_HIGH`/`MEM_START_HIGH` pair | — | — | — |
| `0x2E` | `TRACES` | R | — | read three places, e.g. `0x14001349b` |
| `0x2F` | `MEM_SIZE_EXTENDED` | R | — | read at `0x1400185a9` |
| `0x32` | `SCREEN_*` block | R | — | read consecutively at `0x140015b79` |
| `0x34` | `DEV_CAP` | R, W | — | **write a `SVGA3D_DEVCAP_*` index, read its value**; looped over `0..0x106` at `0x14001844d` |
| `0x35` | `IRQ_STATUS` (extended) | W | — | — |
| `0x36` | `IRQ_MASK` (extended) | W | — | — |
| `0x37` | `MOS` | R | — | — |
| `0x38` | `HOST_TIME` | R | — | — |
| `0x39` | `TRACES_HIGH` | R | — | — |
| `0x3C` | `SCREENS` | R | — | — |
| `0x44` | `IRQ_STATUS` variant | R | — | — |
| `0x46`–`0x49` | `SCREEN_*` writable | W | — | — |
| `0x4B` | `3D_HWVERSION` | R | — | — |

Indices above `0x20` come from name anchors that cannot be resolved to absolute values
without the host's internal register map; only `0x34` (`DEV_CAP`) is confirmed by behaviour
alone (it is looped over 262 capability indices and each read-back is different).

## The three findings that change previous conclusions

**`0x14` is `RESET`, not `CONFIG_DONE`.** `VM3DCtrlResetDevice` (`0x1400124f0`) writes
`ENABLE = 1`, then `RESET = 0`, then republishes both ring pointers as `-1`. Note the order:
reset comes *after* enable, which is only meaningful if `0x14` resets the device rather than
declaring it configured. The previous revision of `vgpuFramebuffer.cpp` labelled `0x14` as
`CONFIG_DONE`; that label is wrong and must not be trusted.

**`0x15` is `SYNC` (write-only), `0x16` is its status.** Two independent call sites prove it:
`0x140013da3` writes `1` and returns immediately (a flush), while `0x14001a241` writes `2` and
then `0x140013de9` polls `read(0x16)` until it returns zero (a flush-and-wait). A
configuration flag has no reason to be written two different values and then polled.

**The display register block starts at `0x20`, not `0x22`.** The mode setter
`0x14001b1e8` writes `0x20` first, then `WIDTH`, `HEIGHT`, `BITS_PER_PIXEL`, and finally
`0x20` again. The pairing at both ends of a mode change means `0x20` is the *enable* for the
display block, and the block therefore begins there. `0x21` is what the host publishes as the
block size and what the driver writes `0` to when it wants to mask and silence interrupts.

## What this means for the probe

`kexts/probe/vgpuProbe.cpp` carried `CONFIG_DONE = 0x14` and `SYNC = 0x16` — the same two
mistakes, inherited from the same wrong inference. They were corrected here: the probe now
reads `0x14` as `RESET`, `0x15` as `SYNC`, `0x16` as `BUSY`, and `0x19` as `CONFIG_DONE`, and
`_checkGeometry` refuses to trust a snapshot whose registers do not cross-check.

The mislabelling never invalidated a *reading* — every value in the table above came off the
right register once the index was right — but it did invalidate the *interpretation*: reading
`0` from `0x14` and calling it "`CONFIG_DONE` is 0" was describing the reset line, not the
FIFO's configured flag.
