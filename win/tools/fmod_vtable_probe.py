"""finds a vtable by its rtti name and lines its slots up across builds
usage: fmod_vtable_probe.py <rtti name> <slots> <elf> [<elf> ...]"""
import struct
import sys


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = d = open(path, 'rb').read()
        phoff, = struct.unpack_from('<Q', d, 0x20)
        shoff, = struct.unpack_from('<Q', d, 0x28)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
        self.loads = []
        for i in range(phnum):
            typ, flags, off, vaddr, paddr, filesz = struct.unpack_from('<IIQQQQ', d, phoff + i * phentsize)
            if typ == 1:
                self.loads.append((vaddr, off, filesz))
        secs = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                '<IIQQQQIIQQ', d, shoff + i * shentsize)
            secs.append((name, typ, addr, off, size, entsize))
        stroff = secs[shstrndx][3]
        self.sections = {}
        for name, typ, addr, off, size, entsize in secs:
            end = d.index(b'\0', stroff + name)
            self.sections[d[stroff + name:end].decode()] = (typ, addr, off, size, entsize)
        self.relative = {}
        self.symbolic = {}
        for sname, (typ, addr, off, size, entsize) in self.sections.items():
            if typ == 4:
                relocs = (struct.unpack_from('<QQq', d, o) for o in range(off, off + size, 24))
            elif typ == 0x60000002:
                relocs = self.aps2(off, size)
            else:
                continue
            for r_offset, r_info, r_addend in relocs:
                rtype = r_info & 0xFFFFFFFF
                if rtype == 8:
                    self.relative[r_offset] = r_addend
                elif rtype in (1, 6, 7):
                    self.symbolic[r_offset] = (r_info >> 32, r_addend)

    def aps2(self, off, size):
        d = self.data
        assert d[off:off + 4] == b'APS2'
        pos = off + 4

        def sleb():
            nonlocal pos
            result = shift = 0
            while True:
                b = d[pos]
                pos += 1
                result |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    if b & 0x40:
                        result -= 1 << shift
                    return result

        count = sleb()
        r_offset = sleb()
        r_info = r_addend = 0
        done = 0
        while done < count:
            group_size = sleb()
            flags = sleb()
            by_info, by_delta, by_addend, has_addend = flags & 1, flags & 2, flags & 4, flags & 8
            delta = sleb() if by_delta else 0
            if by_info:
                r_info = sleb()
            if has_addend and by_addend:
                r_addend += sleb()
            elif not has_addend:
                r_addend = 0
            for _ in range(group_size):
                r_offset += delta if by_delta else sleb()
                if not by_info:
                    r_info = sleb()
                if has_addend and not by_addend:
                    r_addend += sleb()
                yield r_offset & 0xFFFFFFFFFFFFFFFF, r_info & 0xFFFFFFFFFFFFFFFF, r_addend
            done += group_size

    def off(self, va):
        for vaddr, off, filesz in self.loads:
            if vaddr <= va < vaddr + filesz:
                return off + va - vaddr
        return None

    def va_of_file(self, fo):
        for vaddr, off, filesz in self.loads:
            if off <= fo < off + filesz:
                return vaddr + fo - off
        return None

    def bytes_at(self, va, n):
        fo = self.off(va)
        return self.data[fo:fo + n] if fo is not None else b''


def vtables_for(elf, rtti):
    fo = elf.data.find(rtti.encode() + b'\0')
    if fo < 0:
        return []
    name_va = elf.va_of_file(fo)
    typeinfos = [o - 8 for o, a in elf.relative.items() if a == name_va]
    return [o + 8 for o, a in elf.relative.items() if a in typeinfos]


rtti, slots = sys.argv[1], int(sys.argv[2])
elves = [Elf(p) for p in sys.argv[3:]]
tables = []
for elf in elves:
    vts = vtables_for(elf, rtti)
    print(f'{elf.path}: vtables {[hex(v) for v in vts]}')
    tables.append(vts[0] if vts else None)
for i in range(slots):
    row = [f'{i:2d}']
    for elf, vt in zip(elves, tables):
        if vt is None:
            row.append('-')
            continue
        slot = vt + 8 * i
        if slot in elf.relative:
            target = elf.relative[slot]
            row.append(f'{target:#9x} {elf.bytes_at(target, 10).hex()}')
        elif slot in elf.symbolic:
            row.append(f'sym{elf.symbolic[slot]}')
        else:
            row.append('(none)')
    print('  '.join(f'{c:<32}' for c in row))
