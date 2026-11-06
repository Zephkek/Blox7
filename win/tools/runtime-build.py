"""configures and builds the runtime with the ndk
usage: runtime-build.py [--configure] [target ...]"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
ROOT = os.path.dirname(WIN)
OUT = os.path.join(WIN, 'out').replace('\\', '/')
BUILD = os.path.join(WIN, 'out', 'runtime-build')
sys.path.insert(0, HERE)
import deps  # noqa: E402  (NDK, ninja and the deps prefix)

args = [a for a in sys.argv[1:] if not a.startswith('--')]
targets = args or ['blox7', 'blox7_updater']

if '--configure' in sys.argv or not os.path.exists(os.path.join(BUILD, 'build.ninja')):
    common = [a for a in deps.COMMON if not a.startswith(('-DCMAKE_INSTALL_PREFIX', '-DCMAKE_SHARED_LINKER_FLAGS', '-DCMAKE_EXE_LINKER_FLAGS'))]
    cmd = ['cmake', '-S', ROOT, '-B', BUILD] + common + [
        f'-DCMAKE_MODULE_PATH={WIN}/cmake'.replace('\\', '/'),
        f'-DSDL3_DIR={WIN}/cmake'.replace('\\', '/'),
        f'-DSDL3_ttf_DIR={WIN}/cmake'.replace('\\', '/'),
        f'-DBLOX7_WIN_DEPS_PREFIX={deps.PREFIX}',
        f'-DBLOX7_WIN_OUT={OUT}',
        f'-DBLOX7_WIN_SDL_SOURCE={deps.SRC}/SDL',
        f'-DBLOX7_WIN_SDL_TTF_SOURCE={deps.SRC}/SDL_ttf',
        f'-DOPENSSL_ROOT_DIR={deps.PREFIX}',
        f'-DCURL_DIR={deps.PREFIX}/lib/cmake/CURL',
        f'-Dnlohmann_json_DIR={deps.PREFIX}/share/cmake/nlohmann_json',
        f'-DZLIB_ROOT={deps.PREFIX}',
        f'-DCMAKE_C_FLAGS=-I{deps.SRC}/SDL/include -DBLOX7_WIN32=1',
        f'-DCMAKE_CXX_FLAGS=-I"{deps.PREFIX}/include" -DBLOX7_WIN32=1 -include "{WIN}/compat/blox7_win32_compat.h"'.replace('\\', '/'),
        f'-DCMAKE_EXE_LINKER_FLAGS=-Wl,--pack-dyn-relocs=relr -Wl,--export-dynamic-symbol=main -L"{OUT}"',
        f'-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--pack-dyn-relocs=relr -L"{OUT}"',
        '-DBUILD_TESTING=OFF',
        '-DBLOX7_BUILD_FREEBSD_SOCKET_HELPER=OFF',
        '-DBLOX7_WIN32=ON',
    ]
    p = subprocess.run(cmd, capture_output=True, text=True)
    print(p.stdout[-3000:])
    if p.returncode:
        print(p.stderr[-6000:])
        sys.exit('configure failed')

p = subprocess.run(['cmake', '--build', BUILD, '--parallel', '--target'] + targets + ['--', '-k', '0'], capture_output=True, text=True)
out = p.stdout + p.stderr
open(os.path.join(WIN, 'out', 'runtime-build.log'), 'w').write(out)
errors = [l for l in out.splitlines() if 'error:' in l or 'FAILED:' in l]
print('\n'.join(errors[:80]))
print(f'{len(errors)} error lines; build {"ok" if p.returncode == 0 else "failed"}')
sys.exit(p.returncode)
