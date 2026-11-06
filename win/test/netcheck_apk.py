# netcheck as an android app: a debuggable api 35 apk around the arm64 build, so adb shell run-as gives the
# answers roblox gets (app uid and selinux), not the shell's
#   python -I netcheck_apk.py <netcheck_arm64> <out.apk> <work dir> <openssl>
import hashlib, os, struct, subprocess, sys, zipfile

exe, out_apk, work, openssl = sys.argv[1:5]
os.makedirs(work, exist_ok=True)

ATTRS = [
    ('label', 0x01010001), ('name', 0x01010003), ('hasCode', 0x0101000c), ('debuggable', 0x0101000f),
    ('minSdkVersion', 0x0101020c), ('versionCode', 0x0101021b), ('versionName', 0x0101021c),
    ('targetSdkVersion', 0x01010270), ('extractNativeLibs', 0x010104ea),
]
ANDROID_NS = 'http://schemas.android.com/apk/res/android'
strings = [a for a, _ in ATTRS] + ['android', ANDROID_NS, 'package', 'com.blox7.netcheck', '1', 'manifest', 'uses-sdk',
                                    'uses-permission', 'android.permission.INTERNET', 'application', 'netcheck']
S = {s: i for i, s in enumerate(strings)}
RID = dict(ATTRS)
TYPE_STRING, TYPE_INT_DEC, TYPE_BOOL = 0x03, 0x10, 0x12


def chunk(ctype, header_rest, body, header_size):
    size = header_size + len(body)
    return struct.pack('<HHI', ctype, header_size, size) + header_rest + body


def string_pool():
    data = b''
    offsets = []
    for s in strings:
        offsets.append(len(data))
        u = s.encode('utf-16-le')
        data += struct.pack('<H', len(s)) + u + b'\0\0'
    while len(data) % 4:
        data += b'\0'
    header_size = 28
    strings_start = header_size + 4 * len(strings)
    rest = struct.pack('<IIIII', len(strings), 0, 0, strings_start, 0)
    return chunk(0x0001, rest, b''.join(struct.pack('<I', o) for o in offsets) + data, header_size)


def attr(name, kind, value, ns=True):
    nsi = S[ANDROID_NS] if ns else 0xffffffff
    if kind == TYPE_STRING:
        raw, data = S[value], S[value]
    elif kind == TYPE_BOOL:
        raw, data = 0xffffffff, 0xffffffff if value else 0
    else:
        raw, data = 0xffffffff, value
    return (RID.get(name, 0) if ns else 0, struct.pack('<IIIHBBI', nsi, S[name], raw, 8, 0, kind, data))


def start(name, attrs):
    attrs = [a for _, a in sorted(attrs, key=lambda t: t[0])]  # aapt's order: resource id, unmapped first
    ext = struct.pack('<IIHHHHHH', 0xffffffff, S[name], 20, 20, len(attrs), 0, 0, 0) + b''.join(attrs)
    return chunk(0x0102, struct.pack('<II', 1, 0xffffffff), ext, 16)


def end(name):
    return chunk(0x0103, struct.pack('<II', 1, 0xffffffff), struct.pack('<II', 0xffffffff, S[name]), 16)


ns_ext = struct.pack('<II', S['android'], S[ANDROID_NS])
body = (string_pool() +
        chunk(0x0180, b'', b''.join(struct.pack('<I', r) for _, r in ATTRS), 8) +
        chunk(0x0100, struct.pack('<II', 1, 0xffffffff), ns_ext, 16) +
        start('manifest', [attr('package', TYPE_STRING, 'com.blox7.netcheck', ns=False), attr('versionCode', TYPE_INT_DEC, 1),
                           attr('versionName', TYPE_STRING, '1')]) +
        start('uses-sdk', [attr('minSdkVersion', TYPE_INT_DEC, 28), attr('targetSdkVersion', TYPE_INT_DEC, 35)]) + end('uses-sdk') +
        start('uses-permission', [attr('name', TYPE_STRING, 'android.permission.INTERNET')]) + end('uses-permission') +
        start('application', [attr('label', TYPE_STRING, 'netcheck'), attr('hasCode', TYPE_BOOL, False),
                              attr('debuggable', TYPE_BOOL, True), attr('extractNativeLibs', TYPE_BOOL, True)]) + end('application') +
        end('manifest') +
        chunk(0x0101, struct.pack('<II', 1, 0xffffffff), ns_ext, 16))
manifest = struct.pack('<HHI', 0x0003, 8, 8 + len(body)) + body

unsigned = os.path.join(work, 'unsigned.apk')
with zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('AndroidManifest.xml', manifest)
    z.write(exe, 'lib/arm64-v8a/libnetcheck.so')
apk = open(unsigned, 'rb').read()
eocd = apk.rfind(b'PK\x05\x06')
cd_size, cd_offset = struct.unpack('<II', apk[eocd + 12:eocd + 20])
entries, cd, tail = apk[:cd_offset], apk[cd_offset:cd_offset + cd_size], apk[eocd:]

# apk signature scheme v2 by hand, openssl does the rsa
key, cert, pub = (os.path.join(work, n) for n in ('key.pem', 'cert.der', 'pub.der'))
if not os.path.exists(key):
    subprocess.check_call([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-outform', 'DER', '-out', cert,
                           '-days', '30', '-subj', '/CN=blox7 netcheck'], stderr=subprocess.DEVNULL)
    subprocess.check_call([openssl, 'rsa', '-in', key, '-pubout', '-outform', 'DER', '-out', pub], stderr=subprocess.DEVNULL)


def lp(b):
    return struct.pack('<I', len(b)) + b


def top_digest(sections):
    digests = []
    for sec in sections:
        for i in range(0, len(sec), 1 << 20):
            c = sec[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


ALG = 0x0103  # rsa pkcs1 sha256
digest = top_digest([entries, cd, tail])
signed_data = lp(lp(struct.pack('<I', ALG) + lp(digest))) + lp(lp(open(cert, 'rb').read())) + lp(b'')
sd_path, sig_path = os.path.join(work, 'signed_data.bin'), os.path.join(work, 'sig.bin')
open(sd_path, 'wb').write(signed_data)
subprocess.check_call([openssl, 'dgst', '-sha256', '-sign', key, '-out', sig_path, sd_path])
signer = lp(signed_data) + lp(lp(struct.pack('<I', ALG) + lp(open(sig_path, 'rb').read()))) + lp(open(pub, 'rb').read())
value = lp(lp(signer))
pair = struct.pack('<QI', 4 + len(value), 0x7109871a) + value
size = len(pair) + 8 + 16
block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
tail = tail[:16] + struct.pack('<I', len(entries) + len(block)) + tail[20:]
open(out_apk, 'wb').write(entries + block + cd + tail)
print('apk', out_apk, len(entries + block + cd + tail), 'bytes')
