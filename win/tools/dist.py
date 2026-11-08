"""assembles win/out/dist, the runtime folder Blox7.exe carries"""
import os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
ROOT = os.path.dirname(WIN)
OUT = os.path.join(WIN, 'out')
DIST = os.path.join(OUT, 'dist')
sys.path.insert(0, HERE)
import deps  # noqa: E402

LLVM = f'{deps.NDK}/toolchains/llvm/prebuilt/windows-x86_64'
STRIP = f'{LLVM}/bin/llvm-strip.exe'
MB = os.path.join(OUT, 'runtime-build')
BIN = os.path.join(DIST, 'bin')
LIB = os.path.join(DIST, 'lib', 'blox7')
META = os.path.join(DIST, 'share', 'blox7', 'metadata')
CERTS = os.path.join(DIST, 'root', 'etc', 'ssl', 'certs')

shutil.rmtree(DIST, ignore_errors=True)
for d in (BIN, LIB, META, CERTS):
    os.makedirs(d, exist_ok=True)


def put(src, where, name=None, strip=False):
    dst = os.path.join(where, name or os.path.basename(src))
    if strip:
        subprocess.check_call([STRIP, '--strip-debug', '-o', dst, src])
    else:
        shutil.copyfile(src, dst)
    print(f'{os.path.getsize(dst) // 1024:>8} KB  {os.path.relpath(dst, DIST)}')


put(os.path.join(OUT, 'blox7.exe'), BIN)
put(os.path.join(OUT, 'blox7-console.exe'), BIN)
put(os.path.join(OUT, 'libc.so'), BIN)
put(f'{LLVM}/sysroot/usr/lib/x86_64-linux-android/libc++_shared.so', BIN)
put(os.path.join(deps.PREFIX, 'lib', 'libz.so'), BIN)
put(os.path.join(MB, 'blox7'), BIN, 'blox7.so', strip=True)
for name in ('libSDL3.so', 'libSDL3_ttf.so', 'libGLESv2.so.2'):
    put(os.path.join(OUT, name), BIN)
# BLOX7_SDL3_DLL: a build of the same SDL with its pdb next to it, so stall reports can name SDL's frames
put(os.environ.get('BLOX7_SDL3_DLL') or os.path.join(deps.SRC, 'sdl3-win', 'SDL3.dll'), BIN)
put(os.path.join(deps.SRC, 'sdl3ttf-win', 'SDL3_ttf.dll'), BIN)
for name in ('libEGL.so', 'libGLESv2.so', 'libOpenMAXAL.so', 'libOpenSLES.so', 'libandroid.so', 'liblog.so',
             'libmediandk.so', 'libblox7_audio_sdl.so', 'libvulkan.so'):
    if os.path.exists(os.path.join(MB, name)):
        put(os.path.join(MB, name), LIB, strip=True)
        if name in ('libblox7_audio_sdl.so', 'libEGL.so', 'liblog.so'):
            put(os.path.join(MB, name), BIN, strip=True)
put(os.path.join(MB, 'blox7_updater'), LIB, strip=True)
put(os.path.join(OUT, 'blox7_webview_helper.exe'), LIB)
put(os.path.join(OUT, 'WebView2Loader.dll'), LIB)
for name in os.listdir(os.path.join(ROOT, 'config')):
    put(os.path.join(ROOT, 'config', name), META)
put(os.path.join(ROOT, 'exe', 'cacert.pem'), CERTS, 'ca-certificates.crt')
put(f'{deps.NDK}/shader-tools/windows-x86_64/glslc.exe', BIN)
SHADERS = os.path.join(DIST, 'share', 'blox7', 'shaders')
os.makedirs(SHADERS, exist_ok=True)
put(os.path.join(WIN, 'blox7', 'shaders', 'vignette-scanlines.frag'), SHADERS)
for name in ('blox7.png', 'blox7.ico'):
    put(os.path.join(WIN, 'blox7', name), os.path.dirname(SHADERS))


def write_tzdata(path):
    utc = b'TZif' + bytes(16) + struct.pack('>6i', 0, 0, 0, 0, 1, 4) + struct.pack('>ibb', 0, 0, 0) + b'UTC\0'
    zones = sorted(['Etc/GMT', 'Etc/UTC', 'GMT', 'UTC'])
    index_offset = 12 + 12
    data_offset = index_offset + 52 * len(zones)
    index = b''.join(name.encode().ljust(40, b'\0') + struct.pack('>iii', i * len(utc), len(utc), 0)
                     for i, name in enumerate(zones))
    data = utc * len(zones)
    header = b'tzdata2026a\0' + struct.pack('>iii', index_offset, data_offset, data_offset + len(data))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(header + index + data)
    print(f'{os.path.getsize(path) // 1024:>8} KB  {os.path.relpath(path, DIST)}')


write_tzdata(os.path.join(DIST, 'root', 'system', 'usr', 'share', 'zoneinfo', 'tzdata'))
subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'gen-notices.py'), os.path.join(DIST, 'THIRD-PARTY-NOTICES.txt')])
print('dist ready:', DIST)
