"""writes win/studio7/roots.h, the bearssl trust anchors for the roots in exe/cacert.pem, the subjects nss constrains
and the certificates mozilla revoked in onecrl, from the snapshot in win/tools/trust-anchors.txt
usage: trust-anchors.py [<cacert.pem> <trust-anchors.txt> <roots.h>]
       trust-anchors.py refresh <certdata.txt> <genname.c> <onecrl.json> [<cacert.pem> <trust-anchors.txt> <roots.h>]
refresh takes the certdata.txt whose sha-256 is on cacert.pem's "## SHA256:" line, nss's lib/certdb/genname.c and
onecrl.json from https://firefox.settings.services.mozilla.com/v1/buckets/security-state/collections/onecrl/
changeset?_expected=0 (or that collection's /records), and writes the snapshot, then roots.h"""
import base64
import binascii
import calendar
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
PEM = os.path.join(os.path.dirname(WIN), 'exe', 'cacert.pem')
SNAPSHOT = os.path.join(HERE, 'trust-anchors.txt')
OUT = os.path.join(WIN, 'studio7', 'roots.h')
REFRESH = 'trust-anchors.py refresh <certdata.txt> <genname.c> <onecrl.json>'
USAGE = ('usage: trust-anchors.py [<cacert.pem> <trust-anchors.txt> <roots.h>]\n'
         f'       {REFRESH} [<cacert.pem> <trust-anchors.txt> <roots.h>]')
RSA = '1.2.840.113549.1.1.1'
EC = '1.2.840.10045.2.1'
CURVES = {'1.2.840.10045.3.1.7': ('BR_EC_secp256r1', 32), '1.3.132.0.34': ('BR_EC_secp384r1', 48),
          '1.3.132.0.35': ('BR_EC_secp521r1', 66)}
RSA_BITS = 4096
BASIC_CONSTRAINTS = '2.5.29.19'
KEY_USAGE = '2.5.29.15'
LIMITS = {'2.5.29.30': 'name constraints', '2.5.29.36': 'policy constraints', '2.5.29.37': 'extended key usage',
          '2.5.29.54': 'inhibit any policy'}
STRINGS = {0x0C: 'utf-8', 0x13: 'ascii', 0x16: 'ascii', 0x14: 'latin-1', 0x1E: 'utf-16-be', 0x1C: 'utf-32-be'}
ESCAPES = {'n': 10, 't': 9, 'r': 13, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '\\': 92, '"': 34, "'": 39, '?': 63}
DIGEST = '[0-9a-f]{64}'
WORD = '[0-9A-Za-z_-]+'


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


def limit(der, parts):
    kind = oid(der, parts[0])
    if len(parts) == 3 and value(der, parts[1]) not in (b'\0', b'\xff'):
        fail('bad critical flag')
    if kind in LIMITS:
        return LIMITS[kind]
    if kind == BASIC_CONSTRAINTS:
        inner, = children(der, parts[-1][2], parts[-1][3], [0x30], 'basic constraints')
        flags = [item[0] for item in children(der, inner[2], inner[3])]
        if flags not in ([], [0x01], [0x02], [0x01, 0x02]):
            fail('bad basic constraints')
        return 'path length limit' if 0x02 in flags else None
    if kind == KEY_USAGE:
        inner, = children(der, parts[-1][2], parts[-1][3], [0x03], 'key usage')
        bits = value(der, inner)
        if not bits or bits[0] > 7:
            fail('bad key usage')
        return None if len(bits) > 1 and bits[1] & 0x04 else 'key usage without certificate signing'
    return 'critical extension ' + kind if len(parts) == 3 and value(der, parts[1]) != b'\0' else None


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
    limits = []
    for extensions in fields[6:]:
        if extensions[0] == 0xA3:
            outer, = children(der, extensions[2], extensions[3], [0x30], 'extensions')
            for extension in children(der, outer[2], outer[3]):
                parts = children(der, extension[2], extension[3])
                if extension[0] != 0x30 or [part[0] for part in parts] not in ([0x06, 0x04], [0x06, 0x01, 0x04]):
                    fail('bad extension')
                reason = limit(der, parts)
                if reason:
                    limits.append(reason)
    issuer, _ = name(der, fields[2])
    validity = children(der, fields[3][2], fields[3][3])
    if len(validity) != 2:
        fail('validity is not two times')
    _, end = (moment(der, item) for item in validity)
    subject, common = name(der, fields[4])
    key, problem = public_key(der, fields[5])
    spki = der[fields[5][1]:fields[5][3]]
    ids = serial_id(issuer, value(der, fields[0])), key_id(subject, hashlib.sha256(spki).digest())
    return subject, common, end, key, problem or ', '.join(limits), ids


def serial_id(issuer, serial):
    return hashlib.sha256(issuer + serial.lstrip(b'\0')).digest()


def key_id(subject, key_hash):
    return hashlib.sha256(subject + key_hash).digest()


def b64(text, what):
    try:
        return base64.b64decode(text, validate=True)
    except (binascii.Error, TypeError, ValueError):
        fail('bad base64 in ' + what)


def one_name(data, what):
    tag, _, stop = element(data, 0, len(data))
    if tag != 0x30 or stop != len(data):
        fail(what + ' is not one der name')
    return data


def onecrl_lines(path):
    try:
        with open(path, encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, ValueError):
        fail('cannot read the onecrl records in ' + path)
    records = data.get('changes', data.get('data')) if isinstance(data, dict) else None
    if not isinstance(records, list) or not records:
        fail(path + ' holds no onecrl records')
    lines, newest = set(), 0
    for record in records:
        if (not isinstance(record, dict) or record.get('deleted') or not isinstance(record.get('last_modified'), int)
                or not isinstance(record.get('id'), str) or not re.fullmatch(WORD, record['id'])):
            fail(f'{path} holds a record that is not a live onecrl entry')
        found = False
        if 'issuerName' in record and 'serialNumber' in record:
            issuer = one_name(b64(record['issuerName'], 'an issuerName'), 'an issuerName')
            serial = b64(record['serialNumber'], 'a serialNumber')
            if not serial:
                fail('an empty serialNumber')
            lines.add(f'revoked-serial {serial_id(issuer, serial).hex()} {record["id"]}')
            found = True
        if 'subject' in record and 'pubKeyHash' in record:
            subject = one_name(b64(record['subject'], 'a subject'), 'a subject')
            key_hash = b64(record['pubKeyHash'], 'a pubKeyHash')
            if len(key_hash) != 32:
                fail('a pubKeyHash is not 32 bytes')
            lines.add(f'revoked-key {key_id(subject, key_hash).hex()} {record["id"]}')
            found = True
        if not found:
            fail(f'{path} holds a record without issuerName and serialNumber or subject and pubKeyHash')
        newest = max(newest, record['last_modified'])
    return lines | {f'onecrl {newest}'}


def certdata_objects(raw, path):
    lines, objects = [line.rstrip('\r') for line in raw.decode('utf-8').split('\n')], []
    if 'BEGINDATA' not in lines:
        fail(path + ' is not a certdata.txt')
    at = lines.index('BEGINDATA') + 1
    while at < len(lines):
        line = lines[at]
        at += 1
        if not line.strip() or line.startswith('#'):
            continue
        words = line.split(' ', 2)
        if len(words) == 2 and words[1] == 'MULTILINE_OCTAL':
            data = b''
            while at < len(lines) and lines[at] != 'END':
                if not re.fullmatch(r'(\\[0-7]{3})+', lines[at]):
                    fail(f'{path} line {at + 1} is not octal')
                data += bytes(int(digits, 8) for digits in re.findall(r'\\([0-7]{3})', lines[at]))
                at += 1
            if at == len(lines):
                fail(path + ' ends inside a value')
            at += 1
            words.append(data)
        if len(words) != 3:
            fail(f'{path} line {at} is not an attribute')
        if words[0] == 'CKA_CLASS':
            objects.append({})
        if not objects or words[0] in objects[-1]:
            fail(f'{path} line {at} is outside an object or repeats an attribute')
        objects[-1][words[0]] = (words[1], words[2])
    return objects


def certdata_lines(path, stamp, contents, ders):
    with open(path, 'rb') as f:
        raw = f.read()
    if hashlib.sha256(raw).hexdigest() != stamp:
        fail(f'{path} is not the certdata.txt cacert.pem was made from (its "## SHA256:" line says {stamp})')
    certificates, trusts, lines = {}, {}, {f'certdata {stamp}', f'certificates {contents}'}
    for item in certdata_objects(raw, path):
        kind = item.get('CKA_CLASS')
        if kind == ('CK_OBJECT_CLASS', 'CKO_CERTIFICATE'):
            value = item.get('CKA_VALUE')
            if not value or value[0] != 'MULTILINE_OCTAL':
                fail(path + ' holds a certificate without its value')
            certificates[hashlib.sha256(value[1]).hexdigest()] = item.get('CKA_NSS_SERVER_DISTRUST_AFTER',
                                                                         ('CK_BBOOL', 'CK_FALSE'))
        elif kind == ('CK_OBJECT_CLASS', 'CKO_NSS_TRUST'):
            sha1 = item.get('CKA_CERT_SHA1_HASH')
            if not sha1 or sha1[0] != 'MULTILINE_OCTAL' or sha1[1] in trusts:
                fail(path + ' holds a trust object without its own CKA_CERT_SHA1_HASH')
            trusts[sha1[1]] = item.get('CKA_TRUST_SERVER_AUTH', ('', 'none'))
    for digest, distrust in certificates.items():
        if distrust != ('CK_BBOOL', 'CK_FALSE'):
            when = distrust[1].decode('ascii', 'replace') if distrust[0] == 'MULTILINE_OCTAL' else distrust[1]
            if not re.fullmatch(WORD, when):
                fail(f'{path} holds a CKA_NSS_SERVER_DISTRUST_AFTER of {when!r}')
            lines.add(f'left-out {digest} server-distrust-after {when}')
    for der in ders:
        digest, trust = hashlib.sha256(der).hexdigest(), trusts.get(hashlib.sha1(der).digest(), ('', 'none'))
        if digest not in certificates:
            fail(f'cacert.pem holds a certificate {path} does not ({digest})')
        if trust != ('CK_TRUST', 'CKT_NSS_TRUSTED_DELEGATOR'):
            lines.add(f'left-out {digest} server-trust {trust[1] if re.fullmatch(WORD, trust[1]) else "other"}')
    return lines


def c_bytes(text, what):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    literal = r'"((?:[^"\\\n]|\\.)*)"'
    if re.sub(literal, ' ', text).strip():
        fail(what + ' is not only string literals')
    out = bytearray()
    for body in re.findall(literal, text):
        at = 0
        while at < len(body):
            if body[at] != '\\':
                if not ' ' <= body[at] <= '~':
                    fail(what + ' holds a character that is not ascii')
                out.append(ord(body[at]))
                at += 1
                continue
            escape = re.match(r'x([0-9A-Fa-f]+)|([0-7]{1,3})|(.)', body[at + 1:])
            if escape.group(1) or escape.group(2):
                number = int(escape.group(1), 16) if escape.group(1) else int(escape.group(2), 8)
                if number > 0xFF:
                    fail(what + ' holds an escape over 0xff')
                out.append(number)
            elif escape.group(3) in ESCAPES:
                out.append(ESCAPES[escape.group(3)])
            else:
                fail(what + ' holds an unknown escape')
            at += 1 + len(escape.group(0))
    return bytes(out)


def genname_lines(path):
    with open(path, encoding='utf-8') as f:
        text = re.sub(r'\\\n', ' ', f.read().replace('\r\n', '\n'))
    table = re.findall(r'builtInNameConstraints\[\]\[2\] = \{(.*?)\};', text, re.S)
    if len(table) != 1:
        fail(path + ' has no builtInNameConstraints table')
    labels = re.findall(r'NAME_CONSTRAINTS_ENTRY\((\w+)\)', table[0])
    if not labels or re.sub(r'NAME_CONSTRAINTS_ENTRY\(\w+\)|[\s,]', '', table[0]):
        fail(path + ' has an unexpected builtInNameConstraints table')
    lines = set()
    for label in labels:
        body = re.findall(rf'^[ \t]*#[ \t]*define[ \t]+{label}_SUBJECT_DN\b(.*)$', text, re.M)
        if len(body) != 1:
            fail(f'{path} does not define {label}_SUBJECT_DN once')
        subject = one_name(c_bytes(body[0], f'{label}_SUBJECT_DN'), f'{label}_SUBJECT_DN')
        lines.add(f'constrained {hashlib.sha256(subject).hexdigest()} {label}')
    return lines


def bundle(pem):
    with open(pem, encoding='utf-8') as f:
        text = f.read()
    stamp = re.findall(r'^## SHA256: ([0-9a-f]{64})$', text, re.M)
    if len(stamp) != 1:
        fail(pem + ' has no "## SHA256:" line naming its certdata.txt')
    ders = pem_blocks(text)
    return stamp[0], hashlib.sha256(b''.join(ders)).hexdigest(), ders


def snapshot(lines, path):
    if lines != sorted(set(lines)):
        fail(path + ' is not sorted or holds a line twice')
    single, left_out = {'certdata': [], 'certificates': [], 'onecrl': []}, {}
    constrained, serials, keys = set(), set(), set()
    for line in lines:
        kind, *rest = line.split(' ')
        if kind in single and len(rest) == 1 and re.fullmatch('[0-9]+' if kind == 'onecrl' else DIGEST, rest[0]):
            single[kind].append(rest[0])
        elif (kind == 'left-out' and len(rest) == 3 and re.fullmatch(DIGEST, rest[0])
              and rest[1] in ('server-distrust-after', 'server-trust') and re.fullmatch(WORD, rest[2])):
            left_out[rest[0]] = f'{rest[1]} {rest[2]}'
        elif kind == 'constrained' and len(rest) == 2 and re.fullmatch(DIGEST, rest[0]) and re.fullmatch(WORD, rest[1]):
            constrained.add(bytes.fromhex(rest[0]))
        elif (kind in ('revoked-serial', 'revoked-key') and len(rest) == 2 and re.fullmatch(DIGEST, rest[0])
              and re.fullmatch(WORD, rest[1])):
            (serials if kind == 'revoked-serial' else keys).add(bytes.fromhex(rest[0]))
        else:
            fail(f'{path} holds an unknown line: {line[:80]}')
    if any(len(found) != 1 for found in single.values()) or not constrained or not serials or not keys:
        fail(f'{path} is not a whole snapshot, refresh it: {REFRESH}')
    return single['certdata'][0], single['certificates'][0], left_out, constrained, serials, keys


def save(path, text, encoding):
    try:
        with open(path, encoding=encoding) as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    with open(path, 'w', encoding=encoding, newline='\n') as f:
        f.write(text)
    return True


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


def digests(name, values):
    lines = [f'static const unsigned char {name}[][32] = {{']
    for digest in sorted(values):
        lines.append('    {' + ', '.join(f'0x{byte:02x}' for byte in digest[:16]) + ',')
        lines.append('     ' + ', '.join(f'0x{byte:02x}' for byte in digest[16:]) + '},')
    return lines + ['};']


def main():
    sys.stdout.reconfigure(errors='backslashreplace')
    args = sys.argv[1:]
    refresh = args[:1] == ['refresh']
    if refresh and len(args) in (4, 7):
        pem, path, out = args[4:] or (PEM, SNAPSHOT, OUT)
    elif not refresh and len(args) in (0, 3):
        pem, path, out = args or (PEM, SNAPSHOT, OUT)
    else:
        fail(USAGE)
    stamp, contents, ders = bundle(pem)
    if refresh:
        records = sorted(certdata_lines(args[1], stamp, contents, ders) | genname_lines(args[2]) |
                         onecrl_lines(args[3]))
    else:
        try:
            with open(path, encoding='utf-8') as f:
                records = f.read().splitlines()
        except OSError:
            fail(f'cannot read {path}, make it with: {REFRESH}')
    made_from, certificates, left_out, constrained, serials, keys = snapshot(records, path)
    if (made_from, certificates) != (stamp, contents):
        fail(f'{pem} is not the cacert.pem {path} was made from, refresh the snapshot with: {REFRESH}')
    lines, table, ends, skipped = ['#pragma once', '', '#include <bearssl.h>'], [], [], []
    for der in ders:
        subject, common, end, key, problem, ids = certificate(der)
        digest = hashlib.sha256(der).hexdigest()
        if digest in left_out:
            problem = 'left out by certdata.txt, ' + left_out[digest]
        elif hashlib.sha256(subject).digest() in constrained:
            problem = 'name constrained by nss'
        elif ids[0] in serials or ids[1] in keys:
            problem = 'revoked in onecrl'
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
    lines += ['};', ''] + digests('g_constrained', constrained) + [''] + digests('g_revoked_serials', serials)
    lines += [''] + digests('g_revoked_keys', keys) + ['']
    if max(len(line) for line in lines + records) > 120:
        fail('a line of the header or the snapshot is over 120 columns')
    if refresh:
        print(f'{len(records)} lines', 'written to' if save(path, '\n'.join(records) + '\n', 'utf-8') else 'in', path)
    same = not save(out, '\n'.join(lines), 'ascii')
    for line in skipped:
        print('skipped', line)
    print(f'{len(ends)} of {len(ders)} roots, {len(constrained)} subjects nss constrains and '
          f'{len(serials) + len(keys)} onecrl revocations', 'in' if same else 'written to', out)


if __name__ == '__main__':
    main()
