"""lists the raw `syscall` instructions in an elf's .text and the function around each
usage: rawsys.py <elf> [context instructions, default 6]"""
import bisect
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

path = sys.argv[1]
context = int(sys.argv[2]) if len(sys.argv) > 2 else 6
data = open(path, 'rb').read()

shoff, = struct.unpack_from('<Q', data, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
headers = [struct.unpack_from('<IIQQQQ', data, shoff + i * shentsize) for i in range(shnum)]
stroff = headers[shstrndx][4]


def section_name(offset):
    end = data.index(b'\0', stroff + offset)
    return data[stroff + offset:end].decode()


sections = {section_name(h[0]): (h[3], h[4], h[5]) for h in headers}


def file_offset(va):
    for addr, off, size in sections.values():
        if addr <= va < addr + size and off:
            return off + va - addr
    raise KeyError(hex(va))


text_addr, text_off, text_size = sections['.text']
hdr_addr, hdr_off, _ = sections['.eh_frame_hdr']
version, ptr_enc, count_enc, table_enc = data[hdr_off:hdr_off + 4]
assert (version, count_enc, table_enc) == (1, 0x03, 0x3B), (version, count_enc, table_enc)
fde_count, = struct.unpack_from('<I', data, hdr_off + 8)
starts, sizes = [], []
for i in range(fde_count):
    loc, fde = struct.unpack_from('<ii', data, hdr_off + 12 + 8 * i)
    start, fde_va = hdr_addr + loc, hdr_addr + fde
    fo = file_offset(fde_va)
    length, = struct.unpack_from('<I', data, fo)
    assert length != 0xFFFFFFFF
    begin_rel, pc_range = struct.unpack_from('<iI', data, fo + 8)
    if fde_va + 8 + begin_rel != start:
        continue
    starts.append(start)
    sizes.append(pc_range)

md = Cs(CS_ARCH_X86, CS_MODE_64)
code = data[text_off:text_off + text_size]
found = []
i = code.find(b'\x0f\x05')
while i >= 0:
    va = text_addr + i
    k = bisect.bisect_right(starts, va) - 1
    if k >= 0 and va < starts[k] + sizes[k]:
        start, size = starts[k], sizes[k]
        body = data[file_offset(start):file_offset(start) + (va - start) + 2]
        trail = []
        for insn in md.disasm(body, start):
            trail.append(insn)
            if insn.address >= va:
                break
        if trail and trail[-1].address == va and trail[-1].mnemonic == 'syscall':
            found.append((va, start, size, trail[-1 - context:-1]))
    i = code.find(b'\x0f\x05', i + 1)

per_function = {}
for va, start, size, _ in found:
    per_function.setdefault(start, []).append(va)
print(f'{path}: {len(found)} raw syscall instructions in {len(per_function)} functions')
for va, start, size, before in found:
    n = per_function[start].index(va)
    print(f'{va:#x}  fn {start:#x}+{va - start:#x} (size {size:#x}, #{n} of {len(per_function[start])})')
    for insn in before:
        print(f'      {insn.address:#x}  {insn.mnemonic} {insn.op_str}')
