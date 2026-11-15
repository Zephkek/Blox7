"""checks that the runtime libs in dist export everything a roblox libroblox.so imports
usage: check-imports.py <libroblox.so> [dist dir]"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
NDK = os.environ.get('BLOX7_NDK', 'D:/toolchains/android-ndk-r28c')
BIN = f'{NDK}/toolchains/llvm/prebuilt/windows-x86_64/bin'
MERGED = {'libm.so': 'libc.so', 'libdl.so': 'libc.so'}


def tool(name, *args):
    return subprocess.run([f'{BIN}/{name}.exe', *args], check=True, capture_output=True, text=True).stdout


def needed(path):
    return [line.split('[')[1].rstrip(']') for line in tool('llvm-readelf', '-d', path).splitlines()
            if '(NEEDED)' in line]


def symbols(path, flag):
    for line in tool('llvm-nm', '-D', flag, path).splitlines():
        parts = line.split()
        if len(parts) >= 2:
            yield parts[-2], parts[-1].split('@')[0]


def main():
    lib = sys.argv[1]
    dist = sys.argv[2] if len(sys.argv) > 2 else os.path.join(WIN, 'out', 'dist')
    exports, missing_libs = set(), []
    for name in needed(lib):
        name = MERGED.get(name, name)
        found = [p for p in (os.path.join(dist, 'lib', 'blox7', name), os.path.join(dist, 'bin', name))
                 if os.path.exists(p)]
        if not found:
            missing_libs.append(name)
            continue
        exports.update(sym for _, sym in symbols(found[0], '--defined-only'))
    imports = [(kind, sym) for kind, sym in symbols(lib, '--undefined-only')]
    missing = sorted(sym for kind, sym in imports if kind == 'U' and sym not in exports)
    for name in missing_libs:
        print(f'missing library {name}')
    for sym in missing:
        print(f'missing {sym}')
    print(f'{len(imports)} imports, {len(missing)} unresolved, {len(missing_libs)} libraries missing')
    return 1 if missing or missing_libs else 0


if __name__ == '__main__':
    sys.exit(main())
