# VMwareGfx.kext — VMware's own macOS graphics driver, dissected

Found inside `darwin.iso`, which ships with VMware Workstation 17.6+ (`darwin.iso` in the
installation directory) and is what "Install VMware Tools" installs on a macOS guest. This is
the driver the project should have been compared against from the start, and it settles three
arguments at once.

Extracted with `tools/isols.py` (the image is an Apple UDTO with a partition map, not
ISO9660, so no ISO tool reads it) and `pkgutil --expand-full` on
`Install VMware Tools.app/Contents/Resources/VMware Tools.pkg`.

## What it is

`VMwareGfx.kext`, version 12.1.1 (build 2248.42.50), a 107 KB x86_64 kernel extension at
`/Library/Extensions/VMwareGfx.kext`.

Its personality is **almost identical to ours**:

| | VMwareGfx | vgpuFramebuffer |
|---|---|---|
| provider | `IOPCIDevice` | `IOPCIDevice` |
| match | `IOPCIPrimaryMatch 0x040515AD` | `IOPCIMatch 0x040515AD` |
| category | `IOFramebuffer` | `IOFramebuffer` |
| probe score | 30001 | 50000 |
| libraries | IOACPIFamily + IOGraphicsFamily + IOPCIFamily + kpis | IOGraphicsFamily + kpis |
| user client | `VMwareGfxUserClient` | none yet |
| `OSBundleRequired` | Safe Boot | unset |

## What it does not have

`strings` over the binary finds **zero** occurrences of `IOAccelerator`, `MTLDevice` or
`MetalPlugin`. There is no Metal plugin bundle in the package — the only other graphics file
is `libresolutionSet.dylib` and the `vmware-resolutionSet` tool.

So VMware's own macOS driver is a framebuffer with 2D and modesetting. No 3D, no Metal, no
accelerator. This is independent confirmation of
[docs/vmware-svga-capabilities.md](docs/vmware-svga-capabilities.md): the absence of 3D on a
macOS guest is a product decision, not a missing driver, and the vendor's own driver does not
attempt it.

## What it has that we do not (yet)

The binary strings show a real command-FIFO implementation and a modesetting path:

```
FIFOInit  FIFOReserve  FIFOCommit
FIFO: min=%u, size=%u
FIFO_CAP_RESERVE=0x%08x failed
FIFO_CAP_CURSOR_BYPASS_3=0x%08x failed
Extended FIFO not available
Set resolution to %ux%u
Requested custom resolution %ux%u.
Last custom resolution: %ux%u
```

Three concrete things worth adopting, in order:

1. **Modesetting.** "Set resolution to %ux%u" means it programs `SVGA_REG_WIDTH`/`HEIGHT`
   (and re-reads `BYTES_PER_LINE`/`FB_OFFSET`) through the same index/value port pair we
   already read. That is what would turn our single inherited mode into a real mode list, and
   it is the prerequisite for a `vmware-resolutionSet`-style tool. Our kext currently returns
   `kIOReturnUnsupportedMode` from `setDisplayMode` for anything but the inherited mode.
2. **A user client** (`VMwareGfxUserClient`) so a userspace tool can drive the driver. Needed
   for custom resolutions without a kernel-side trigger.
3. **FIFO command submission** — `FIFOReserve`/`FIFOCommit` with the reserve capability —
   which is the transport any 2D acceleration (cursor bypass, rect copy, dirty-rect updates)
   would ride on. Note that it uses the FIFO for 2D and cursor only; nothing suggests it
   submits SVGA3D commands.

Also worth copying: `OSBundleRequired: Safe Boot`, so the driver keeps working in safe mode
(ours is currently skipped there, which makes a safe-mode recovery awkward).

## What this means for the project

The framebuffer stage is now at parity with the vendor's own driver, minus custom resolutions
and a user client. Those two are well-scoped, low-risk, and the vendor binary proves the
device can do them; they are the natural next step and they make the guest genuinely
pleasant to use (real VRAM reporting, any resolution, dynamic window resize).

They are still not acceleration. There is no acceleration to be had on this platform, by us
or by VMware, and the vendor's driver is the proof.
