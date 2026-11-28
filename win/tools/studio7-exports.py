"""writes the export lists of studio7's shim dlls (win/studio7/layer/s7*.def) from a windows 7 system32
usage: studio7-exports.py <system32 dir>"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LAYER = os.path.join(os.path.dirname(HERE), 'studio7', 'layer')
OURS = {
    'kernel32': ('s7kernel', ['WaitOnAddress', 'WakeByAddressSingle', 'WakeByAddressAll', 'GetSystemTimePreciseAsFileTime',
                              'GetCurrentThreadStackLimits', 'CreateFile2', 'CopyFile2', 'PrefetchVirtualMemory',
                              'VerifyVersionInfoW'], ['S7Anchor'], ['WakeAllConditionVariable', 'WakeConditionVariable',
                                                                    'LoadLibraryExW']),
    'user32': ('s7user', ['GetDpiForWindow', 'GetDpiForSystem', 'GetSystemMetricsForDpi', 'AdjustWindowRectExForDpi',
                          'GetCurrentInputMessageSource'], [], ['RegisterClassExW', 'SendMessageW']),
    'ws2_32': ('s7ws2', ['GetAddrInfoExW', 'GetAddrInfoExCancel', 'GetAddrInfoExOverlappedResult'], [], []),
    'mfplat': ('s7mf', ['MFCreateDXGIDeviceManager', 'MFCreateDXGISurfaceBuffer'], [], ['MFTEnumEx']),
    'bcryptprimitives': ('s7bcp', ['ProcessPrng'], [], []),
    'advapi32': ('s7adv', ['EventSetInformation'], [], []),
}


def exports(path):
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    count = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    if struct.unpack_from('<H', d, opt)[0] != 0x20b:
        sys.exit(f'{path} is not a 64-bit dll')
    sections = [struct.unpack_from('<IIII', d, opt + struct.unpack_from('<H', d, pe + 20)[0] + 40 * i + 8)
                for i in range(count)]

    def at(rva):
        for size, va, raw_size, raw in sections:
            if va <= rva < va + max(size, raw_size):
                return rva - va + raw
        sys.exit(f'{path} has a bad export table')

    table = at(struct.unpack_from('<I', d, opt + 112)[0])
    base, _, names, _, name_rvas, ordinals = struct.unpack_from('<IIIIII', d, table + 16)
    out = []
    for i in range(names):
        start = at(struct.unpack_from('<I', d, at(name_rvas) + 4 * i)[0])
        out.append((d[start:d.index(b'\0', start)].decode('ascii'), base + struct.unpack_from('<H', d, at(ordinals) + 2 * i)[0]))
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for dll, (shim, added, plain, replaced) in OURS.items():
        real = exports(os.path.join(sys.argv[1], dll + '.dll'))
        lines = [f'LIBRARY {shim}', 'EXPORTS']
        lines += [f'    {name} = {"s7_" if name in replaced else dll + "."}{name} @{ordinal}' for name, ordinal in real
                  if name not in added]
        free = max(ordinal for _, ordinal in real) + 1000
        lines += [f'    {name} = s7_{name} @{free + i}' for i, name in enumerate(added)]
        lines += [f'    {name} @{free + len(added) + i}' for i, name in enumerate(plain)]
        with open(os.path.join(LAYER, shim + '.def'), 'w', encoding='ascii', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        print(f'{shim}.def: {len(real)} names from {dll}.dll, {len(added) + len(replaced)} of our own')


if __name__ == '__main__':
    main()
