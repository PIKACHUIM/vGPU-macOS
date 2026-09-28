# vGPU-macOS

> Paravirtualized GPU for macOS — hand the guest a GPU it doesn't have, then choose what really renders.

**Status: pre-B0.** Nothing here accelerates anything yet. This repository is at the
feasibility-gate stage, and its architecture is deliberately undecided until the gate is answered
(see [Gate B0](#gate-b0) below).

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

## Current target

The first bring-up target is **macOS in VMware Workstation**:

- the VMware SVGA II device already exists and needs no hypervisor changes;
- its 3D protocol (SVGA3D / VGPU10) is open source — Mesa's `svga` gallium driver and `vmwgfx` are
  the specification — and reaches OpenGL 4.3, which includes compute shaders;
- so one SVGA3D backend would cover every VMware host, on any host GPU, on Windows/Linux/macOS.

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
