from pathlib import Path
import argparse
import hashlib
import json
import os
import random
import re
from fractions import Fraction
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description='提取实际资源方法，使用本地 Zig/Node 执行 CPU 替身验证。')
parser.add_argument('--zig', required=True, help='本地 Zig 可执行文件路径，不下载工具')
parser.add_argument('--node', default='node', help='本地 Node 可执行文件路径')
parser.add_argument('--result', type=Path, help='保存精简结果及精确 argv 命令的 txt 路径')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source_path = repo / 'kexts/framebuffer/vgpuFramebuffer.cpp'
source_bytes = source_path.read_bytes()
source = source_bytes.decode('utf-8').replace('\r\n', '\n')
# 每次使用独立系统临时目录；提取源码、对象、Wasm 和 Zig 缓存均不写入仓库。
work = Path(tempfile.mkdtemp(prefix='vgpu-multi-resource-'))
out = work / 'multi_resource_cpu_test.cpp'
log = [
    '多资源 CPU 验证（本地、无 GPU/虚拟机执行）',
    'source: kexts/framebuffer/vgpuFramebuffer.cpp',
    'source SHA-256 (原始文件字节): ' + hashlib.sha256(source_bytes).hexdigest(),
    '命令采用 JSON argv；每个数组元素为独立参数，不经 shell 解释。',
    'cwd: ' + str(Path.cwd()),
    '入口 argv: ' + json.dumps([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]], ensure_ascii=False),
    'Python: ' + sys.version.split()[0],
    '临时产物: ' + str(work),
]
for script in (Path(__file__).resolve(), Path(__file__).with_name('run_multi_resource_cpu_test.js')):
    log.append(script.name + ' SHA-256: ' + hashlib.sha256(script.read_bytes()).hexdigest())


def run(argv):
    argv = [str(arg) for arg in argv]
    log.append('argv: ' + json.dumps(argv, ensure_ascii=False))
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(work / 'zig-global-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = str(work / 'zig-local-cache')
    result = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8', errors='replace', env=env)
    log.append('exit: ' + str(result.returncode))
    for output in (result.stdout, result.stderr):
        if output.strip():
            log.append(output.strip())
    if result.returncode:
        raise RuntimeError('命令失败，退出码 ' + str(result.returncode))


def save_result():
    text = '\n'.join(log) + '\n'
    print(text)
    if args.result:
        args.result.write_text(text, encoding='utf-8')

def function(signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end]

types = source[source.index('static const UInt32 kDxTemplateRtvId ='):source.index('// kStduPaint overlays')]
methods = [
    'IOReturn vgpuFramebuffer::userDraw(',
    'IOReturn vgpuFramebuffer::userClear(',
    'IOReturn vgpuFramebuffer::userSubmit(',
    'IOReturn vgpuFramebuffer::userReadRow(',
    'VgpuColorResource *vgpuFramebuffer::findColorResource(',
    'bool vgpuFramebuffer::resourceOwnerOpen(',
    'IOReturn vgpuFramebuffer::registerResourceOwner(',
    'void vgpuFramebuffer::revokeResourceOwner(',
    'static void fillResourceInfo(',
    'IOReturn vgpuFramebuffer::resourceCreate(',
    'IOReturn vgpuFramebuffer::resourceRelease(',
    'IOReturn vgpuFramebuffer::resourceInfo(',
    'IOReturn vgpuFramebuffer::resourceRead(',
    'static UInt32 resourceClearUnorm8Bits(',
    'IOReturn vgpuFramebuffer::resourceSubmit(',
    'bool vgpuFramebuffer::clearColorResource(',
    'void vgpuFramebuffer::releaseUnpublishedColors(',
]
prefix = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
using UInt8 = uint8_t; using UInt32 = uint32_t; using UInt64 = uint64_t;
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady, kIOReturnNotOpen,
       kIOReturnNoResources, kIOReturnIOError, kIOReturnUnsupported, kIOReturnNotFound,
       kIOReturnNotPermitted, kIOReturnNoMemory };
const int kIOMemoryIncoherentIOFlush = 1, kIODirectionInOut = 3;
const UInt32 kSvga3dCmdDxSetRenderTargets=1161, kSvga3dCmdDxClearRenderTargetView=1176,
    kSvga3dCmdDxReadbackSubResource=1183, kSvga3dCmdDxSetRasterizerState=1174,
    SVGA3D_INVALID_ID_U32=0xffffffff, kCbFlagDxContext=2;
#define VGPU_FB_TAG "test"
#define IOLog(...) ((void)0)
#define bzero(p,n) memset(p,0,n)
static int allocations=0;
void *IOMalloc(size_t n) { ++allocations; return malloc(n); }
void IOFree(void *p,size_t) { --allocations; free(p); }
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(l && !l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l && l->held); l->held=false; }
struct IOMemoryMap { int releases=0; void release() { ++releases; } };
struct IOBufferMemoryDescriptor {
    void *bytes=nullptr; int operations=0, failAt=0, releases=0, completes=0;
    IOReturn performOperation(int, int, int) { return ++operations == failAt ? kIOReturnIOError : kIOReturnSuccess; }
    void *getBytesNoCopy() { return bytes; }
    void release() { ++releases; }
    void complete(int) { ++completes; }
};
'''
cls = r'''
class vgpuFramebuffer {
public:
    Lock lock; Lock *_userDrawLock=&lock;
    VgpuResourceOwner *_resourceOwners=nullptr;
    VgpuColorResource _colorResources[2];
    UInt64 _nextOwnerToken=0, _nextResourceHandle=0, _userDrawGeneration=1;
    bool _resourceBackendReady=true, _resourceBackendFailed=false, _userDrawReady=true,
         _userDrawStopping=false, vertexSetupOk=true;
    UInt32 _dxBusy=0, fifo=1; UInt32 *_dxFifo=&fifo;
    void *_dxVirt=nullptr;
    IOBufferMemoryDescriptor data[2], pt[2], vertex;
    IOMemoryMap maps[4];
    IOBufferMemoryDescriptor *_vertexMem[1]={&vertex};
    UInt32 *gpu[3]={}, target=1;
    int submits=0, failAt=0, vertexSetups=0;
    bool suppressReadback=false;
    UInt32 commands[256]={}, lastClear[4]={};
    vgpuFramebuffer() {
        _dxVirt=calloc(65536,4); vertex.bytes=calloc(12,4);
        for (int i=0;i<3;++i) gpu[i]=(UInt32 *)calloc(65536,4);
        for (int i=0;i<2;++i) {
            auto &r=_colorResources[i]; r.data=&data[i]; r.pt=&pt[i];
            r.dataMap=&maps[i*2]; r.ptMap=&maps[i*2+1];
            r.virt=calloc(65536,4); data[i].bytes=r.virt;
            r.deviceVisible=true; r.dataPrepared=true; r.ptPrepared=true;
        }
    }
    ~vgpuFramebuffer() {
        while (_resourceOwners) revokeResourceOwner(_resourceOwners->token);
        for (int i=0;i<2;++i) free(data[i].bytes);
        for (auto p:gpu) free(p);
        free(vertex.bytes); free(_dxVirt);
    }
    bool setupVertexInput() { ++vertexSetups; return vertexSetupOk; }
    bool cbSubmitCtx(const UInt32 *c,UInt32 n,UInt32 queue,UInt32 cid,UInt32 flags) {
        assert(lock.held); assert(submits < 256);
        commands[submits++]=n?c[0]:0;
        if (submits==failAt) return false;
        if (!n) { assert(queue==0 && cid==0 && flags==1); return true; }
        assert(queue==1 && cid==1 && flags==2);
        if(c[0]==1161) { assert(n==4 && c[2]==0xffffffff); target=c[3]; }
        if(c[0]==1176) {
            memcpy(lastClear,c+3,sizeof(lastClear));
            int slot=c[2]==1?2:(int)c[2]-16; assert(slot>=0 && slot<3);
            // 仅模拟端点颜色；不模拟 GPU 浮点量化或光栅化。
            UInt32 color=(c[3]==0x3f800000?0xff0000:0) | (c[4]==0x3f800000?0xff00:0) |
                (c[5]==0x3f800000?0xff:0) | (c[6]==0x3f800000?0xff000000:0);
            for(int i=0;i<65536;++i) gpu[slot][i]=color;
        }
        if(c[0]==1152) {
            int slot=target==1?2:(int)target-16; assert(slot>=0 && slot<3);
            gpu[slot][0]=0xffff0000;
        }
        if(c[0]==1183 && !suppressReadback) {
            int slot=c[2]==3?2:(int)c[2]-16; assert(slot>=0 && slot<3);
            if(slot<2) {
                // READBACK 前已写判别值，并已完成一次 flush。
                assert(data[slot].operations%2==1);
                assert(((UInt32 *)_colorResources[slot].virt)[0]==0x7e5aa5c3);
            }
            memcpy(slot==2?_dxVirt:_colorResources[slot].virt,gpu[slot],65536*4);
        }
        return true;
    }
    bool resourceOwnerOpen(UInt64);
    IOReturn registerResourceOwner(UInt64 *,const UInt32 *);
    void revokeResourceOwner(UInt64);
    VgpuColorResource *findColorResource(UInt64,UInt64);
    IOReturn userDraw(UInt64,UInt64 *,const UInt32 * = nullptr);
    IOReturn userClear(UInt64,UInt64 *,const UInt32 *);
    IOReturn userSubmit(UInt64,UInt64 *,const UInt32 *,const UInt32 *);
    IOReturn userReadRow(UInt64,UInt64,UInt64,void *);
    IOReturn resourceCreate(UInt64,const VgpuResourceRequest *,VgpuResourceInfo *);
    IOReturn resourceRelease(UInt64,const VgpuResourceReleaseRequest *,VgpuResourceReleaseResponse *);
    IOReturn resourceInfo(UInt64,const VgpuResourceReleaseRequest *,VgpuResourceInfo *);
    IOReturn resourceRead(UInt64,const VgpuResourceReadRequest *,VgpuResourceReadResponse *);
    IOReturn resourceSubmit(UInt64,const VgpuResourceSubmitRequest *,VgpuResourceSubmitResponse *);
    bool clearColorResource(UInt32);
    void releaseUnpublishedColors();
};
'''
tests = r'''
static int checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static VgpuResourceRequest createReq={1,32,256,256,141,0,0,0};
struct Fixture {
    // closing 必须比 fb 活得更久，模拟 client 在注销完成前保持原子标志存活。
    UInt32 closing=0, closing2=0; vgpuFramebuffer fb; UInt64 owner=0, owner2=0;
    Fixture() { CHECK(fb.registerResourceOwner(&owner,&closing)==0); CHECK(fb.registerResourceOwner(&owner2,&closing2)==0); CHECK(owner && owner2>owner); }
    VgpuResourceInfo create(UInt64 o=0) { VgpuResourceInfo i={}; CHECK(fb.resourceCreate(o?o:owner,&createReq,&i)==0); CHECK(i.version==1 && i.byteSize==64 && i.handle && i.generation==1 && i.format==141 && i.flags==7 && i.reserved[0]==0 && i.reserved[1]==0); return i; }
};
static VgpuResourceSubmitRequest submitReq(VgpuResourceInfo i,UInt32 opcode=1) {
    VgpuResourceSubmitRequest r={}; r.version=1; r.byteSize=96; r.handle=i.handle; r.expectedGeneration=i.generation; r.opcode=opcode;
    if(opcode==3) for(int v=0;v<3;++v) r.verticesBits[v*4+3]=0x3f800000;
    return r;
}
static void lifecycle() {
    Fixture f; auto a=f.create(), b=f.create(); auto &fb=f.fb;
    CHECK(a.handle!=b.handle && fb._colorResources[0].virt!=fb._colorResources[1].virt);
    VgpuResourceInfo info={}; int before=fb.submits;
    CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnNoResources && fb.submits==before);
    auto q=submitReq(a); q.rgbaBits[0]=0x3f800000; VgpuResourceSubmitResponse s={};
    CHECK(fb.resourceSubmit(f.owner,&q,&s)==0 && s.generation==2);
    VgpuResourceReadResponse row={}; VgpuResourceReadRequest read={1,32,a.handle,2,0,0};
    CHECK(fb.resourceRead(f.owner,&read,&row)==0 && ((UInt32 *)row.pixels)[0]==0x00ff0000);
    q=submitReq(b); q.rgbaBits[1]=q.rgbaBits[3]=0x3f800000;
    CHECK(fb.resourceSubmit(f.owner,&q,&s)==0 && s.generation==2);
    CHECK(fb.resourceRead(f.owner,&read,&row)==0 && ((UInt32 *)row.pixels)[0]==0x00ff0000);
    read.handle=b.handle; CHECK(fb.resourceRead(f.owner,&read,&row)==0 && ((UInt32 *)row.pixels)[0]==0xff00ff00);
    read.generation=1; before=fb.submits;
    CHECK(fb.resourceRead(f.owner,&read,&row)==kIOReturnNotReady);
    CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnNotReady && fb.submits==before);
    read.generation=2; q.expectedGeneration=2;
    CHECK(fb.resourceRead(f.owner2,&read,&row)==kIOReturnNotPermitted);
    CHECK(fb.resourceSubmit(f.owner2,&q,&s)==kIOReturnNotPermitted);
    VgpuResourceReleaseRequest rel={1,24,a.handle,0}; VgpuResourceReleaseResponse released={};
    CHECK(fb.resourceInfo(f.owner2,&rel,&info)==kIOReturnNotPermitted);
    CHECK(fb.resourceRelease(f.owner2,&rel,&released)==kIOReturnNotPermitted);
    CHECK(fb.resourceRelease(f.owner,&rel,&released)==0 && released.flags==1 && released.reserved==0);
    CHECK(fb.resourceRelease(f.owner,&rel,&released)==kIOReturnNotFound);
    CHECK(fb.resourceInfo(f.owner,&rel,&info)==kIOReturnNotFound);
    auto c=f.create(f.owner2); CHECK(c.handle>b.handle);
    read.handle=c.handle; read.generation=1;
    for(UInt32 r=0;r<256;++r) { read.row=r; CHECK(fb.resourceRead(f.owner2,&read,&row)==0); for(UInt32 p=0;p<1024;++p) CHECK(row.pixels[p]==0); }
    fb.revokeResourceOwner(f.owner2); CHECK(fb.resourceRead(f.owner2,&read,&row)==kIOReturnNotOpen);
    auto d=f.create(); CHECK(d.handle>c.handle);
    __atomic_store_n(&f.closing,1u,__ATOMIC_RELEASE); before=fb.submits;
    CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnNotOpen && fb.submits==before);
    UInt64 gen=0; CHECK(fb.userDraw(f.owner,&gen)==kIOReturnNotOpen && fb.submits==before);
    CHECK(fb.userReadRow(f.owner,1,0,row.pixels)==kIOReturnNotOpen);
}
static void validation() {
    Fixture f; auto a=f.create(); auto &fb=f.fb; auto q=submitReq(a); VgpuResourceSubmitResponse s={}; VgpuResourceInfo info={};
    int before=fb.submits;
    UInt32 invalid[]={0x7f800000,0x7fc00001,0xbf000000,0x3f800001,0xff800000};
    for(auto bits:invalid) { q.rgbaBits[0]=bits; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument); }
    q=submitReq(a); q.version=2; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a); q.byteSize=95; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a); q.flags=1; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a); q.handle=0; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a); q.expectedGeneration=0; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a); q.opcode=4; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnUnsupported);
    q=submitReq(a); q.verticesBits[0]=1; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a,3); q.verticesBits[3]=0; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    q=submitReq(a,3); q.verticesBits[2]=0xbf800000; CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnBadArgument);
    auto c=createReq; c.reserved=1; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnBadArgument);
    c=createReq; c.flags=1; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnBadArgument);
    c=createReq; c.version=2; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnBadArgument);
    c=createReq; c.byteSize=31; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnBadArgument);
    c=createReq; c.width=255; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnUnsupported);
    c=createReq; c.format=142; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnUnsupported);
    c=createReq; c.samples=1; CHECK(fb.resourceCreate(f.owner,&c,&info)==kIOReturnUnsupported);
    VgpuResourceReadResponse row={}; VgpuResourceReadRequest r={1,32,a.handle,1,256,0};
    CHECK(fb.resourceRead(f.owner,&r,&row)==kIOReturnBadArgument); r.row=0; r.generation=0;
    CHECK(fb.resourceRead(f.owner,&r,&row)==kIOReturnBadArgument);
    CHECK(fb.submits==before && !fb._resourceBackendFailed);
    q=submitReq(a); q.rgbaBits[0]=0x80000000; CHECK(fb.resourceSubmit(f.owner,&q,&s)==0);
    fb._colorResources[0].generation=UINT64_MAX; q.expectedGeneration=UINT64_MAX; before=fb.submits;
    CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnNoResources && fb.submits==before);
    fb._nextResourceHandle=UINT64_MAX; CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnNoResources);
    fb._nextOwnerToken=UINT64_MAX; UInt64 token=0; CHECK(fb.registerResourceOwner(&token,&f.closing)==kIOReturnNoResources);
}
static void failedState(Fixture &f,VgpuResourceInfo a) {
    auto &fb=f.fb; CHECK(fb._resourceBackendFailed && !fb._resourceBackendReady && !fb._userDrawReady && !fb._dxBusy);
    CHECK(fb._colorResources[0].generation==a.generation);
    auto q=submitReq(a); VgpuResourceSubmitResponse s={}; VgpuResourceInfo info={};
    VgpuResourceReadResponse row={}; VgpuResourceReadRequest r={1,32,a.handle,a.generation,0,0};
    VgpuResourceReleaseRequest rel={1,24,a.handle,0}; VgpuResourceReleaseResponse out={};
    int before=fb.submits; UInt64 gen=0;
    CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnNotReady);
    CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnNotReady);
    CHECK(fb.resourceRead(f.owner,&r,&row)==kIOReturnNotReady);
    CHECK(fb.resourceInfo(f.owner,&rel,&info)==kIOReturnNotReady);
    CHECK(fb.userDraw(f.owner,&gen)==kIOReturnNotReady);
    CHECK(fb.userReadRow(f.owner,1,0,row.pixels)==kIOReturnNotReady);
    CHECK(fb.resourceRelease(f.owner,&rel,&out)==0);
    CHECK(fb.submits==before);
    fb.releaseUnpublishedColors();
    for(int i=0;i<2;++i) CHECK(fb.data[i].releases==0 && fb.pt[i].releases==0 && fb.data[i].completes==0 && fb.maps[2*i].releases==0);
}
static void failures() {
    for(UInt32 op=1;op<=3;++op) {
        int stages=op==1?4:(op==2?9:10);
        for(int fail=1;fail<=stages;++fail) {
            Fixture f; auto a=f.create(); auto q=submitReq(a,op); auto &fb=f.fb; VgpuResourceSubmitResponse s; memset(&s,0xcc,sizeof(s));
            int before=fb.submits; fb.failAt=before+fail;
            CHECK(fb.resourceSubmit(f.owner,&q,&s)==kIOReturnIOError);
            CHECK(fb.submits==before+fail && s.version==0xcccccccc); failedState(f,a);
        }
    }
    for(int fail=1;fail<=4;++fail) {
        Fixture f; auto &fb=f.fb; fb.failAt=fail; VgpuResourceInfo info={};
        CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnIOError);
        CHECK(fb.submits==fail && fb._nextResourceHandle==0 && fb._colorResources[0].owner==0 && fb._resourceBackendFailed);
        int before=fb.submits; CHECK(fb.resourceCreate(f.owner,&createReq,&info)==kIOReturnNotReady && fb.submits==before);
    }
    for(int phase=1;phase<=2;++phase) {
        Fixture f; auto a=f.create(); auto q=submitReq(a); VgpuResourceSubmitResponse s={};
        f.fb.data[0].failAt=f.fb.data[0].operations+phase;
        CHECK(f.fb.resourceSubmit(f.owner,&q,&s)==kIOReturnIOError); failedState(f,a);
    }
    { Fixture f; auto a=f.create(); f.fb.vertexSetupOk=false; auto q=submitReq(a,3); VgpuResourceSubmitResponse s={}; int before=f.fb.submits;
      CHECK(f.fb.resourceSubmit(f.owner,&q,&s)==kIOReturnIOError && f.fb.submits==before); failedState(f,a); }
    { Fixture f; f.fb.suppressReadback=true; VgpuResourceInfo i={}; CHECK(f.fb.resourceCreate(f.owner,&createReq,&i)==kIOReturnIOError && f.fb._nextResourceHandle==0 && f.fb._resourceBackendFailed); }
    { Fixture f; auto a=f.create(); UInt64 gen=0; f.fb.failAt=f.fb.submits+1; CHECK(f.fb.userDraw(f.owner,&gen)==kIOReturnIOError && gen==0); failedState(f,a); }
}
static void routingAndCleanup() {
    Fixture f; auto a=f.create(),b=f.create(); auto &fb=f.fb; VgpuResourceSubmitResponse s={};
    auto q=submitReq(b,2); CHECK(fb.resourceSubmit(f.owner,&q,&s)==0 && fb.target==17);
    UInt64 gen=0; CHECK(fb.userDraw(f.owner,&gen)==0 && fb.target==1 && gen==2);
    CHECK(fb._colorResources[0].generation==1 && fb._colorResources[1].generation==2);
    q=submitReq(a,3); CHECK(fb.resourceSubmit(f.owner,&q,&s)==0 && fb.target==16 && fb._userDrawGeneration==2);
    CHECK(fb._colorResources[1].generation==2);
    fb._colorResources[1].deviceVisible=false; fb.releaseUnpublishedColors();
    CHECK(fb.data[1].releases==1 && fb.pt[1].releases==1 && fb.data[1].completes==1 && fb.pt[1].completes==1);
    CHECK(fb.maps[2].releases==1 && fb.maps[3].releases==1 && fb._colorResources[1].virt==nullptr);
    CHECK(fb.data[0].releases==0 && fb.pt[0].releases==0);
    fb.releaseUnpublishedColors(); CHECK(fb.data[1].releases==1 && fb.data[1].completes==1);
}
static void clearCommandBits() {
    // 检查提交内容，不把 CPU 端点颜色替身当成 GPU 量化 oracle。
    const UInt32 input[4]={0x3f000000,0x3e800000,0x3f400000,0x80000000};
    const UInt32 expected[4]={0x3f008081,0x3e808081,0x3f3fbfc0,0};
    for(UInt32 op=1;op<=3;++op) {
        UInt32 closing=0; vgpuFramebuffer fb; UInt64 owner=0;
        assert(fb.registerResourceOwner(&owner,&closing)==0);
        VgpuResourceInfo resources[2]={}; VgpuResourceSubmitResponse s={};
        for(auto &resource:resources) {
            assert(fb.resourceCreate(owner,&createReq,&resource)==0);
            auto q=submitReq(resource,op); memcpy(q.rgbaBits,input,sizeof(input));
            assert(fb.resourceSubmit(owner,&q,&s)==0);
            for(int i=0;i<4;++i) assert(fb.lastClear[i]==expected[i]);
        }
        UInt64 gen=0;
        assert(fb.userClear(owner,&gen,input)==0);
        for(int i=0;i<4;++i) assert(fb.lastClear[i]==input[i]);
    }
}
int main() { lifecycle(); validation(); failures(); routingAndCleanup(); CHECK(allocations==0); printf("PASS: %d assertions; actual extracted methods, CPU mocks only\n",checks); clearCommandBits(); assert(allocations==0); }
'''
out.write_text(prefix + types + cls + '\n\n'.join(function(s) for s in methods) + tests, encoding='utf-8')
print(f'已提取 {len(methods)} 个实际函数及全部 ABI 断言到 {out}')
# 无系统运行库的执行版本；只替换测试环境声明，实际内核方法保持原样。
wasm = out.read_text(encoding='utf-8')
wasm = re.sub(r'#include <[^>]+>\n', '', wasm)
wasm = r'''
using uint8_t=unsigned char; using uint32_t=unsigned int; using uint64_t=unsigned long long;
using size_t=unsigned long;
#define UINT64_MAX (~(uint64_t)0)
extern "C" {
#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
IMPORT(malloc) void *malloc(size_t); IMPORT(calloc) void *calloc(size_t,size_t); IMPORT(free) void free(void *);
IMPORT(memcpy) void *memcpy(void *,const void *,size_t); IMPORT(memset) void *memset(void *,int,size_t);
IMPORT(exit) void exit(int); IMPORT(test_fail) void test_fail(int,const char *); IMPORT(test_pass) void test_pass(int);
}
#define assert(x) do { if(!(x)) test_fail(__LINE__,#x); } while(0)
''' + wasm
wasm = wasm.replace('fprintf(stderr,"FAIL line %d: %s\\n",__LINE__,#x); exit(1);', 'test_fail(__LINE__,#x);')
wasm = wasm.replace('printf("PASS: %d assertions; actual extracted methods, CPU mocks only\\n",checks);', 'test_pass(checks);')
wasm = wasm.replace('int main() {', 'extern "C" void run_tests() {')
wasm_source = work / 'multi_resource_cpu_test_wasm.cpp'
wasm_source.write_text(wasm, encoding='utf-8')
mac = wasm[:wasm.index('static int checks=0;')]
mac = mac.replace('#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))', '#define IMPORT(name)')
mac_source = work / 'multi_resource_methods_macos.cpp'
mac_source.write_text(mac, encoding='utf-8')
log.extend([
    '提取方法: ' + str(len(methods)) + '；ABI static_assert: ' + str(types.count('static_assert(')),
    '提取函数及类型 SHA-256 (UTF-8/LF): ' + hashlib.sha256((types + '\n\n'.join(function(s) for s in methods)).encode('utf-8')).hexdigest(),
    '环境覆盖: ZIG_GLOBAL_CACHE_DIR=' + str(work / 'zig-global-cache'),
    '环境覆盖: ZIG_LOCAL_CACHE_DIR=' + str(work / 'zig-local-cache'),
    '断言边界: 263020 次 CHECK 包含 256*1024=262144 次清零字节检查、256 次读行成功检查，以及 620 次其他 CHECK（含 fixture 检查）。',
    'C++ assert（锁/命令约束）与 ABI 编译期 static_assert 不计入 263020；JS 另检验成功回调及分配全部回收。',
    '覆盖: 双资源隔离、owner/handle/generation、参数拒绝、提交/flush/回读失败关闭、目标路由及未发布资源清理。',
    '限制: CPU 替身只模拟端点颜色和单像素 draw，不验证 GPU 光栅化、浮点量化、真实 IOKit/DMA/fence、并发时序或完整 kext 链接加载。',
    'macOS 对象编译仅验证提取方法及 ABI，不代表完整驱动构建。',
])
# oracle 使用 Fraction 和单调 float32 位模式的二分搜索，不复用内核的移位/除法算法。
def exact_float(bits):
    bits &= 0x7fffffff
    exponent, mantissa = bits >> 23, bits & 0x7fffff
    if exponent == 0:
        return Fraction(mantissa, 1 << 149)
    return Fraction((1 << 23) + mantissa, 1 << (150 - exponent))


def float_floor(value):
    lo, hi = 0, 0x3f800000
    while lo < hi:
        middle = (lo + hi + 1) // 2
        if exact_float(middle) <= value:
            lo = middle
        else:
            hi = middle - 1
    return lo


def nearest_float(value):
    lower = float_floor(value)
    if exact_float(lower) == value:
        return lower
    upper = lower + 1
    return min((lower, upper), key=lambda b: (abs(exact_float(b) - value), b & 1))


canonical = [nearest_float(Fraction(q, 255)) for q in range(256)]
cases = {0, 0x80000000, 1, 2, 0x7ffffe, 0x7fffff, 0x800000, 0x800001,
         0x3f7ffffe, 0x3f7fffff, 0x3f800000}
for bits in canonical:
    cases.update(b for b in (bits - 1, bits, bits + 1) if 0 <= b <= 0x3f800000)
for q in range(255):
    boundary = Fraction(2 * q + 1, 510)
    lower = float_floor(boundary)
    # 精确可表示的 half（0.5）也包含其两侧；其他 half 包含夹住它的两个 float。
    cases.update((lower - 1, lower, lower + 1))
rng = random.Random(0xB6A8)
cases.update(rng.randrange(0x3f800001) for _ in range(50000))
cases.update(rng.randrange(0x3b000000, 0x3f800001) for _ in range(10000))
cases.update(rng.randrange(1, 0x800000) for _ in range(1000))
quantization_cases = [[bits, canonical[round(exact_float(bits) * 255)]] for bits in sorted(cases)]
case_path = work / 'clear_quantization_cases.json'
case_path.write_text(json.dumps(quantization_cases), encoding='utf-8')
helper_source = work / 'clear_quantization.cpp'
helper_source.write_text('using UInt32=unsigned int; using UInt64=unsigned long long;\n' +
                         function('static UInt32 resourceClearUnorm8Bits(') +
                         '\nextern "C" UInt32 quantize(UInt32 b) { return resourceClearUnorm8Bits(b); }\n',
                         encoding='utf-8')
helper_runner = r'''
const fs=require('fs');
(async()=>{
    const {instance}=await WebAssembly.instantiate(fs.readFileSync(process.argv[1]));
    const cases=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
    for(const [bits,expected] of cases) {
        const actual=instance.exports.quantize(bits)>>>0;
        if(actual!==expected) throw new Error(`input=${bits.toString(16)} actual=${actual.toString(16)} expected=${expected.toString(16)}`);
    }
    console.log(`Fraction oracle: PASS ${cases.length} 个输入；提取的实际整数 helper。`);
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
log.extend([
    '量化覆盖: 256 个 q/255 格点及邻居、255 个精确 half 边界的相邻 float、+/-0、subnormal、0/1 邻域。',
    '固定随机种子 0xB6A8：50000 个全域合法 bits、10000 个高值域 bits、1000 个 subnormal；去重后共 ' + str(len(cases)) + ' 个输入。',
    '量化期望: Fraction 精确解码、round 的 ties-even、二分搜索最邻近 float32；不使用宿主浮点运算。',
    '新增命令断言: 两个资源的三个 opcode 均提交 canonical bits，旧 userClear 仍透传原始 bits；不改变原有 CHECK 计数。',
])
try:
    run([args.zig, 'version'])
    run([args.node, '--version'])
    for optimization in ('-O0', '-O2'):
        helper_wasm = work / ('clear_quantization' + optimization + '.wasm')
        run([args.zig, 'c++', '-target', 'wasm32-freestanding', '-std=c++17', optimization,
             '-fno-exceptions', '-fno-rtti', '-fsanitize-trap=all', '-nostdlib', '--rtlib=none', helper_source,
             '-Wl,--no-entry', '-Wl,--export=quantize', '-o', helper_wasm])
        run([args.node, '-e', helper_runner, helper_wasm, case_path])
    run([args.zig, 'c++', '-target', 'x86_64-macos-none', '-std=c++17', '-O2',
         '-fno-exceptions', '-fno-rtti', '-nostdinc', '-mkernel', '-fapple-kext',
         '-mgeneral-regs-only', '-fno-sanitize=all', '-Werror', '-S', helper_source,
         '-o', work / 'clear_quantization_macos.s'])
    run([args.zig, 'c++', '-target', 'wasm32-freestanding', '-std=c++17', '-O0',
         '-fno-exceptions', '-fno-rtti', '-fsanitize-trap=all', '-nostdlib', '--rtlib=none', wasm_source,
         '-Wl,--no-entry', '-Wl,--export=run_tests', '-Wl,--export=__heap_base',
         '-Wl,--initial-memory=134217728', '-o', work / 'multi_resource_cpu_test.wasm'])
    run([args.node, Path(__file__).with_name('run_multi_resource_cpu_test.js').resolve(),
         work / 'multi_resource_cpu_test.wasm'])
    run([args.zig, 'c++', '-target', 'x86_64-macos-none', '-std=c++17', '-O0',
         '-fno-exceptions', '-fno-rtti', '-nostdinc', '-mkernel', '-fapple-kext',
         '-DKERNEL', '-fno-sanitize=all', '-Wframe-larger-than=1024', '-Werror', '-c', mac_source,
         '-o', work / 'multi_resource_methods_macos.o'])
    if source_path.read_bytes() != source_bytes:
        raise RuntimeError('验证期间源文件发生变化，请重跑')
except (OSError, RuntimeError) as error:
    log.append('结果: FAIL — ' + str(error))
    save_result()
    sys.exit(1)
log.append('结果: PASS；CPU 运行、分配回收及 macOS 提取方法对象编译通过。')
save_result()
