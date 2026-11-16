import pathlib
import subprocess

metal = '/System/Library/Frameworks/Metal.framework/Versions/A/Metal'
print('== UUID / exports ==')
for flag in ('-uuid', '-exports'):
    p = subprocess.run(['xcrun', 'dyld_info', flag, metal], text=True, capture_output=True, check=True)
    for line in p.stdout.splitlines():
        if flag == '-uuid' or 'OBJC_CLASS_$__MTLDevice' in line or 'OBJC_METACLASS_$__MTLDevice' in line:
            print(line)
print('== Objective-C metadata ==')
p = subprocess.run(['xcrun', 'dyld_info', '-objc', metal], text=True, capture_output=True, check=True)
lines = p.stdout.splitlines()
for i, line in enumerate(lines):
    if '_MTLDevice' in line:
        print('metadata_line=', i+1)
        print('\n'.join(lines[max(0, i-2):i+12]))
print('== selected implementation bodies ==')
lines = pathlib.Path('/var/tmp/vgpu-fb/metal-code.txt').read_text().splitlines()
targets = ('-[_MTLDevice init]:', '-[_MTLDevice initWithAcceleratorPort:]:', '-[_MTLDevice dealloc]:', '-[_MTLDevice .cxx_construct]:', '-[_MTLDevice .cxx_destruct]:', '+[_MTLDevice initialize]:', '+[_MTLDevice allocWithZone]:')
for i, line in enumerate(lines):
    if any(t in line for t in targets):
        print('source_line=', i+1)
        for item in lines[i:i+1500]:
            if item != line and item and not item[0].isspace() and item.endswith(':'):
                break
            print(item)
print('== SDK link stub declarations ==')
sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
p = pathlib.Path(sdk) / 'System/Library/Frameworks/Metal.framework/Versions/A/Metal.tbd'
if p.exists():
    for i, line in enumerate(p.read_text().splitlines()):
        if '_MTLDevice' in line:
            print(str(p), i+1, line)
