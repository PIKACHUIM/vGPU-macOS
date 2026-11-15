"""Fit the .rdata handler table against the SVGA3D DX command enum.

Usage:
    python vmx-dxfit.py

Step 1: for each of the 236 entries at 0x140a3d4e4, read the payload size its
        handler asks for (`mov edx, imm` right before `call 0x140508bb0`).
Step 2: try every base so that cmdId - base lands on an entry of the expected
        size.  Sizes come from the upstream headers:
            DX_DEFINE_CONTEXT        1143  4   {cid}
            DX_BIND_CONTEXT          1145 12   {cid, mobid, validContents}
            DX_SET_TOPOLOGY          1160  4   {topology}
            DX_SET_RENDERTARGETS     1161  4   + trailing rtv array
            DX_SET_DEPTHSTENCIL_STATE 1163 8   {id, stencilRef}
            DX_SET_RASTERIZER_STATE  1164  4   {id}
            DX_CLEAR_RENDERTARGET_VIEW 1176 20 {rtvId, rgba[4]}
            DX_GROW_COTABLE          1237 16   {cid, mobid, type, validSize}
"""
import struct

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IB = 0x140000000
TABLE = 0x140a3d4e4
N = 236
READER = 0x140508bb0

EXPECT = {
    1143: 4,      # DX_DEFINE_CONTEXT
    1145: 12,     # DX_BIND_CONTEXT
    1160: 4,      # DX_SET_TOPOLOGY
    1161: 4,      # DX_SET_RENDERTARGETS
    1163: 8,      # DX_SET_DEPTHSTENCIL_STATE
    1164: 4,      # DX_SET_RASTERIZER_STATE
    1176: 20,     # DX_CLEAR_RENDERTARGET_VIEW
    1237: 16,     # DX_GROW_COTABLE
}
NAMES = {
    1143: "DX_DEFINE_CONTEXT", 1145: "DX_BIND_CONTEXT", 1160: "DX_SET_TOPOLOGY",
    1161: "DX_SET_RENDERTARGETS", 1163: "DX_SET_DEPTHSTENCIL_STATE",
    1164: "DX_SET_RASTERIZER_STATE", 1176: "DX_CLEAR_RENDERTARGET_VIEW",
    1237: "DX_GROW_COTABLE",
}

data = open(PATH, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
so = pe + 24 + optsz
secs = {}
for i in range(nsec):
    o = so + 40 * i
    nm = data[o:o + 8].rstrip(b"\0").decode("latin1")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs[nm] = (va, vs, raw, rs)


def v2r(va):
    rva = va - IB
    for nm, (sva, vs, raw, rs) in secs.items():
        if sva <= rva < sva + max(vs, rs):
            o = raw + (rva - sva)
            if o < raw + rs:
                return o
    return None


def payload_size(fn):
    ro = v2r(fn)
    if ro is None:
        return None
    for k in range(0, 48):
        if data[ro + k] != 0xBA:            # mov edx, imm32
            continue
        imm = struct.unpack_from("<I", data, ro + k + 1)[0]
        for j in range(k + 5, min(k + 17, 64)):
            if data[ro + j] == 0xE8:        # call rel32
                rel = struct.unpack_from("<i", data, ro + j + 1)[0]
                if fn + j + 5 + rel == READER:
                    return imm
    return None


handlers = []
sizes = []
for i in range(N):
    o = v2r(TABLE + i * 4)
    fn = struct.unpack_from("<I", data, o)[0] + IB
    handlers.append(fn)
    sizes.append(payload_size(fn))

best = []
for base in range(1000, 1200):
    hit = 0
    tot = 0
    detail = []
    for cid, want in EXPECT.items():
        i = cid - base
        if not (0 <= i < N):
            continue
        tot += 1
        got = sizes[i]
        ok = got == want
        hit += ok
        detail.append((cid, i, got, want, handlers[i], ok))
    if tot >= 4:
        best.append((hit, base, tot, detail))
best.sort(key=lambda t: (-t[0], -t[2]))

for hit, base, tot, detail in best[:3]:
    print(f"=== base {base}  ({hit}/{tot} size matches)  => table[0] = cmd {base}")
    for cid, i, got, want, fn, ok in detail:
        print(f"   {'OK ' if ok else 'MIS'} {NAMES[cid]:26s} cmd {cid:5d} idx {i:3d} "
              f"handler {fn:#x} size got {got} want {want}")
    print()
