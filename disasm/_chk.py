import io
s=io.open(r'D:/Codes/vGPU-macOS/kexts/framebuffer/vgpuFramebuffer.cpp',encoding='utf-8',errors='replace').read()
i=s.index('COTSZ %u (set %s, q63)')
print(s[i-2600:i+900])
