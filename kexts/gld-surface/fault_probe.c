/* 仅此测试二进制拦截返回元数据；实际提交和读行仍调用原始 IOKit。 */
#include "vgpu_surface.h"
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef kern_return_t (*call_type)(mach_port_t, uint32_t, const uint64_t *,
    uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
static call_type real_call;
static unsigned mode, rows;
static vgpu_surface *other;
static bool injected;

kern_return_t surface_test_call(mach_port_t connection, uint32_t selector,
    const uint64_t *input, uint32_t input_count, const void *structure,
    size_t structure_size, uint64_t *output, uint32_t *output_count,
    void *bytes, size_t *byte_count)
{
    kern_return_t result = real_call(connection,selector,input,input_count,
        structure,structure_size,output,output_count,bytes,byte_count);
    if (result != kIOReturnSuccess) return result;
    if (selector == 3) {
        if (mode == 1) *output_count = 2;
        if (mode == 2) output[1] = 255;
        if (mode == 3) output[2] = 257;
        if (mode == 4) output[0] = 0;
        if (mode == 8) *output_count = 4;
    }
    if (selector == 1) {
        ++rows;
        if (rows == 4 && mode == 5) *byte_count = 1023;
        if (rows == 4 && mode == 6) *byte_count = 1025;
        if (rows == 4 && mode == 7) return kIOReturnIOError;
        if ((rows == 4 && mode == 9) || (rows == 256 && mode == 10)) {
            const float color[4] = {.875f,.125f,.625f,.75f};
            uint64_t generation;
            injected = vgpu_surface_clear(other,color,&generation) == kIOReturnSuccess;
        }
    }
    return result;
}
int main(void)
{
    void *symbol = dlsym(RTLD_DEFAULT,"IOConnectCallMethod");
    _Static_assert(sizeof(real_call)==sizeof(symbol), "function pointer size");
    memcpy(&real_call,&symbol,sizeof(real_call));
    if (!real_call) return 1;
    vgpu_surface *s = NULL;
    if (vgpu_surface_open(256,256,VGPU_SURFACE_BGRX8,&s) ||
        vgpu_surface_open(256,256,VGPU_SURFACE_BGRX8,&other)) return 1;
    const float color[4] = {.25f,.75f,.125f,.375f};
    unsigned failures=0;
    for (mode=1;mode<=8;++mode) {
        if (mode>=5 && mode<=7) continue;
        uint64_t generation=UINT64_MAX;
        IOReturn result=vgpu_surface_clear(s,color,&generation);
        bool ok=result==kIOReturnIOError && generation==UINT64_MAX;
        printf("%s injected clear metadata mode=%u output unchanged\n",ok?"PASS":"FAIL",mode);
        if(!ok) ++failures;
    }
    unsigned char *buffer=malloc(256*256*4+64);
    if (!buffer) return 1;
    for (unsigned test=5;test<=10;++test) {
        if(test==8) continue;
        mode=0;
        uint64_t generation=0;
        if (vgpu_surface_clear(s,color,&generation)) {++failures;break;}
        mode=test;rows=0;injected=false;
        memset(buffer,0xa5,256*256*4+64);
        IOReturn result=vgpu_surface_read(s,generation,
            (vgpu_surface_rect){0,0,256,256},VGPU_SURFACE_RGBA8_OUTPUT,
            (vgpu_surface_pack){0,0,0,0,4},buffer+32,256*256*4);
        bool ok=result==(test>=9?kIOReturnNotReady:kIOReturnIOError);
        for(size_t i=0;i<256*256*4+64;++i) if(buffer[i]!=0xa5) ok=false;
        if(test>=9 && !injected) ok=false;
        printf("%s mode=%u rows=%u return=0x%08x all destination and guards unchanged\n",
               ok?"PASS":"FAIL",test,rows,(unsigned)result);
        if(!ok) ++failures;
    }
    free(buffer);
    vgpu_surface_release(other);
    vgpu_surface_release(s);
    printf("fault probe failures=%u (metadata faults injected; modes 9/10 real GPU interleaving)\n",failures);
    return failures?1:0;
}
