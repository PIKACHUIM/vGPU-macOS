# VMware's virtual GPU: what it actually accelerates, and how Metal maps onto it

This answers three questions: what acceleration the VMware virtual GPU provides, whether it
covers OpenGL and video codecs, and what path leads from Metal to it. Facts measured on this
guest are marked **[measured]**; the rest is from VMware's and Mesa's documentation.

## What the device is

`display@F` is VMware SVGA II, vendor `0x15AD` device `0x0405`. It is a real PCI device with
four BARs **[measured]**:

| BAR | physical | length | what it is |
|---|---|---|---|
| 0 | `0x2040` | 16 | I/O port block: index port at +0, value port at +1 |
| 1 | `0xf0000000` | 128 MB | VRAM |
| 2 | `0xfb800000` | 8 MB | device memory / FIFO region |
| 3 | `0xe8000` | 32 KB | (not yet identified) |

Its registers are not memory mapped. Write a register index to BAR0+0, then read or write
BAR0+1. Reading the live state through them works from our own kext **[measured]**:

```
svg a ports 0x2040/0x2041, pci command 0x7
svg a id 0x90000002 enable 1
svg a mode 1024x768 depth 24 bpp 32 pitch 4096
svg a max 6688x5016
svg a fb offset 0x0 fb size 0x300000
svg a vram 0x8000000 fb start 0xf0000000 caps 0xfdff83e2
```

Three things follow. The device is already enabled and in SVGA mode 2. The visible framebuffer
is VRAM offset 0, pitched at 4096, which interests the same 3 MB of VRAM that our aperture
already covers. And the device is willing to go far beyond 1024x768: `SVGA_REG_MAX_WIDTH` and
`SVGA_REG_MAX_HEIGHT` are 6688x5016.

## 3D CAN be turned on for a macOS guest — the earlier conclusion was wrong

Everything above about the device is unchanged. This section was rewritten on 2026-09-29 after
the earlier "decisive limitation" turned out to be a misdiagnosis.

**What we first believed.** With no 3D setting in the VM, the host log said
`SVGA3dCaps: host, at power on (3d disabled)`, the FIFO was dormant and no SVGA3D capabilities
were published. Adding `mks.enable3d = "TRUE"` made power-on fail outright (`vmrun start`
returned an error, no VMX process, nothing in the log). We concluded VMware validates the
configuration at power-on and refuses 3D for a darwin guest — a switch "our driver cannot
influence".

**That was wrong.** The refusal is not a hard-coded darwin check. The 3D decision in the
host's embedded configuration logic is:

```
(mks-is-3d-enabled vga-only) =
  (and (not vga-only)
       (hwversion-get-bool "mks.enable3d.available")   ; capability lookup
       (vmconfig-getbool #f "mks.enable3d"))            ; the .vmx switch
```

`mks.enable3d.available` is a per-hardware-version *capability key*, and capability keys can
be overridden from the `.vmx` like ordinary settings. The original test set only
`mks.enable3d` and never the capability, so power-on failed.

**Measured on 2026-09-29, all three combinations** (VM powered off for each edit):

| `.vmx` keys | Power on | 3D state |
|---|---|---|
| `mks.enable3d = "TRUE"` alone | refused | — |
| `mks.enable3d` + `svga.allowSVGA3 = "TRUE"` | refused | — |
| `mks.enable3d` + `mks.enable3d.available = "TRUE"` | **accepted** | `SVGA3dCaps: host, at power on (3d enabled)` |

The working recipe is therefore exactly two lines:

```
mks.enable3d = "TRUE"
mks.enable3d.available = "TRUE"
```

With them, the host log shows the full 3D bring-up: `MKSRenderMain: PowerOn allowed BasicOps
ISB DX12 DX11 DX11Basic VK`, `Sandbox Renderer: VKRenderer` (the Windows host renders through
Vulkan, not through a Metal backend — none is needed), `SVGA: FIFO capabilities 0x0000077f`,
`SVGA3dCaps: guest, compatibility level: 10` (vGPU10), and `svga.supports3D bool 1` with the
whole feature list (`svga.gl43`, `svga.sm5`, `svga.sm41`, multisample up to 8x, BC6/7 ...).
The guest boots normally; our framebuffer kext loads, the display name stays
"VMware SVGA 3D GPU", WindowServer composites as before, no regression **[measured]**.

Operational notes:

- Edit the `.vmx` only while the VM is off. VMware rewrites the file from its in-memory
  configuration at power-off and will silently resurrect keys you removed while it ran.
- `svga.allowSVGA3` is a different, independent gate (the SVGA3 *device model*) and is not
  needed; with it present but the capability absent, power-on is still refused.

**What this does and does not reopen.** The SVGA3D device, its FIFO command stream and the
vGPU10 feature set are now switched on for a darwin guest — the hardware door is open. What
is still missing is the guest-side software: the FIFO only comes alive when a guest driver
initialises it (at kext start we still read `fifo min 0 max 0 ... 3d hw version 0x0`, because
VMware's own macOS driver, VMwareGfx.kext, is 2D-only). So the mission shifts from "the
platform refuses" to "we must write the driver": FIFO bring-up, then the Metal-to-SVGA3D
path described below. Metal itself has no driver to attach to yet; that work is now
*possible*, not done. Video encode/decode remain unavailable: SVGA3D has no video commands
(see the acceleration table below).

## FIFO bring-up: SVGA3D is live from inside the guest (measured 2026-09-29)

With 3D enabled at the host, the remaining step was the guest-side one: no macOS driver has
ever configured the SVGA command FIFO, so it sat dormant (`min 0 max 0`) and the host
published nothing into it. `vgpuFramebuffer` now does the bring-up at kext start, following
the same sequence Linux's vmwgfx uses:

1. `SVGA_FIFO_MIN = 288 regs * 4 bytes` — reserving room for the extended FIFO registers is
   what makes the 3D capability block and the devcap set exist at all;
2. `SVGA_FIFO_MAX = SVGA_REG_MEM_SIZE` (256 KB on this guest, inside the 8 MB BAR2);
3. `NEXT_CMD = STOP = MIN` (empty command stream);
4. `SVGA_REG_CONFIG_DONE = 1`.

Measured result, from the guest's own kernel log:

```
fifo[after ] min 1152 max 262144 next_cmd 1152 stop 1152 caps 0x77f busy 0
SVGA3D LIVE (DEVCAP_3D=1)
```

And the command stream itself is verified end-to-end: the kext submits one `SVGA_CMD_FENCE`
(id 30, one parameter dword) with the reserve protocol — `SVGA_FIFO_RESERVED = 8`, write two
dwords at `NEXT_CMD`, advance `NEXT_CMD`, clear `RESERVED` — and polls `SVGA_FIFO_FENCE`:

```
fence 0x31415926 submitted at offset 1152 (reserve=1), polling...
FIFO command stream VERIFIED: fence 0x31415926 acked (next_cmd 1160 stop 1160, 194245 spins)
```

The host consumed the command and acknowledged it in well under a millisecond, with
`NEXT_CMD` and `STOP` fully caught up afterwards. Submission, host processing and the fence
acknowledgement path are all measured working.

Two operational notes measured along the way:

- The host republishes its FIFO capabilities on the `CONFIG_DONE` 0→1 transition only. On a
  later guest boot (the FIFO memory survives; see below) the capabilities read zero until
  the sequence is run again, so the kext always runs it — it is idempotent for an empty
  stream, and foreign partitions (any other `FIFO_MIN`) are left untouched.
- `SVGA_CMD_FENCE` is a raw legacy command: command id dword, then parameters, without the
  `SVGA3dCmdHeader` wrapper that the 3D command set uses.

The host accepted the configuration immediately and published its FIFO capability register
(0x77f, matching what the host log recorded at power-on). The authoritative 3D verdict comes
through the `SVGA_REG_DEV_CAP` backdoor (write a `SVGA3D_DEVCAP_*` index, read the value from
the same register): `DEVCAP_3D` (= index 0) reads **1**. This is byte-for-byte the check
Linux's `vmw_fifo_have_3d()` performs for a GBOBJECTS device, and this device is one — its
legacy `SVGA_FIFO_3D_HWVERSION` register stays zero by design, which is expected and not a
failure.

Measured devcaps (first 32; full decode against `svga3d_devcaps.h` is a later stage's job):

```
devcap[ 0.. 7] 00000001 00000008 00000008 00000008 00000007 00000001 0000000d 00000001
devcap[ 8..15] 00000008 00000001 00000001 00000004 00000001 00000001 00000001 00000001
devcap[16..23] 00000001 433d0000 00000014 00008000 00008000 00004000 00008000 00008000
devcap[24..31] 00000010 001fffff 000fffff 0000ffff 0000ffff 00000020 00000020 03ffffff
```

Implementation notes that cost time and are worth keeping:

- **Register offsets.** An earlier revision of the kext carried `3D_HWVERSION=28` and
  `3D_CAPS=34`, from a misremembered pre-extended-FIFO layout. The real layout (vmwgfx's
  `device_include/svga_reg.h`, Xorg's `vmware` driver) is `CAPABILITIES=4`,
  `3D_HWVERSION=7`, `3D_CAPS=32..287`, `GUEST_3D_HWVERSION=288`, `BUSY=290`. The first four
  FIFO registers are byte offsets; every other index is a dword index.
- **The FIFO survives a guest reboot.** No device reset happens unless the VM powers off
  completely, so BAR2's configuration persists. The kext recognises its own partition
  (`min == 1152`) on later boots, skips re-initialisation, and goes straight to the
  capability readouts. It declines to touch a FIFO partitioned by anyone else.
- **Version handshake.** `SVGA3D_HWVERSION_CURRENT` is `WS8_B1 = (2<<16)|1 = 0x00020001`
  (`SVGA3D_MAKE_HWVERSION`), and the guest announces it in `GUEST_3D_HWVERSION`; there is no
  host response to wait for on this device.
- **What is still ahead** for a real driver: guest memory objects (MOBs) for the GBOBJECTS
  path, surface definition, DX context creation and the command stream itself. The FIFO is
  the transport for all of it, and it is now up.

## GB object model is UP: OTables, a MOB and a surface (measured 2026-09-29)

The kext now also brings up the guest-backed object infrastructure. One contiguous 64 KiB
working buffer holds three page-table pages, the object tables and a data region; the page
tables are filled with PPN64s of their target pages, so nothing has to be physically
contiguous beyond the 4 KiB alignment of the buffer itself.

Measured sequence, every step fence-verified from inside the guest:

```
gb: working buffer phys 0x1478a000 len 65536
gb: MOB otable base set (ppn 83850, 16384 bytes)
gb: SURFACE otable base set (ppn 83851, 4608 bytes)
gb: data MOB 1 defined (ppn 83852, 16384 bytes, PT64_0)
gb: GB SURFACE 1 defined (256x256 X8R8G8B8 on MOB 1) -- GB object model is UP
```

with zero errors in the host's own log. The working recipe, all of it from vmwgfx's
definitions:

- `SVGA_3D_CMD_SET_OTABLE_BASE64` (1115) registers each object table: {type,
  baseAddress PPN64, sizeInBytes, validSizeInBytes=0, ptDepth=SVGA3D_MOBFMT_PT64_0}.
- `SVGA_3D_CMD_DEFINE_GB_MOB64` (1135) defines a MOB: {mobid, ptDepth, base PPN64,
  sizeInBytes}. With PT64_0 the base PPN points at one page-table page of PPN64 entries.
  The guest writes the matching `SVGAOTableMobEntry` {ptDepth, sizeInBytes, base} into the
  MOB table itself, before the command.
- `SVGA_3D_CMD_DEFINE_GB_SURFACE` (1097) defines a surface: {sid, surfaceFlags as a **64-bit**
  field, format, numMipLevels, multisampleCount, autogenFilter, size{w,h,d}} — **40 bytes**.
  The guest fills the `SVGAOTableSurfaceEntry` (72 bytes) afterwards.
- Every 3D command carries `SVGA3dCmdHeader {id, size}`, and **the size must be exact**: a
  header size of 36 instead of 40 made the host abort FIFO processing silently, and the
  next fence never came back. That failure mode is the reason every command here is
  fence-verified the moment it is submitted.

What this buys: the object substrate a real driver builds on is registered and validated —
contexts (DX_DEFINE_CONTEXT), shaders and render targets all live in the same tables, and
guest RAM is now addressable by the host's 3D engine through the data MOB.

## DX command set is OPEN: DX context created (measured 2026-10-05)

The DX (vGPU10) path is confirmed available and entered. Measured devcaps through the
backdoor:

```
gb: devcaps DXCONTEXT=1 SM41=1 GL43=1
gb: DXCONTEXT otable base set (ppn 126903, 2048 bytes)
gb: DX CONTEXT 1 created -- DX command set is OPEN
```

with zero errors in the host's log. The DX additions to the bring-up sequence:

- `SVGA3D_DEVCAP_DXCONTEXT` (devcap index 95) gates the whole DX command family; it reads 1,
  as do `SM41` (244) and `GL43` (261) — the device claims the full shader-model and GL 4.3
  feature set.
- The DX context object table (`SVGA_OTABLE_DXCONTEXT` = 5) holds
  `SVGAOTableDXContextEntry {uint32 cid; SVGAMobId mobid;}` — 8 bytes per context. It gets
  its own `SET_OTABLE_BASE64` like the others.
- `SVGA_3D_CMD_DX_DEFINE_CONTEXT` (1143) is just `{uint32 cid}` — 4 bytes — with the cid
  chosen by the guest. The context's working memory (the COTable storage) is bound later,
  not at definition time.

From here, rendering work is a matter of context state: binding the surface as a render
target view, clearing, and reading the result back through the MOB — the round trip that
would prove the host's 3D engine actually computed into guest memory.

## Bisect: guest FIFO ownership kills the legacy present path (measured 2026-10-06)

A boot freeze appeared between the October 5 milestones and was bisected with staged
switches in the kext (`kFbStage` / `kGbStage`). Findings, all measured on this host:

| kext configuration | display |
|---|---|
| kext loaded, console takeover, **no FIFO writes at all** | **works** — lock screen, desktop, everything |
| + FIFO init (`FIFO_MIN/MAX/CONFIG_DONE`), no guest commands | **frozen** at the boot progress screen |
| + `SVGA_CMD_FENCE` (acked), + full-screen `SVGA_CMD_UPDATE` (acked) | still frozen |
| + GB objects / DX context (all fence-acked) | still frozen |
| kext unloaded entirely | works |

The guest system keeps booting fine over SSH in every frozen case (WindowServer and
loginwindow run); only presentation dies. So: **the moment the guest takes ownership of the
FIFO with 3D enabled, the MKS stops auto-presenting the framebuffer** — it switches to the
screen-target presentation model and the legacy scanout is gone. Acked `SVGA_CMD_UPDATE`
does not bring it back. Notably the same sequence presented fine on September 29; nothing on
the host changed since (GPU driver and VMware binaries date-checked), which makes the
trigger state-dependent rather than version-dependent — treat it as a platform behaviour
boundary, not a bug we introduced.

Consequence for the driver: **FIFO ownership and the present path must land together.**
The next milestone is the screen-object present path — `SVGA_CMD_DEFINE_SCREEN`, a
GMRFB/GB-surface as the source, and the blit/DMA that feeds it — so that our kext keeps
presenting the console after `CONFIG_DONE`. Until then the kext ships with `kFbStage=0`
(full display, SVGA3D machinery dormant behind the switch).

Two real bugs fixed en route to the bisect:

- **Stack overflow**: the SVGA3D command builder used a `UInt32 words[10]` array while
  `DEFINE_GB_SURFACE` writes twelve dwords — an 8-byte stack corruption in the kernel that
  froze boots nondeterministically. Now `words[16]` with a comment.
- `SVGA3D_HWVERSION_CURRENT` is `0x00020001`, not `0x08000001`; the guest announces it in
  `SVGA_FIFO_GUEST_3D_HWVERSION`.

## Acceleration: what you get

| Capability | Status |
|---|---|
| 3D | Yes, via SVGA3D. The guest builds commands into a FIFO; the host renders them |
| OpenGL | **Yes.** vGPU10 gives OpenGL 4.1; 4.3 with Workstation 17 / Fusion 13, VM hardware version 20+, and a guest driver from Mesa 22 or later. Measured elsewhere: `Max core profile version: 4.3`, renderer `SVGA3D` |
| OpenGL ES | 3.0 (2.0 if the guest sets `SVGA_VGPU10=0`) |
| Direct3D | DX11 feature level 11_0, via VMware Tools' WDDM 1.2 driver. VMware's own table: Tools 12.0 → WDDM 1.2, DX11, OpenGL 4.3 |
| Compute | Yes — arrives with OpenGL 4.3 / D3D11 compute |
| Vulkan | **No.** Workstation does not expose Vulkan to guests, whatever the host supports |
| CUDA / OptiX | **No.** Workstation has no PCIe passthrough; `nvidia-smi` in a guest returns nothing |
| Video encode | **No.** NVENC-class hardware encoding is not exposed to guests |
| Video decode | **No.** There is no decode engine in SVGA3D and no DXVA/VA-API path. `SVGA3D`'s command set has no video commands at all |

So the answer to "does it support OpenGL" is yes, up to 4.3, and the answer to "does it
accelerate codecs" is **no, neither encode nor decode**. This is the sharpest difference from
the Apple paravirtualisation stack we studied earlier, which ships an independent
`AppleVideoToolboxParavirtualization` channel precisely for decode. On VMware there is nothing
to attach to: VideoToolbox would have to run entirely in software, and the same is true of the
display and audio channels in the paravirt design. Any plan that assumes "GPU acceleration"
implies accelerated video is wrong on this platform.

## Shaders are DXBC, not SPIR-V

A vGPU10 shader object carries **DXBC** — the D3D11 bytecode container. That is a consequence of
the guest-side driver model: SVGA3D mimics D3D11 closely enough that the Windows guest driver
compiles HLSL to DXBC and hands it over.

Metal's IR is a different thing entirely. A `MTLLibrary` holds **AIR**, which is LLVM bitcode.
Neither is SPIR-V. So the shader path has to be:

```
Metal shader  ->  AIR  ->  SPIR-V  ->  [ NIR ]  ->  DXBC  ->  SVGA3D shader object
```

The two realistic ways to get from SPIR-V to DXBC:

1. **Reuse Mesa's svga backend** (MIT). `spirv_to_nir` turns SPIR-V into NIR, and Mesa's svga
   driver already has a DXBC emitter downstream of it. This is the shortest route because the
   emitter is written, tested, and maintained by the same project that maintains the reference
   SVGA guest driver.
2. `spirv-cross` to HLSL, then `dxc` to DXIL/DXBC. More moving parts, but it uses tooling that
   is also actively maintained.

Either way, the first hop — AIR to SPIR-V — is what `metal2vulkan` does, and it works from AIR
bitcode rather than from MSL source. That layer is not something to write again.

## What Metal expects that SVGA3D does not have

This is the part that decides how much of Metal can be implemented, so it has to be settled
before writing a driver rather than after. SVGA3D froze at a 2012-era feature set, and the
following Metal concepts have no counterpart in it:

- argument buffers and `MTLBuffer`-as-descriptor-heap
- tile memory and programmable blending
- mesh and object shaders
- hardware ray tracing, and the whole `MTLAccelerationStructure` family
- Metal's memory model: `MTLHeap`, aliasing, `MTLResource` hazard tracking
- imageblocks, and `simdgroup` matrix operations beyond what compute can express

The practical consequence is that "a Metal driver" is not one target but a range. A useful
first target is the subset that CoreGraphics, Core Animation and WindowServer actually need:
linear and tiled images, blending, and enough compute for Core Image. MPS, Metal Performance
Shaders, and anything expecting argument buffers come later or not at all.

## Consequences for this project

- **The API layer is the work, not the shading.** SVGA3D has to be driven from Metal's object
  model: devices, queues, command buffers, encoders, resources, synchronisation. That layer
  does not exist in Mesa and must be written. DXVK and vkd3d-proton are the reference for how
  much this costs.
- **Vulkan cannot be the host-side shortcut**, because there is no Vulkan in the guest. A
  Vulkan backend would have to be a *host-side* renderer reached through something else, which
  means the SVGA3D protocol is the only guest-visible door — a different shape from the QEMU
  path, where a Vulkan backend sits directly behind the paravirt protocol.
- **Video is software-only here.** Worth stating plainly in the plan rather than discovering it
  after the GPU works.
- The device's real ceiling is 6688x5016 and its current mode is already programmed and being
  scanned out. The cheapest first target is to *inherit* that mode rather than set one, which
  is the step being attempted now (see task #14).
