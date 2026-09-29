# vGPU-macOS

> Paravirtualized GPU for macOS — hand the guest a GPU it doesn't have, then choose what really renders.

**Status: B0 passed — a native macOS display driver is running on real VRAM.**

`kexts/framebuffer` is a working `IOFramebuffer` driver for the VMware SVGA II device. On the
test guest it loads at every boot, evicts the firmware-provided framebuffer, owns the console,
and WindowServer composites the whole desktop into the adapter's own VRAM. The guest reports
`Chipset Model: VMware SVGA 3D GPU`. See [What works today](#what-works-today) and
[docs/kext-loading.md](docs/kext-loading.md).

Everything beyond a framebuffer — 3D, Metal, compute, video — is blocked by the hypervisor on
this platform, and that is a measured finding rather than a guess: VMware refuses to power a
macOS guest on with 3D acceleration enabled at all
([docs/vmware-svga-capabilities.md](docs/vmware-svga-capabilities.md)).

---

## What this is

macOS will only render through a GPU it can drive, and it can only drive a GPU in one of two ways:

- **Track A — Apple's own driver.** The guest uses the in-box `AppleParavirtGPU.kext`. This requires
  the host to present Apple's paravirtual device, which requires owning the hypervisor's device
  model. Only QEMU-family VMMs qualify.
- **Track B — your own driver.** The device protocol is irrelevant; you must ship a macOS graphics
  accelerator instead. Expensive, but open to every VMM.

This project explores a third option that sits between them:

> **H1 — fabricate the device inside the guest.** A macOS extension can synthesise a PCI device
> that the kernel will enumerate (this is already demonstrated in production by
> `MacHyperVSupport`'s graphics bridge). If we synthesise one carrying the Apple paravirtual GPU's
> vendor/device IDs, macOS's *own* driver attaches — and the hardest layer, the private Metal driver
> interface, is supplied by Apple rather than by us. We then only implement the paravirtual device's
> far side and a pluggable backend.

If H1 holds, the project becomes: **reuse an existing paravirtual command-stream decoder, add a
device front-end and one backend per transport.** If it does not hold, the project falls back to
Track B. Both are documented in [docs/](docs/).

## Gate B0

H1 is a hypothesis, not a finding. Before any backend work, B0 must answer:

| # | Question | Cost |
|---|---|---|
| 1 | What does `AppleParavirtGPU.kext` match on (`IOPCIMatch`, ACPI properties, option ROM)? | read its `Info.plist` |
| 2 | Does it exist on x86_64 / arm64, and does it live in the System Kernel Collection? | `ioreg` + a file listing |
| 3 | Can a synthesised device win the probe race (`IOProbeScore`), or start without a real device? | minimal kext experiment |
| 4 | Does the paravirtual protocol assume host-mapped memory (`PGPhysicalMemoryRange`, trace ranges)? | protocol reading |
| 5 | How complete is the existing decoder? Apple's driver demands conformance, not "it lights up". | opcode coverage audit |

**Do not start backend work before B0 answers 1 and 2.** They are nearly free and they decide the
project's shape.

## What works today

A macOS guest in VMware Workstation, with no VMware Tools and no bootloader, whose display
driver is ours:

```
kmutil showloaded
  134  0  0xffffff7f96a8c000  0x1ff5  0x1ff5  com.vgpu.framebuffer (1.0.0)

ioreg -rc IOFramebuffer
  +-o vgpuFramebuffer  <class vgpuFramebuffer, registered, matched, active>

kernel log
  vgpu-fb: start, provider=IOPCIDevice
  vgpu-fb: BAR1 phys 0xf0000000 length 134217728
  vgpu-fb: inheriting 1024x768 pitch 4096 at VRAM offset 0x0
  vgpu-fb: aperture phys 0xf0000000 length 3145728
  vgpu-fb: started
  vgpu-fb: enableController                <- called by IOGraphicsFamily
```

It works by **inheriting the mode rather than setting one**: the device has already been
programmed by firmware and the boot-time NDRV driver, so the kext reads
`SVGA_REG_FB_OFFSET`, `SVGA_REG_BYTES_PER_LINE` and the current width and height out of the
device's register port (BAR0), and offers exactly that geometry — one mode, the device's own
pitch as `bytesPerRow`, and an aperture that is a sub-range of VRAM starting at the
framebuffer offset. Nothing is ever programmed, so nothing can be left in a state that
cannot be recovered.

Install, on the guest, as root:

```sh
cd kexts/framebuffer && sh build.sh && sudo sh install.sh && reboot
```

`install.sh` signs the kext with a self-signed identity carrying an organisational unit (the
team identifier kext approval is recorded against), inserts the matching `kext_policy` row,
and asks `kernelmanagerd` to rebuild the auxiliary kernel collection — which it does
immediately, before the reboot. Recovery, over ssh, if the console is ever lost:

```sh
rm -rf /Library/Extensions/vgpuFramebuffer.kext && kmutil clear-staging && reboot
```

The network stack does not depend on the console.

## Current target

Measured on 2026-09-29: **VMware refuses to power a macOS guest on with 3D acceleration
enabled.** Adding `mks.enable3d = "TRUE"` to the configuration makes `vmrun start` fail with
"Unknown error" before any VMX process exists; removing it restores a normal boot. The
hypervisor's own log says `SVGA3dCaps: host, at power on (3d disabled)`, and in the guest the
command FIFO has never been initialised and no SVGA3D capabilities are published — the 3D
path is not dormant waiting for a driver, it is switched off at power-on validation.

So the framebuffer above is the whole of what the VMware route can give a macOS guest, and
the interesting targets move elsewhere:

- **3D, Metal and compute** require the hypervisor's device model, which means the
  paravirtualisation route: present Apple's paravirtual GPU device from a VMM whose device
  model we control, and let macOS's own `AppleParavirtGPU.kext` attach (track A / H1 below).
  See [docs/vmware-svga-capabilities.md](docs/vmware-svga-capabilities.md) for the measured
  capability picture and the AIR → SPIR-V → NIR → DXBC path a shader compiler would have to
  take on any SVGA-like backend.
- **Video encode and decode** exist in neither route: SVGA3D's command set has no video
  commands, and the Apple paravirtualisation stack keeps decode on a separate channel
  (`AppleVideoToolboxParavirtualization`) that a virtual device would have to implement too.
- Enabling darwin-guest 3D on a Windows or Linux host, if it is possible at all, is a
  patch-the-hypervisor problem of the same kind as running macOS guests there in the first
  place (see drdonk/unlocker), not a guest-side one. [docs/plan.md](docs/plan.md) tracks it.

## Layout

```
crates/     Rust: command-stream model, wire formats, one crate per backend
kexts/      Objective-C++ macOS extensions (framebuffer / accelerator front-ends)
docs/       design documents and research notes
tools/      host-side helpers (SSH runner, reference fetcher)
refs/       upstream checkouts — git-ignored, recreated by tools/fetch-refs.sh
```

## Working on the test guest

Credentials live in `.env.local` (git-ignored, never committed):

```
VG_HOST=192.168.9.131
VG_PORT=22
VG_USER=<user>
VG_PASS=<password>
```

Then:

```sh
python tools/mssh.py -- uname -a
python tools/mssh.py -- "sw_vers; csrutil status"
python tools/mssh.py --put ./local/file /tmp/file
```

## References

Upstream projects this work builds on or borrows from — none of them are vendored into history:

| Project | Used for | Licence |
|---|---|---|
| [steelbrain/reims-vgpu](https://github.com/steelbrain/reims-vgpu) | paravirtual device model, wire formats, QEMU shim, snapshot test rig | LGPL-3.0-or-later |
| [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | Metal AIR → SPIR-V translation | LGPL-3.0-or-later |
| [acidanthera/MacHyperVSupport](https://github.com/acidanthera/MacHyperVSupport) | synthesised PCI device + `IOFramebuffer` front-end, VMBus client | BSD-3-Clause style |
| Mesa `svga` / `vmwgfx` | SVGA3D protocol specification and DXBC emitter | MIT |

## Licence

AGPL-3.0. Upstream components keep their own licences; check compatibility before copying code in
rather than reimplementing it.

## Disclaimer

Not affiliated with, sponsored by, or endorsed by Apple Inc. Apple, macOS and Metal are trademarks
of Apple Inc. Running macOS on non-Apple hardware violates Apple's licence agreement in most
jurisdictions; this repository documents research and does not ship any Apple binaries.
