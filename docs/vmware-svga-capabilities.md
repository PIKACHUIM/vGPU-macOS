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
