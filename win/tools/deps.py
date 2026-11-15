"""builds the third-party libs for android x86_64 api 28 with the ndk
usage: deps.py [name ...]"""
import os, shlex, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
SRC = os.environ.get('BLOX7_DEPS_SRC', 'D:/toolchains/src')
NDK = os.environ.get('BLOX7_NDK', 'D:/toolchains/android-ndk-r28c')
NINJA = os.environ.get('BLOX7_NINJA',
                       'C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
PREFIX = os.path.join(WIN, 'out', 'deps').replace('\\', '/')
BUILD = os.path.join(WIN, 'out', 'deps-build')
NASM = os.environ.get('BLOX7_NASM', 'D:/toolchains/nasm-3.02/nasm.exe')
BASH = os.environ.get('BLOX7_BASH', 'C:/Program Files/Git/bin/bash.exe')
MINGW = os.environ.get('BLOX7_MINGW', 'D:/toolchains/llvm-mingw-20261020-msvcrt-x86_64')
FFMPEG_BUILD = os.environ.get('BLOX7_FFMPEG_BUILD', 'D:/toolchains/build/ffmpeg')
MESON = os.environ.get('BLOX7_MESON', 'D:/toolchains/meson-venv/Scripts/meson.exe')
OPENH264_BUILD = os.environ.get('BLOX7_OPENH264_BUILD', 'D:/toolchains/build/openh264')

COMMON = [
    '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={NINJA}',
    f'-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake',
    '-DANDROID_ABI=x86_64', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={PREFIX}', f'-DCMAKE_PREFIX_PATH={PREFIX}',
    f'-DCMAKE_FIND_ROOT_PATH={PREFIX}', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
    # our loader reads RELR or plain relocs, not android's packed ones
    '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--pack-dyn-relocs=relr', '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--pack-dyn-relocs=relr',
]

DEPS = [
    ('zlib', 'zlib', ['-DZLIB_BUILD_EXAMPLES=OFF', '-DZLIB_BUILD_TESTING=OFF']),
    ('boringssl', 'boringssl', ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DOPENSSL_SMALL=OFF']),
    ('curl', 'curl', ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON', '-DCURL_USE_OPENSSL=ON', '-DBUILD_CURL_EXE=OFF',
                      '-DBUILD_TESTING=OFF', '-DCURL_USE_LIBPSL=OFF', '-DCURL_USE_LIBSSH2=OFF', '-DUSE_NGHTTP2=OFF',
                      '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF', '-DUSE_LIBIDN2=OFF', '-DENABLE_ARES=OFF',
                      '-DCURL_DISABLE_LDAP=ON', '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF', '-DENABLE_CURL_MANUAL=OFF',
                      f'-DOPENSSL_ROOT_DIR={PREFIX}', '-DHAVE_BORINGSSL=1',
                      '-DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt', '-DCURL_CA_PATH=none']),
    ('libyaml', 'libyaml', ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DINSTALL_CMAKE_DIR=lib/cmake/yaml']),
    ('capstone', 'capstone', ['-DBUILD_SHARED_LIBS=OFF', '-DCAPSTONE_BUILD_TESTS=OFF', '-DCAPSTONE_BUILD_CSTOOL=OFF',
                              '-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF', '-DCAPSTONE_X86_SUPPORT=ON', '-DCAPSTONE_ARM64_SUPPORT=ON']),
    ('utf8proc', 'utf8proc', ['-DBUILD_SHARED_LIBS=OFF', '-DUTF8PROC_ENABLE_TESTING=OFF']),
    ('freetype', 'freetype', ['-DBUILD_SHARED_LIBS=OFF', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_PNG=ON', '-DFT_DISABLE_HARFBUZZ=ON',
                              '-DFT_DISABLE_BROTLI=ON', '-DFT_REQUIRE_ZLIB=ON']),
    ('json', 'json', ['-DJSON_BuildTests=OFF', '-DJSON_Install=ON']),
    ('libpng', 'libpng', ['-DPNG_SHARED=OFF', '-DPNG_STATIC=ON', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF',
                          '-DPNG_FRAMEWORK=OFF', f'-DZLIB_ROOT={PREFIX}']),
    ('extra', os.path.join(WIN, 'cmake', 'extra-deps'), [f'-DZLIB_SOURCE_DIR={SRC}/zlib']),
]

FFMPEG = ['--target-os=android', '--arch=x86_64', '--enable-cross-compile', '--enable-pic', '--enable-shared',
          '--disable-static', '--disable-programs', '--disable-doc', '--disable-network', '--disable-autodetect',
          '--disable-everything', '--enable-decoder=h264,hevc,aac', '--enable-encoder=aac', '--disable-avdevice',
          '--disable-avformat', '--disable-avfilter', '--disable-swresample', '--disable-stripping']


def run(cmd, cwd=None, env=None):
    print('>', ' '.join(cmd[:4]), '...', flush=True)
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if p.returncode:
        print(p.stdout[-4000:])
        print(p.stderr[-4000:])
        sys.exit(f'failed: {cmd[0]} in {cwd}')
    return p.stdout


FIXES = {
    # curl 8.23 vs boringssl, round 1
    'curl': [('lib/vtls/openssl.c', '#if OPENSSL_VERSION_NUMBER < 0x40100000L\nstatic size_t ASN1_STRING_get_length',
              '#if OPENSSL_VERSION_NUMBER < 0x40100000L && !defined(OPENSSL_IS_BORINGSSL)\nstatic size_t ASN1_STRING_get_length')],
}


def apply_fixes(name, src):
    for path, old, new in FIXES.get(name, []):
        p = os.path.join(SRC, src, path)
        text = open(p, encoding='utf-8').read()
        if new not in text:
            if old not in text:
                sys.exit(f'{name}: cannot apply the fix to {path}')
            open(p, 'w', encoding='utf-8', newline='').write(text.replace(old, new))


def build(name, src, extra):
    apply_fixes(name, src)
    b = os.path.join(BUILD, name)
    os.makedirs(b, exist_ok=True)
    run(['cmake', '-S', os.path.join(SRC, src), '-B', b] + COMMON + extra)
    run(['cmake', '--build', b, '--parallel'])
    run(['cmake', '--install', b])
    print(f'{name}: installed')


def build_ffmpeg():
    tc = f'{NDK}/toolchains/llvm/prebuilt/windows-x86_64/bin'
    make = f'{NDK}/prebuilt/windows-x86_64/bin/make.exe'
    stage = f'{FFMPEG_BUILD}/stage'
    os.makedirs(stage, exist_ok=True)
    if not os.path.exists(f'{FFMPEG_BUILD}/src'):
        run(['cmd', '/c', 'mklink', '/J', os.path.normpath(f'{FFMPEG_BUILD}/src'), os.path.normpath(f'{SRC}/ffmpeg')])
    target = '--target=x86_64-none-linux-android28'
    args = [f'--prefix={stage}', f'--cc={tc}/clang.exe', f'--ld={tc}/clang.exe', f'--ar={tc}/llvm-ar.exe',
            f'--nm={tc}/llvm-nm.exe', f'--ranlib={tc}/llvm-ranlib.exe', f'--strip={tc}/llvm-strip.exe',
            f'--x86asmexe={NASM}', f'--host-cc={MINGW}/bin/clang.exe', f'--extra-cflags={target}',
            f'--extra-ldflags={target} -Wl,--pack-dyn-relocs=relr'] + FFMPEG
    script = ' && '.join([shlex.join(['src/configure'] + args),
                          f'{shlex.quote(make)} -j{os.cpu_count()}', f'{shlex.quote(make)} install'])
    run([BASH, '-c', script], cwd=FFMPEG_BUILD)
    shutil.copytree(f'{stage}/include', f'{PREFIX}/include', dirs_exist_ok=True)
    for lib in ('libavcodec.so', 'libavutil.so', 'libswscale.so'):
        shutil.copy2(f'{stage}/lib/{lib}', f'{PREFIX}/lib/{lib}')
    print('ffmpeg: installed')


def build_openh264():
    tc = f'{NDK}/toolchains/llvm/prebuilt/windows-x86_64/bin'
    target = "'--target=x86_64-none-linux-android28'"
    os.makedirs(OPENH264_BUILD, exist_ok=True)
    cross = f'{OPENH264_BUILD}/cross.ini'
    with open(cross, 'w', encoding='utf-8') as f:
        f.write(f"[binaries]\nc = '{tc}/clang.exe'\ncpp = '{tc}/clang++.exe'\nar = '{tc}/llvm-ar.exe'\n"
                f"strip = '{tc}/llvm-strip.exe'\nnasm = '{NASM}'\n\n[built-in options]\n"
                f"c_args = [{target}]\ncpp_args = [{target}]\nc_link_args = [{target}]\ncpp_link_args = [{target}]\n\n"
                "[host_machine]\nsystem = 'android'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n")
    out = f'{OPENH264_BUILD}/out'
    env = {**os.environ, 'NINJA': NINJA}
    if not os.path.exists(f'{out}/build.ninja'):
        run([MESON, 'setup', out, f'{SRC}/openh264', '--cross-file', cross, '--buildtype', 'release',
             '-Ddefault_library=static', '-Dtests=disabled'], env=env)
    run([MESON, 'compile', '-C', out], env=env)
    shutil.copytree(f'{SRC}/openh264/codec/api/wels', f'{PREFIX}/include/wels', dirs_exist_ok=True)
    shutil.copy2(f'{out}/libopenh264.a', f'{PREFIX}/lib/libopenh264.a')
    print('openh264: installed')


if __name__ == '__main__':
    wanted = sys.argv[1:] or [d[0] for d in DEPS] + ['openh264', 'ffmpeg']
    for name, src, extra in DEPS:
        if name in wanted:
            build(name, src, extra)
    if 'openh264' in wanted:
        build_openh264()
    if 'ffmpeg' in wanted:
        build_ffmpeg()
