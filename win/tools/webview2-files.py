"""writes win/webview2/wv2files.h, the offsets and files blox7 unpacks webview2 109 with, from microsoft's package
usage: webview2-files.py <catalog package>"""
import hashlib
import lzma
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'webview2', 'wv2files.h')
VERSION = '109.0.1518.140'
PREFIX = f'Chrome-bin/{VERSION}/'
BROWSER_ONLY = {
    'BHO/ie_to_edge_bho.dll', 'BHO/ie_to_edge_bho_64.dll', 'BHO/ie_to_edge_stub.exe', 'VisualElements/Logo.png',
    'VisualElements/LogoBeta.png', 'VisualElements/LogoCanary.png', 'VisualElements/LogoDev.png',
    'VisualElements/SmallLogo.png', 'VisualElements/SmallLogoBeta.png', 'VisualElements/SmallLogoCanary.png',
    'VisualElements/SmallLogoDev.png', 'cookie_exporter.exe', 'elevation_service.exe', 'identity_helper.exe',
    'identity_proxy/beta.identity_helper.exe.manifest', 'identity_proxy/canary.identity_helper.exe.manifest',
    'identity_proxy/dev.identity_helper.exe.manifest', 'identity_proxy/identity_helper.Sparse.Beta.msix',
    'identity_proxy/identity_helper.Sparse.Canary.msix', 'identity_proxy/identity_helper.Sparse.Dev.msix',
    'identity_proxy/identity_helper.Sparse.Internal.msix', 'identity_proxy/identity_helper.Sparse.Stable.msix',
    'identity_proxy/internal.identity_helper.exe.manifest', 'identity_proxy/resources.pri',
    'identity_proxy/stable.identity_helper.exe.manifest', 'msedge.exe', 'msedge.exe.sig', 'msedge_proxy.exe',
    'msedge_pwa_launcher.exe', 'pwahelper.dll', 'pwahelper.exe',
}
LZMA, BCJ2, COPY = '030101', '0303011b', '00'
ROLES = ('MAIN', 'CALL', 'JUMP', 'RC')


def fail(text):
    sys.exit('webview2-files: ' + text)


def resource(data, kind, name):
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        fail('not a pe file')
    count, = struct.unpack_from('<H', data, pe + 6)
    optional_size, = struct.unpack_from('<H', data, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from('<H', data, optional)
    rva, = struct.unpack_from('<I', data, optional + (96 if magic == 0x10B else 112) + 16)
    sections = [struct.unpack_from('<IIII', data, optional + optional_size + 40 * i + 8) for i in range(count)]

    def offset(at):
        for virtual_size, virtual, raw_size, raw in sections:
            if virtual <= at < virtual + max(virtual_size, raw_size):
                return raw + at - virtual
        fail(f'rva {at:#x} is in no section')

    base = offset(rva)

    def entries(at):
        named, ids = struct.unpack_from('<HH', data, base + at + 12)
        for i in range(named + ids):
            key, target = struct.unpack_from('<II', data, base + at + 16 + 8 * i)
            if key & 0x80000000:
                text = base + (key & 0x7FFFFFFF)
                length, = struct.unpack_from('<H', data, text)
                key = data[text + 2:text + 2 + 2 * length].decode('utf-16le')
            yield key, target & 0x7FFFFFFF

    for kind_key, kind_at in entries(0):
        if kind_key != kind:
            continue
        for name_key, name_at in entries(kind_at):
            if name_key != name:
                continue
            for _, leaf in entries(name_at):
                at, size = struct.unpack_from('<II', data, base + leaf)
                return offset(at), size
    fail(f'no resource {kind}/{name}')


def tar_member(data, at, size, wanted):
    end = at + size
    while at + 512 <= end and data[at:at + 512] != bytes(512):
        name = data[at:at + 100].split(b'\0')[0].decode('utf-8')
        length = int(data[at + 124:at + 136].split(b'\0')[0].strip() or b'0', 8)
        if wanted(name):
            return name, at + 512, length
        at += 512 + (length + 511) // 512 * 512
    fail('no edge installer in the package')


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def byte(self):
        self.at += 1
        return self.data[self.at - 1]

    def take(self, count):
        self.at += count
        return self.data[self.at - count:self.at]

    def number(self):
        first, mask, value = self.byte(), 0x80, 0
        for i in range(8):
            if not first & mask:
                return value | ((first & (mask - 1)) << (8 * i))
            value |= self.byte() << (8 * i)
            mask >>= 1
        return value


def bits(r, count):
    out, mask, byte = [], 0, 0
    for _ in range(count):
        if not mask:
            byte, mask = r.byte(), 0x80
        out.append(bool(byte & mask))
        mask >>= 1
    return out


def digests(r, count):
    defined = [True] * count if r.byte() else bits(r, count)
    return [struct.unpack('<I', r.take(4))[0] if d else None for d in defined]


def folder_of(r):
    coders = []
    for _ in range(r.number()):
        flags = r.byte()
        coder = r.take(flags & 0x0F).hex()
        ins, outs = (r.number(), r.number()) if flags & 0x10 else (1, 1)
        props = r.take(r.number()) if flags & 0x20 else b''
        if flags & 0x80:
            fail('7z coder with alternative methods')
        coders.append({'id': coder, 'ins': ins, 'outs': outs, 'props': props})
    ins, outs = sum(c['ins'] for c in coders), sum(c['outs'] for c in coders)
    bonds = [(r.number(), r.number()) for _ in range(outs - 1)]
    if ins - len(bonds) == 1:
        bound = {b[0] for b in bonds}
        packed = [i for i in range(ins) if i not in bound]
    else:
        packed = [r.number() for _ in range(ins - len(bonds))]
    return {'coders': coders, 'bonds': bonds, 'packed': packed, 'outs': outs, 'crc': None}


def final_size(folder):
    bound = {b[1] for b in folder['bonds']}
    return next(folder['unpack'][i] for i in range(folder['outs']) if i not in bound)


def streams_of(r):
    info = {'pack_pos': 0, 'pack_sizes': [], 'folders': [], 'counts': None, 'sizes': None, 'crcs': None}
    while True:
        kind = r.byte()
        if kind == 0x00:
            break
        if kind == 0x06:
            info['pack_pos'] = r.number()
            count = r.number()
            while (part := r.byte()) != 0x00:
                if part == 0x09:
                    info['pack_sizes'] = [r.number() for _ in range(count)]
                elif part == 0x0A:
                    digests(r, count)
                else:
                    fail(f'7z pack info {part:#x}')
        elif kind == 0x07:
            if r.byte() != 0x0B:
                fail('7z folders missing')
            count = r.number()
            if r.byte():
                fail('7z external folders')
            info['folders'] = [folder_of(r) for _ in range(count)]
            if r.byte() != 0x0C:
                fail('7z unpack sizes missing')
            for folder in info['folders']:
                folder['unpack'] = [r.number() for _ in range(folder['outs'])]
            while (part := r.byte()) != 0x00:
                if part != 0x0A:
                    fail(f'7z unpack info {part:#x}')
                for folder, crc in zip(info['folders'], digests(r, count)):
                    folder['crc'] = crc
        elif kind == 0x08:
            counts = [1] * len(info['folders'])
            sizes = crcs = None
            while (part := r.byte()) != 0x00:
                if part == 0x0D:
                    counts = [r.number() for _ in info['folders']]
                elif part == 0x09:
                    sizes = []
                    for folder, n in zip(info['folders'], counts):
                        if n:
                            some = [r.number() for _ in range(n - 1)]
                            sizes += some + [final_size(folder) - sum(some)]
                elif part == 0x0A:
                    known = [n == 1 and f['crc'] is not None for f, n in zip(info['folders'], counts)]
                    read = iter(digests(r, sum(n for n, k in zip(counts, known) if not k)))
                    crcs = []
                    for folder, n, k in zip(info['folders'], counts, known):
                        crcs += [folder['crc']] if k else [next(read) for _ in range(n)]
                else:
                    fail(f'7z substreams {part:#x}')
            if sizes is None:
                sizes = [final_size(f) for f, n in zip(info['folders'], counts) if n == 1]
            info['counts'], info['sizes'], info['crcs'] = counts, sizes, crcs
        else:
            fail(f'7z streams info {kind:#x}')
    if info['counts'] is None:
        info['counts'] = [1] * len(info['folders'])
        info['sizes'] = [final_size(f) for f in info['folders']]
        info['crcs'] = [f['crc'] for f in info['folders']]
    return info


def lzma_raw(data, props, size):
    lc, rest = props[0] % 9, props[0] // 9
    dictionary, = struct.unpack('<I', props[1:5])
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': dictionary, 'lc': lc, 'lp': rest % 5, 'pb': rest // 5}]
    out = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters).decompress(data, max_length=size)
    if len(out) != size:
        fail(f'lzma gave {len(out)} of {size} bytes')
    return out


MARKER = re.compile(rb'[\xe8\xe9]|\x0f[\x80-\x8f]')


def bcj2(main, call, jump, rc, size):
    if rc[0] != 0:
        fail('bcj2 range coder does not start with 0')
    probs = [1024] * 258
    out = bytearray()
    at = calls = jumps = 0
    span, code, rc_at = 0xFFFFFFFF, int.from_bytes(rc[1:5], 'big'), 5
    last = 0
    while len(out) < size:
        if at < len(main) and last == 0x0F and main[at] & 0xF0 == 0x80:
            mark = at
        else:
            found = MARKER.search(main, at)
            if found is None:
                out += main[at:]
                at = len(main)
                break
            mark = found.end() - 1
        before = main[mark - 1] if mark > at else last
        out += main[at:mark + 1]
        at = mark + 1
        if len(out) >= size:
            break
        byte = main[mark]
        index = before if byte == 0xE8 else 256 if byte == 0xE9 else 257
        bound = (span >> 11) * probs[index]
        if code < bound:
            span = bound
            probs[index] += (2048 - probs[index]) >> 5
            taken = False
        else:
            span -= bound
            code -= bound
            probs[index] -= probs[index] >> 5
            taken = True
        if span < 1 << 24:
            span = (span << 8) & 0xFFFFFFFF
            code = ((code << 8) | rc[rc_at]) & 0xFFFFFFFF
            rc_at += 1
        if not taken:
            last = byte
            continue
        if byte == 0xE8:
            target, calls = int.from_bytes(call[calls:calls + 4], 'big'), calls + 4
        else:
            target, jumps = int.from_bytes(jump[jumps:jumps + 4], 'big'), jumps + 4
        target = (target - (len(out) + 4)) & 0xFFFFFFFF
        out += target.to_bytes(4, 'little')
        last = target >> 24
    if len(out) < size or at != len(main) or calls != len(call) or jumps != len(jump):
        fail('bcj2 streams do not line up')
    return bytes(out[:size])


def first_in(coders):
    bases, total = [], 0
    for coder in coders:
        bases.append(total)
        total += coder['ins']
    return bases


def first_out(coders):
    bases, total = [], 0
    for coder in coders:
        bases.append(total)
        total += coder['outs']
    return bases


def decode(buf, at, sizes, folder):
    coders = folder['coders']
    ins, outs = first_in(coders), first_out(coders)
    packed = {}
    for index, size in zip(folder['packed'], sizes):
        packed[index] = buf[at:at + size]
        at += size
    bond = dict(folder['bonds'])
    done = {}

    def stream_in(index):
        return packed[index] if index in packed else stream_out(bond[index])

    def stream_out(index):
        if index not in done:
            which = max(i for i, base in enumerate(outs) if base <= index)
            coder, size = coders[which], folder['unpack'][index]
            if coder['id'] == LZMA:
                done[index] = lzma_raw(stream_in(ins[which]), coder['props'], size)
            elif coder['id'] == BCJ2:
                done[index] = bcj2(*[stream_in(ins[which] + k) for k in range(4)], size)
            elif coder['id'] == COPY:
                done[index] = stream_in(ins[which])[:size]
            else:
                fail(f'7z coder {coder["id"]}')
        return done[index]

    bound = {b[1] for b in folder['bonds']}
    data = stream_out(next(i for i in range(folder['outs']) if i not in bound))
    if folder['crc'] is not None and zlib.crc32(data) != folder['crc']:
        fail('7z folder crc differs')
    return data


def archive(buf):
    if buf[:6] != b'7z\xbc\xaf\x27\x1c':
        fail('not a 7z archive')
    if zlib.crc32(buf[12:32]) != struct.unpack_from('<I', buf, 8)[0]:
        fail('7z start header crc differs')
    at, size, crc = struct.unpack_from('<QQI', buf, 12)
    header = buf[32 + at:32 + at + size]
    if zlib.crc32(header) != crc:
        fail('7z header crc differs')
    r = Reader(header)
    kind = r.byte()
    if kind == 0x17:
        info = streams_of(r)
        header = decode(buf, 32 + info['pack_pos'], info['pack_sizes'], info['folders'][0])
        r = Reader(header)
        kind = r.byte()
    if kind != 0x01:
        fail(f'7z header {kind:#x}')
    streams = files = None
    while (part := r.byte()) != 0x00:
        if part == 0x04:
            streams = streams_of(r)
        elif part == 0x05:
            count = r.number()
            names, empty = None, [False] * count
            while (prop := r.byte()) != 0x00:
                size = r.number()
                start = r.at
                if prop == 0x11:
                    if r.byte():
                        fail('7z external names')
                    names = r.data[r.at:start + size].decode('utf-16le').split('\0')[:count]
                elif prop == 0x0E:
                    empty = bits(r, count)
                r.at = start + size
            files = [{'name': names[i], 'empty': empty[i]} for i in range(count)]
        else:
            fail(f'7z header part {part:#x}')
    return streams, files


def roles_of(folder):
    coders = folder['coders']
    ins, outs = first_in(coders), first_out(coders)
    split = [i for i, c in enumerate(coders) if c['id'] == BCJ2 and c['ins'] == 4 and c['outs'] == 1]
    if len(split) != 1 or len(coders) != 4:
        fail('the package is not three lzma streams into bcj2')
    bound = {b[1] for b in folder['bonds']}
    if [i for i in range(folder['outs']) if i not in bound] != [outs[split[0]]]:
        fail('bcj2 is not the last coder')
    bond = dict(folder['bonds'])
    roles = {}
    for role, index in zip(ROLES, range(ins[split[0]], ins[split[0]] + 4)):
        if index in bond:
            which = max(i for i, first in enumerate(outs) if first <= bond[index])
            coder = coders[which]
            if coder['id'] != LZMA or ins[which] not in folder['packed'] or role == 'RC':
                fail(f'bcj2 {role.lower()} stream is not lzma from the package')
            roles[role] = (folder['packed'].index(ins[which]), coder['props'], folder['unpack'][bond[index]])
        elif index in folder['packed'] and role == 'RC':
            roles[role] = (folder['packed'].index(index), None, None)
        else:
            fail(f'bcj2 {role.lower()} stream has no source')
    return roles


def main():
    if len(sys.argv) != 2:
        fail('usage: webview2-files.py <catalog package>')
    data = open(sys.argv[1], 'rb').read()
    package = hashlib.sha256(data).hexdigest()
    print('package', len(data), 'bytes, sha-256', package)
    at, size = resource(data, 'D', 102)
    name, mini_at, mini_size = tar_member(data, at, size,
                                          lambda n: n.startswith('MicrosoftEdge_X64_') and '.exe.{' in n)
    mini = data[mini_at:mini_at + mini_size]
    print('installer', name, mini_size, 'bytes at', mini_at, 'sha-256', hashlib.sha256(mini).hexdigest())
    packed_at, packed_size = resource(mini, 'B7', 'MSEDGE.PACKED.7Z')
    packed = mini[packed_at:packed_at + packed_size]
    outer, outer_files = archive(packed)
    if len(outer['folders']) != 1 or [f['name'] for f in outer_files] != ['MSEDGE.7z']:
        fail('MSEDGE.PACKED.7Z does not hold just MSEDGE.7z')
    folder = outer['folders'][0]
    base = mini_at + packed_at + 32 + outer['pack_pos']
    starts = [base + sum(outer['pack_sizes'][:i]) for i in range(len(outer['pack_sizes']))]
    roles = roles_of(folder)
    body = decode(packed, 32 + outer['pack_pos'], outer['pack_sizes'], folder)
    total = len(body)
    if total != roles['MAIN'][2] + roles['CALL'][2] + roles['JUMP'][2]:
        fail('bcj2 output is not main + call + jump')
    for role in ROLES:
        pack, props, unpack = roles[role]
        print(f'{role.lower():4} pack {pack} at {starts[pack]} size {outer["pack_sizes"][pack]}',
              f'props {props.hex()} unpacks to {unpack}' if props else 'raw')
    print('MSEDGE.7z', total, 'bytes, sha-256', hashlib.sha256(body).hexdigest())
    inner, files = archive(body)
    if any(len(f['coders']) != 1 or f['coders'][0]['id'] != COPY for f in inner['folders']):
        fail('MSEDGE.7z is not stored')
    stored = iter(f for f in files if not f['empty'])
    sizes, crcs = iter(inner['sizes']), iter(inner['crcs'] or [None] * len(inner['sizes']))
    table, at = [], 32 + inner['pack_pos']
    for folder, count, pack_size in zip(inner['folders'], inner['counts'], inner['pack_sizes']):
        offset = at
        for _ in range(count):
            entry, size, crc = next(stored), next(sizes), next(crcs)
            name = entry['name'].replace('\\', '/')
            if not name.startswith(PREFIX) or not size or not name.isascii():
                fail(f'unexpected file {name}')
            if crc is not None and zlib.crc32(body[offset:offset + size]) != crc:
                fail(f'crc differs for {name}')
            table.append((name[len(PREFIX):], offset, size, hashlib.sha256(body[offset:offset + size]).hexdigest()))
            offset += size
        at += pack_size
    if any(a[1] + a[2] > b[1] for a, b in zip(table, table[1:])) or table[-1][1] + table[-1][2] > total:
        fail('files overlap or run past the archive')
    missing = BROWSER_ONLY - {name for name, _, _, _ in table}
    if missing:
        fail('browser-only files not in the archive: ' + ', '.join(sorted(missing)))
    fixed = [row for row in table if row[0] not in BROWSER_ONLY]
    print('files', len(table), 'bytes', sum(r[2] for r in table), 'first at', table[0][1], 'last ends at',
          table[-1][1] + table[-1][2])
    print('fixed version subset', len(fixed), 'files', sum(r[2] for r in fixed), 'bytes')

    lines = ['#pragma once', '', f'#define WV2_PACKAGE_BYTES {len(data)}', f'#define WV2_PACKAGE_SHA256 "{package}"',
             f'#define WV2_ARCHIVE_BYTES {total}']
    for role in ROLES:
        pack, props, unpack = roles[role]
        lines.append(f'#define WV2_{role}_AT {starts[pack]}')
        lines.append(f'#define WV2_{role}_{"PACKED" if props else "BYTES"} {outer["pack_sizes"][pack]}')
        if props:
            lines.append(f'#define WV2_{role}_BYTES {unpack}')
            lines.append(f'#define WV2_{role}_PROPS {{{", ".join(f"0x{b:02x}" for b in props)}}}')
    lines += [f'#define WV2_FIXED_FILES {len(fixed)}', f'#define WV2_FIXED_BYTES {sum(r[2] for r in fixed)}',
              f'#define WV2_ALL_FILES {len(table)}', f'#define WV2_ALL_BYTES {sum(r[2] for r in table)}', '',
              'struct wv2_file {', '  const wchar_t* name;', '  unsigned offset;', '  unsigned size;', '  int fixed;',
              '  const char* sha256;', '};', '', 'static const struct wv2_file wv2_files[] = {']
    for name, offset, size, digest in table:
        quoted = name.replace('/', '\\\\')
        lines.append(f'    {{L"{quoted}", {offset}, {size}, {0 if name in BROWSER_ONLY else 1},')
        lines.append(f'     "{digest}"}},')
    lines += ['};', '']
    if max(len(line) for line in lines) > 120:
        fail('a line of the header is over 120 columns')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
