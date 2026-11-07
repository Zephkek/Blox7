"""builds the third-party libs for android x86_64 api 28 with the ndk
usage: deps.py [name ...]"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
SRC = os.environ.get('BLOX7_DEPS_SRC', 'D:/toolchains/src')
NDK = os.environ.get('BLOX7_NDK', 'D:/toolchains/android-ndk-r28c')
NINJA = os.environ.get('BLOX7_NINJA',
                       'C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
PREFIX = os.path.join(WIN, 'out', 'deps').replace('\\', '/')
BUILD = os.path.join(WIN, 'out', 'deps-build')

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


def run(cmd, cwd=None):
    print('>', ' '.join(cmd[:4]), '...', flush=True)
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
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


if __name__ == '__main__':
    wanted = sys.argv[1:] or [d[0] for d in DEPS]
    for name, src, extra in DEPS:
        if name in wanted:
            build(name, src, extra)
