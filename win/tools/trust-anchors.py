"""writes win/studio7/roots.h, the bearssl trust anchors for the roots in exe/cacert.pem
usage: trust-anchors.py [<cacert.pem> <roots.h>]"""
import base64
import binascii
import calendar
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
PEM = os.path.join(os.path.dirname(WIN), 'exe', 'cacert.pem')
OUT = os.path.join(WIN, 'studio7', 'roots.h')
RSA = '1.2.840.113549.1.1.1'
EC = '1.2.840.10045.2.1'
CURVES = {'1.2.840.10045.3.1.7': ('BR_EC_secp256r1', 32), '1.3.132.0.34': ('BR_EC_secp384r1', 48),
          '1.3.132.0.35': ('BR_EC_secp521r1', 66)}
RSA_BITS = 4096
STRINGS = {0x0C: 'utf-8', 0x13: 'ascii', 0x16: 'ascii', 0x14: 'latin-1', 0x1E: 'utf-16-be', 0x1C: 'utf-32-be'}
ENTRUST_NET_PREMIUM_2048 = '6dc47172e01cbcb0bf62580d895fe2b8ac9ad4f873801e0c10b9c837d21eb177'
ENTRUST_ROOT_G2 = '43df5774b03e7fef5fe40d931a7bedf1bb2e6b42738c4e6d3841103d3aa7f339'
ENTRUST_ROOT_EC1 = '02ed0eb28c14da45165c566791700d6451d7fb56f0b2ab1d3b8eb070e56edff5'
IZENPE = '2530cc8e98321502bad96f9b1fba1b099e2d299e0f4548bb914f363bc0d4531f'
TUBITAK_KAMU_SM_SSL_1 = '46edc3689046d53a453fb3104ab80dcaec658b2660ea1629dd7e867990648716'
SERVER_DISTRUST_AFTER = (ENTRUST_NET_PREMIUM_2048, ENTRUST_ROOT_G2, ENTRUST_ROOT_EC1, IZENPE)
NSS_NAME_CONSTRAINTS = (TUBITAK_KAMU_SM_SSL_1,)


def fail(text):
    sys.exit('trust-anchors: ' + text)


def element(data, at, end):
    if end - at < 2:
        fail('truncated element')
    tag, size = data[at], data[at + 1]
    if tag & 0x1F == 0x1F:
        fail('multi-byte tag')
    at += 2
    if size & 0x80:
        count = size & 0x7F
        if not 1 <= count <= 4 or end - at < count:
            fail('bad length')
        size = int.from_bytes(data[at:at + count], 'big')
        if size < 0x80 or not data[at]:
            fail('length not in der form')
        at += count
    if size > end - at:
        fail('element runs past its parent')
    return tag, at, at + size


def children(data, at, end, tags=None, what='element'):
    out = []
    while at < end:
        tag, start, stop = element(data, at, end)
        out.append((tag, at, start, stop))
        at = stop
    if tags is not None and [item[0] for item in out] != tags:
        fail(f'{what} holds {[hex(item[0]) for item in out]}')
    return out


def value(data, item):
    return data[item[2]:item[3]]


def oid(data, item):
    body = value(data, item)
    if item[0] != 0x06 or not body or body[-1] & 0x80:
        fail('bad object identifier')
    arcs, number, fresh = [], 0, True
    for byte in body:
        if fresh and byte == 0x80:
            fail('object identifier not in der form')
        number = number << 7 | byte & 0x7F
        fresh = not byte & 0x80
        if fresh:
            arcs.append(number)
            number = 0
    first = min(arcs[0] // 40, 2)
    return '.'.join(str(arc) for arc in [first, arcs[0] - 40 * first] + arcs[1:])


def integer(data, item):
    body = value(data, item)
    if item[0] != 0x02 or not body:
        fail('bad integer')
    if body[0] & 0x80:
        fail('negative integer')
    if len(body) > 1 and not body[0] and not body[1] & 0x80:
        fail('integer not in der form')
    body = body.lstrip(b'\0')
    if not body:
        fail('zero integer')
    return body


def name(data, item):
    common = ''
    for rdn in children(data, item[2], item[3]):
        if rdn[0] != 0x31:
            fail('name element is not a set')
        attributes = children(data, rdn[2], rdn[3])
        if not attributes:
            fail('empty name element')
        for attribute in attributes:
            if attribute[0] != 0x30:
                fail('name attribute is not a sequence')
            pair = children(data, attribute[2], attribute[3])
            if len(pair) != 2:
                fail('name attribute is not a type and a value')
            kind, text = pair
            if oid(data, kind) == '2.5.4.3' and text[0] in STRINGS:
                common = value(data, text).decode(STRINGS[text[0]], 'replace')
    return data[item[1]:item[3]], common


def moment(data, item):
    text = value(data, item).decode('ascii', 'replace')
    if item[0] == 0x17 and re.fullmatch('[0-9]{12}Z', text):
        year = int(text[:2]) + (1900 if int(text[:2]) >= 50 else 2000)
        rest = text[2:]
    elif item[0] == 0x18 and re.fullmatch('[0-9]{14}Z', text):
        year = int(text[:4])
        rest = text[4:]
    else:
        fail('bad time ' + text)
    month, day, hour, minute, second = (int(rest[at:at + 2]) for at in range(0, 10, 2))
    days = calendar.monthrange(year, month)[1] if 1 <= month <= 12 else 0
    if not 1 <= day <= days or hour > 23 or minute > 59 or second > 59:
        fail('bad time ' + text)
    return calendar.timegm((year, month, day, hour, minute, second))


def public_key(data, item):
    algorithm, bits = children(data, item[2], item[3], [0x30, 0x03], 'public key info')
    parts = children(data, algorithm[2], algorithm[3])
    if not 1 <= len(parts) <= 2:
        fail('bad key algorithm')
    kind, key = oid(data, parts[0]), value(data, bits)
    if kind not in (RSA, EC):
        return None, 'key type ' + kind
    if not key or key[0]:
        fail('key bits do not end on a byte')
    key = key[1:]
    if kind == RSA:
        if len(parts) == 2 and data[parts[1][1]:parts[1][3]] != b'\x05\0':
            fail('rsa key parameters are not null')
        tag, start, stop = element(key, 0, len(key))
        if tag != 0x30 or stop != len(key):
            fail('rsa key is not one sequence')
        modulus, exponent = (integer(key, part) for part in children(key, start, stop, [0x02, 0x02], 'rsa key'))
        size = int.from_bytes(modulus, 'big').bit_length()
        if size > RSA_BITS:
            return None, f'{size}-bit rsa key'
        return ('rsa', modulus, exponent), None
    if len(parts) != 2 or parts[1][0] != 0x06:
        return None, 'ec key without a named curve'
    curve = oid(data, parts[1])
    if curve not in CURVES:
        return None, 'ec curve ' + curve
    symbol, size = CURVES[curve]
    if len(key) != 1 + 2 * size or key[0] != 0x04:
        return None, 'ec key not an uncompressed point'
    return ('ec', symbol, key), None


def certificate(der):
    tag, start, stop = element(der, 0, len(der))
    if tag != 0x30 or stop != len(der):
        fail('not one der certificate')
    tbs, _, _ = children(der, start, stop, [0x30, 0x30, 0x03], 'certificate')
    fields = children(der, tbs[2], tbs[3])
    if fields and fields[0][0] == 0xA0:
        version, = children(der, fields[0][2], fields[0][3], [0x02], 'version')
        if value(der, version) not in (b'\0', b'\x01', b'\x02'):
            fail('unknown certificate version')
        fields = fields[1:]
    if [field[0] for field in fields[:6]] != [0x02, 0x30, 0x30, 0x30, 0x30, 0x30]:
        fail('tbs is not serial, algorithm, issuer, validity, subject and key')
    rest = [field[0] for field in fields[6:]]
    if rest != sorted(set(rest)) or not set(rest) <= {0x81, 0x82, 0xA3}:
        fail('unexpected fields after the key')
    for extensions in fields[6:]:
        if extensions[0] == 0xA3:
            outer, = children(der, extensions[2], extensions[3], [0x30], 'extensions')
            for extension in children(der, outer[2], outer[3]):
                parts = children(der, extension[2], extension[3])
                if extension[0] != 0x30 or [part[0] for part in parts] not in ([0x06, 0x04], [0x06, 0x01, 0x04]):
                    fail('bad extension')
                oid(der, parts[0])
    name(der, fields[2])
    validity = children(der, fields[3][2], fields[3][3])
    if len(validity) != 2:
        fail('validity is not two times')
    _, end = (moment(der, item) for item in validity)
    subject, common = name(der, fields[4])
    key, problem = public_key(der, fields[5])
    return subject, common, end, key, problem


def pem_blocks(text):
    blocks = re.findall(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', text, re.S)
    if len(blocks) != text.count('-----BEGIN'):
        fail('a pem block is not a certificate')
    out = []
    for block in blocks:
        try:
            out.append(base64.b64decode(''.join(block.split()), validate=True))
        except binascii.Error:
            fail('bad base64 in a pem block')
    return out


def blob(name, data):
    lines = [f'static const unsigned char {name}[] = {{']
    for at in range(0, len(data), 16):
        lines.append('    ' + ', '.join(f'0x{byte:02x}' for byte in data[at:at + 16]) + ',')
    return lines + ['};']


def main():
    if len(sys.argv) not in (1, 3):
        fail('usage: trust-anchors.py [<cacert.pem> <roots.h>]')
    pem, out = sys.argv[1:3] if len(sys.argv) == 3 else (PEM, OUT)
    with open(pem, encoding='utf-8') as f:
        ders = pem_blocks(f.read())
    lines, table, ends, skipped = ['#pragma once', '', '#include <bearssl.h>'], [], [], []
    for der in ders:
        subject, common, end, key, problem = certificate(der)
        digest = hashlib.sha256(der).hexdigest()
        if digest in SERVER_DISTRUST_AFTER:
            problem = 'distrusted for servers after a date'
        elif digest in NSS_NAME_CONSTRAINTS:
            problem = 'name constrained by nss'
        if problem:
            skipped.append(f'{common or subject.hex()}: {problem}')
            continue
        i = len(ends)
        lines += [''] + blob(f'g_root{i}_dn', subject)
        table.append(f'    {{{{(unsigned char *)g_root{i}_dn, sizeof(g_root{i}_dn)}}, BR_X509_TA_CA,')
        if key[0] == 'rsa':
            lines += blob(f'g_root{i}_n', key[1]) + blob(f'g_root{i}_e', key[2])
            table.append(f'     {{BR_KEYTYPE_RSA, {{.rsa = {{(unsigned char *)g_root{i}_n, sizeof(g_root{i}_n),')
            table.append(f'                               (unsigned char *)g_root{i}_e, sizeof(g_root{i}_e)}}}}}}}},')
        else:
            lines += blob(f'g_root{i}_q', key[2])
            table.append(f'     {{BR_KEYTYPE_EC, {{.ec = {{{key[1]}, (unsigned char *)g_root{i}_q, '
                         f'sizeof(g_root{i}_q)}}}}}}}},')
        ends.append(end)
    if not ends:
        fail('no usable roots')
    lines += ['', 'static const br_x509_trust_anchor g_roots[] = {'] + table + ['};', '',
              'static const unsigned long long g_root_ends[] = {']
    lines += ['    ' + ', '.join(str(end) for end in ends[at:at + 8]) + ',' for at in range(0, len(ends), 8)]
    lines += ['};', '']
    if max(len(line) for line in lines) > 120:
        fail('a line of the header is over 120 columns')
    text = '\n'.join(lines)
    try:
        with open(out, encoding='ascii') as f:
            same = f.read() == text
    except OSError:
        same = False
    if not same:
        with open(out, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    for line in skipped:
        print('skipped', line)
    print(f'{len(ends)} of {len(ders)} roots', 'in' if same else 'written to', out)


if __name__ == '__main__':
    main()
