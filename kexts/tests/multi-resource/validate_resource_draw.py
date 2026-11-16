"""提取真实资源方法；复用既有 CPU fixture，新增 draw-only 回归。无 GPU 执行。"""
import argparse
import ast
import hashlib
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--zig', type=Path)
parser.add_argument('--emit', type=Path, help='另存可由真实 SDK 编译的 CPU 测试源码')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source_bytes = (repo / 'kexts/framebuffer/vgpuFramebuffer.cpp').read_bytes()
source = source_bytes.decode('utf-8').replace('\r\n', '\n')
base = ast.parse(Path(__file__).with_name('validate_multi_resource.py').read_text(encoding='utf-8'))
values = {}
for node in base.body:
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        if node.targets[0].id in ('prefix', 'cls', 'tests', 'methods'):
            values[node.targets[0].id] = ast.literal_eval(node.value)

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

types = source[source.index('static const UInt32 kDxTemplateRtvId ='):source.index('// kStduPaint overlays')]
prefix = values['prefix'].replace('kSvga3dCmdDxSetRasterizerState=1174',
    'kSvga3dCmdDxSetRasterizerState=1164, kSvga3dCmdDxSetTopology=1160, '
    'kSvga3dCmdDxSetDepthStencilState=1163, kSvga3dCmdDxSetViewports=1174')
cls = values['cls'].replace('UInt32 commands[256]={}', 'UInt32 payload[256][10]={}, lengths[256]={}; UInt32 commands[256]={}')
cls = cls.replace('commands[submits++]=n?c[0]:0;',
    'assert(n<=10); lengths[submits]=n; if(n) { assert(c[1]==(n-2)*4); memcpy(payload[submits],c,n*4); } commands[submits++]=n?c[0]:0;')
tests = values['tests'].replace('q.opcode=4;', 'q.opcode=5;')
tests = tests.replace('if(opcode==3)', 'if(opcode==3 || opcode==4)')
tests = tests.replace('op<=3;++op) {\n        int stages=op==1?4:(op==2?9:10);',
    'op<=4;++op) {\n        int stages=op==1?4:(op==2?9:(op==3?10:14));')
extra = r'''
static void drawOnly() {
    CHECK(kResourceMobIds[0]==32 && kResourceMobIds[1]==33);
    Fixture f; auto a=f.create(), b=f.create(); auto &fb=f.fb;
    VgpuResourceSubmitResponse out={};
    const UInt32 sequence[]={1219,1161,1150,1160,1162,1163,1174,1150,1157,1158,1164,1152,1183,0};
    for(int slot=0;slot<2;++slot) {
        auto info=slot?b:a;
        for(int i=0;i<65536;++i) fb.gpu[slot][i]=0x80402010+slot;
        auto q=submitReq(info,4);
        q.verticesBits[0]=0xbf400000; q.verticesBits[5]=0x3f000000;
        int first=fb.submits;
        CHECK(fb.resourceSubmit(f.owner,&q,&out)==0 && out.generation==2 && out.handle==info.handle);
        CHECK(fb.submits-first==14 && fb.target==16u+slot);
        for(int j=0;j<14;++j) CHECK(fb.commands[first+j]==sequence[j]);
        CHECK(fb.payload[first+1][2]==0xffffffff && fb.payload[first+1][3]==16u+slot);
        CHECK(fb.payload[first+2][2]==1 && fb.payload[first+2][3]==2);
        CHECK(fb.payload[first+3][2]==1);
        CHECK(fb.payload[first+4][2]==1 && fb.payload[first+4][7]==0xffffffff);
        CHECK(fb.payload[first+5][2]==1 && fb.payload[first+5][3]==0);
        CHECK(fb.payload[first+6][2]==0 && fb.payload[first+6][3]==0 && fb.payload[first+6][4]==0);
        CHECK(fb.payload[first+6][5]==0x43800000 && fb.payload[first+6][6]==0x43800000 && fb.payload[first+6][7]==0 && fb.payload[first+6][8]==0x3f800000);
        CHECK(fb.payload[first+7][2]==3 && fb.payload[first+7][3]==1);
        CHECK(fb.payload[first+8][2]==2 && fb.payload[first+9][3]==4 && fb.payload[first+9][4]==16);
        CHECK(fb.payload[first+10][2]==1 && fb.payload[first+11][2]==3 && fb.payload[first+11][3]==0);
        CHECK(fb.payload[first+12][2]==16u+slot && fb.payload[first+12][3]==0);
        for(int j=0;j<12;++j) CHECK(((UInt32 *)fb.vertex.bytes)[j]==q.verticesBits[j]);
        CHECK(((UInt32 *)fb.data[slot].bytes)[0]==0xffff0000);
        for(int i=1;i<65536;++i) CHECK(((UInt32 *)fb.data[slot].bytes)[i]==0x80402010u+slot);
        CHECK(fb._colorResources[1-slot].generation==(slot?2u:1u) && fb._userDrawGeneration==1);
        CHECK(fb.gpu[2][0]==0 && fb.gpu[2][65535]==0);
        int before=fb.submits;
        CHECK(fb.resourceSubmit(f.owner,&q,&out)==kIOReturnNotReady && fb.submits==before);
        q.expectedGeneration=2;
        CHECK(fb.resourceSubmit(f.owner2,&q,&out)==kIOReturnNotPermitted && fb.submits==before);
        VgpuResourceReadRequest read={1,32,info.handle,1,0,0}; VgpuResourceReadResponse row={};
        CHECK(fb.resourceRead(f.owner,&read,&row)==kIOReturnNotReady);
        read.generation=2; CHECK(fb.resourceRead(f.owner,&read,&row)==0 && ((UInt32 *)row.pixels)[1]==0x80402010u+slot);
    }
    auto q=submitReq(a,4); q.expectedGeneration=2;
    int before=fb.submits;
    for(int i=0;i<4;++i) {
        for(UInt32 bad : {1u,0x80000000u,0x3f800000u,0x7fc00000u}) {
            auto badq=q; badq.rgbaBits[i]=bad;
            CHECK(fb.resourceSubmit(f.owner,&badq,&out)==kIOReturnBadArgument);
        }
    }
    for(int i=0;i<12;++i) {
        for(UInt32 bad : {0x7f800000u,0xff800000u,0x7fc00000u,0x3f800001u}) {
            auto badq=q; badq.verticesBits[i]=bad;
            CHECK(fb.resourceSubmit(f.owner,&badq,&out)==kIOReturnBadArgument);
        }
    }
    for(int v=0;v<3;++v) {
        auto badq=q; badq.verticesBits[4*v+2]=0x80000001;
        CHECK(fb.resourceSubmit(f.owner,&badq,&out)==kIOReturnBadArgument);
        badq=q; badq.verticesBits[4*v+3]=0;
        CHECK(fb.resourceSubmit(f.owner,&badq,&out)==kIOReturnBadArgument);
    }
    CHECK(fb.submits==before && !fb._resourceBackendFailed && !fb.lock.held);
    fb._colorResources[0].generation=UINT64_MAX; q.expectedGeneration=UINT64_MAX;
    CHECK(fb.resourceSubmit(f.owner,&q,&out)==kIOReturnNoResources && fb.submits==before);
    fb._colorResources[0].generation=2; q.expectedGeneration=2;
    fb._dxBusy=1; CHECK(fb.resourceSubmit(f.owner,&q,&out)==kIOReturnNotReady); fb._dxBusy=0;
    fb._userDrawStopping=true; CHECK(fb.resourceSubmit(f.owner,&q,&out)==kIOReturnNotReady); fb._userDrawStopping=false;
    __atomic_store_n(&f.closing,1u,__ATOMIC_RELEASE);
    CHECK(fb.resourceSubmit(f.owner,&q,&out)==kIOReturnNotOpen && fb.submits==before);
    for(int stage=0;stage<3;++stage) {
        Fixture g; auto c=g.create(); auto r=submitReq(c,4); VgpuResourceSubmitResponse result;
        memset(&result,0xcc,sizeof(result));
        if(!stage) g.fb.vertexSetupOk=false;
        else g.fb.data[0].failAt=g.fb.data[0].operations+stage;
        CHECK(g.fb.resourceSubmit(g.owner,&r,&result)==kIOReturnIOError && result.version==0xcccccccc);
        failedState(g,c);
    }
    { Fixture g; auto c=g.create(); auto r=submitReq(c,4); VgpuResourceSubmitResponse result={};
      for(int v=0;v<3;++v) { r.verticesBits[v*4]=0xbf800000; r.verticesBits[v*4+1]=0x3f800000; r.verticesBits[v*4+2]=0x80000000; }
      CHECK(g.fb.resourceSubmit(g.owner,&r,&result)==0); }
}
'''
# 使用数组，保持 freestanding 版本无需 initializer_list 标准库。
extra = extra.replace('for(UInt32 bad : {1u,0x80000000u,0x3f800000u,0x7fc00000u})',
    'UInt32 badColors[]={1u,0x80000000u,0x3f800000u,0x7fc00000u}; for(UInt32 bad : badColors)')
extra = extra.replace('for(UInt32 bad : {0x7f800000u,0xff800000u,0x7fc00000u,0x3f800001u})',
    'UInt32 badVertices[]={0x7f800000u,0xff800000u,0x7fc00000u,0x3f800001u}; for(UInt32 bad : badVertices)')
tests = tests.replace('int main() {', extra + '\nint main() { drawOnly();')
text = prefix + types + cls + '\n\n'.join(function(s) for s in values['methods']) + tests
print('源码 SHA256 ' + hashlib.sha256(source_bytes).hexdigest(), flush=True)
if args.emit:
    assert args.emit.parent.is_dir()
    args.emit.write_text(text, encoding='utf-8')
if args.zig:
    import re
    work = Path(tempfile.mkdtemp(prefix='vgpu-draw4-cpu-'))
    wasm = re.sub(r'#include <[^>]+>\n', '', text)
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
    (work / 'draw.cpp').write_text(wasm, encoding='utf-8')
    runner = Path(__file__).with_name('run_multi_resource_cpu_test.js').read_text(encoding='utf-8')
    runner = runner.replace('n !== 263020', 'n < 394000').replace('预期 263020', '预期至少 394000')
    (work / 'run.js').write_text(runner, encoding='utf-8')
    for level in ('-O0', '-O2'):
        subprocess.run([str(args.zig), 'c++', '-target', 'wasm32-freestanding', '-std=c++17', level,
            '-fno-exceptions', '-fno-rtti', '-fsanitize-trap=all', '-nostdlib', '--rtlib=none',
            str(work / 'draw.cpp'), '-Wl,--no-entry', '-Wl,--export=run_tests', '-Wl,--export=__heap_base',
            '-Wl,--initial-memory=268435456', '-o', str(work / 'draw.wasm')], check=True)
        subprocess.run(['node', str(work / 'run.js'), str(work / 'draw.wasm')], check=True)
    print('CPU 产物 ' + str(work))
