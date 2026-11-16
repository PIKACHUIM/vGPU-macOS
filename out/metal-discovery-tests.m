// 只测试参数和 CF 属性类型；不运行默认 Metal 验收。
#define main metal_probe_program_main
#include "metal-discovery-probe.m"
#undef main
#include <assert.h>

int main(void) {
    @autoreleasepool {
        char name[255];
        assert(strcmp(discovery_name_gate(NULL, name), "G1_NO_METAL_PLUGIN_NAME") == 0);
        assert(strcmp(discovery_name_gate(kCFBooleanTrue, name), "G1_NAME_NOT_STRING") == 0);
        int number = 7;
        CFNumberRef value = CFNumberCreate(NULL, kCFNumberIntType, &number);
        assert(strcmp(discovery_name_gate(value, name), "G1_NAME_NOT_STRING") == 0);
        CFRelease(value);
        assert(discovery_name_gate(CFSTR("VGPUDeviceDiagnostic"), name) == NULL);
        const CFStringRef invalid[] = {CFSTR(""), CFSTR(".."), CFSTR("../x"), CFSTR("x/y"),
                                      CFSTR("x\\y"), CFSTR("x y"), CFSTR("测试")};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
            assert(strcmp(discovery_name_gate(invalid[i], name), "DIAGNOSTIC_NAME_UNSUPPORTED") == 0);
        UniChar embedded[] = {'a', 0, 'b'};
        CFStringRef nul = CFStringCreateWithCharacters(NULL, embedded, 3);
        assert(strcmp(discovery_name_gate(nul, name), "DIAGNOSTIC_NAME_UNSUPPORTED") == 0);
        CFRelease(nul);
        char longName[256];
        memset(longName, 'a', 255); longName[255] = 0;
        CFStringRef longValue = CFStringCreateWithCString(NULL, longName, kCFStringEncodingUTF8);
        assert(strcmp(discovery_name_gate(longValue, name), "DIAGNOSTIC_NAME_UNSUPPORTED") == 0);
        CFRelease(longValue);
        const char *bad[] = {"probe", "--unknown"};
        const char *extra[] = {"probe", "--discovery-contract-only", "extra"};
        assert(metal_probe_program_main(2, bad) == 64);
        assert(metal_probe_program_main(3, extra) == 64);
        printf("PASS: pure CF name/type checks and rejected arguments; no Metal device API called\n");
        return 0;
    }
}
