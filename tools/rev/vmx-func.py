import struct,sys,re
IB=0x140000000
PATH=r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
data=open(PATH,"rb").read()
pe=struct.unpack_from("<I",data,0x3C)[0]
nsec=struct.unpack_from("<H",data,pe+6)[0]
optsz=struct.unpack_from("<H",data,pe+20)[0]
so=pe+24+optsz
secs={}
for i in range(nsec):
    o=so+40*i
    nm=data[o:o+8].rstrip(b"\0").decode("latin1")
    vs,va,rs,raw=struct.unpack_from("<IIII",data,o+8)
    secs[nm]=(va,vs,raw,rs)
def v2r(va):
    rva=va-IB
    for nm,(sva,vs,raw,rs) in secs.items():
        if sva<=rva<sva+max(vs,rs):
            o=raw+(rva-sva)
            if o<raw+rs: return o
    return None
pv,ps,pr,psz=secs[".pdata"]
funcs=[]
for i in range(len(data[pr:pr+psz])//12):
    b,e,u=struct.unpack_from("<III",data,pr+i*12)
    if b: funcs.append((IB+b,IB+e))
funcs.sort()
def containing(va):
    lo,hi=0,len(funcs)-1; best=None
    while lo<=hi:
        m=(lo+hi)//2
        if funcs[m][0]<=va: best=funcs[m]; lo=m+1
        else: hi=m-1
    if best and best[0]<=va<best[1]: return best
    return None
cmd=sys.argv[1]
if cmd=="fn":
    va=int(sys.argv[2],16); f=containing(va)
    print(f"{va:#x} in func {f[0]:#x}..{f[1]:#x} (len {f[1]-f[0]:#x})" if f else "not in any pdata func")
