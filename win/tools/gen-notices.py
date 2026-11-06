"""writes THIRD-PARTY-NOTICES.txt (utf-8 + crlf, win7 notepad is picky)
usage: gen-notices.py <out>"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
ROOT = os.path.dirname(WIN)
SRC = os.environ.get('BLOX7_DEPS_SRC', 'D:/toolchains/src')
AOSP = os.environ.get('BLOX7_AOSP', 'D:/toolchains/aosp')
LIC = os.path.join(WIN, 'licenses')
TP = os.path.join(ROOT, 'third_party')

LLVM = f'{AOSP}/scudo/LICENSE.TXT'

PARTS = [
    ('Mocktail', 'https://github.com/komaruworld/mocktail',
     'The Android runtime Blox7 is built on (blox7.so, its helper programs and Android-side libraries), and '
     'the Windows port around it.', [f'{ROOT}/LICENSE']),
    ('Android bionic', 'https://android.googlesource.com/platform/bionic',
     'The C library, math library and dynamic-linker interface in libc.so.', [f'{AOSP}/bionic/libc/NOTICE']),
    ('Scudo', 'https://llvm.org/docs/ScudoHardenedAllocator.html', 'The memory allocator in libc.so.', [LLVM]),
    ('LLVM libc', 'https://libc.llvm.org', 'String and memory routines in libc.so. Same license as Scudo above.', []),
    ('Arm Optimized Routines', 'https://github.com/ARM-software/optimized-routines', 'Math routines in libc.so.',
     [f'{AOSP}/arm-optimized-routines/LICENSE']),
    ('LLVM libc++ and libc++abi', 'https://libcxx.llvm.org',
     'The C++ standard library (libc++_shared.so), from the Android NDK r28c. Same license as Scudo above.', []),
    ('zlib', 'https://zlib.net', 'Compression (libz.so, and inside blox7.so and the updater).',
     [f'{SRC}/zlib/LICENSE']),
    ('minizip', 'https://github.com/madler/zlib/tree/master/contrib/minizip',
     'ZIP reading for the updater. Under the zlib license above; parts carry the Info-ZIP license below.',
     [f'{SRC}/zlib/contrib/minizip/LICENSE.Info-Zip']),
    ('BoringSSL', 'https://boringssl.googlesource.com/boringssl', 'TLS and cryptography.', [f'{SRC}/boringssl/LICENSE']),
    ('curl', 'https://curl.se', 'HTTP downloads.', [f'{SRC}/curl/COPYING']),
    ('libyaml', 'https://github.com/yaml/libyaml', 'Reading config.yaml.', [f'{SRC}/libyaml/License']),
    ('Capstone', 'https://www.capstone-engine.org', 'Disassembly for the updater\'s compatibility checks.',
     [f'{SRC}/capstone/LICENSE.TXT', f'{SRC}/capstone/LICENSE_LLVM.TXT']),
    ('utf8proc', 'https://juliastrings.github.io/utf8proc/', 'Unicode text handling.', [f'{SRC}/utf8proc/LICENSE.md']),
    ('FreeType', 'https://freetype.org', 'Font rendering (blox7.so and SDL3_ttf.dll), used under the FreeType '
     'License.', [f'{SRC}/freetype/docs/FTL.TXT']),
    ('HarfBuzz', 'https://harfbuzz.github.io', 'Text shaping, inside SDL3_ttf.dll.', [f'{LIC}/harfbuzz.txt']),
    ('JSON for Modern C++', 'https://github.com/nlohmann/json', 'JSON parsing.', [f'{SRC}/json/LICENSE.MIT']),
    ('libpng', 'http://www.libpng.org/pub/png/libpng.html', 'PNG images.', [f'{SRC}/libpng/LICENSE']),
    ('SDL3', 'https://libsdl.org', 'Windows, input, audio and graphics setup (SDL3.dll).',
     [f'{SRC}/sdl3-win/LICENSE.txt']),
    ('SDL3_ttf', 'https://github.com/libsdl-org/SDL_ttf', 'Text rendering (SDL3_ttf.dll).',
     [f'{SRC}/sdl3ttf-win/LICENSE.txt']),
    ('Microsoft WebView2 SDK', 'https://developer.microsoft.com/microsoft-edge/webview2/',
     'WebView2Loader.dll, which starts the web view Roblox\'s sign-in pages run in.',
     [f'{SRC}/webview2-sdk/pkg/LICENSE.txt', f'{SRC}/webview2-sdk/pkg/NOTICE.txt']),
    ('shaderc', 'https://github.com/google/shaderc', 'glslc.exe, which compiles custom effect shaders, from the '
     'Android NDK r28c.', [f'{LIC}/shaderc.txt']),
    ('glslang', 'https://github.com/KhronosGroup/glslang', 'The GLSL front end inside glslc.exe.', [f'{LIC}/glslang.txt']),
    ('SPIRV-Tools', 'https://github.com/KhronosGroup/SPIRV-Tools', 'The SPIR-V optimizer inside glslc.exe.',
     [f'{LIC}/spirv-tools.txt']),
    ('SPIRV-Headers', 'https://github.com/KhronosGroup/SPIRV-Headers', 'SPIR-V definitions inside glslc.exe.',
     [f'{LIC}/spirv-headers.txt']),
    ('mcpelauncher-linker', 'https://github.com/minecraft-linux/mcpelauncher-linker', 'The linker interface '
     'Blox7\'s code is written against.', [f'{TP}/mcpelauncher-linker/LICENSE']),
    ('libjnivm', 'https://github.com/ChristopherHX/libjnivm', 'The Java Native Interface Roblox\'s code talks to.',
     [f'{TP}/libjnivm/LICENSE']),
    ('Vulkan-Headers', 'https://github.com/KhronosGroup/Vulkan-Headers', 'Vulkan definitions.',
     [f'{LIC}/vulkan-headers.txt']),
    ('ANGLE headers', 'https://chromium.googlesource.com/angle/angle', 'EGL and OpenGL ES definitions.',
     [f'{TP}/angle_headers/LICENSE']),
    ('JNI headers (OpenJDK 17u)', 'https://github.com/openjdk/jdk17u', 'jni.h and jni_md.h, under the GPL 2.0 with '
     'the Classpath exception.', [f'{TP}/jni/LICENSE', f'{TP}/jni/ASSEMBLY_EXCEPTION']),
    ('AssetStudio Texture2DDecoderNative', 'https://github.com/Perfare/AssetStudio', 'ETC1 texture decoding in '
     'blox7.so.', [f'{LIC}/assetstudio-texture2ddecoder.txt']),
    ('Mozilla CA certificate list', 'https://curl.se/docs/caextract.html', 'The trusted root certificates '
     '(root/etc/ssl/certs/ca-certificates.crt), from Mozilla\'s certdata.txt as curl publishes it, under the '
     'Mozilla Public License 2.0. Its source is at https://hg.mozilla.org/projects/nss/file/tip/lib/ckfw/builtins/'
     'certdata.txt.', [f'{LIC}/mpl-2.0.txt']),
]

HEADER = """Blox7 third-party notices

Blox7 is made of the software below, each under its own license. The full license texts follow the list.

Blox7 is not affiliated with or endorsed by Roblox Corporation or Microsoft. Roblox and the Roblox logo are
trademarks of Roblox Corporation. Windows is a trademark of Microsoft. Blox7 does not include Roblox: it
downloads Roblox's own Android app from its publisher's releases and verifies it against Roblox's signing
certificate.
"""


def wrap(text, width=110):
    out, line = [], ''
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + ' ' + word).strip()
    if line:
        out.append(line)
    return '\n'.join(out)


def main():
    rule = '=' * 110
    parts = [HEADER, '']
    for i, (name, url, what, _) in enumerate(PARTS, 1):
        parts.append(f'{i:2}. {name} - {url}')
        parts.append(wrap(what, 104).replace('\n', '\n    ').join(['    ', '']))
    parts.append('')
    for i, (name, url, what, files) in enumerate(PARTS, 1):
        if not files:
            continue
        parts.append(rule)
        parts.append(f'{i}. {name}')
        parts.append(rule)
        for path in files:
            if not os.path.exists(path):
                sys.exit('missing license file: ' + path)
            with open(path, encoding='utf-8', errors='replace') as f:
                parts.append(f.read().replace('\r\n', '\n').strip('\n'))
            parts.append('')
    text = '\n'.join(parts).rstrip('\n') + '\n'
    with open(sys.argv[1], 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write(text)
    print('notices:', len(PARTS), 'parts,', os.path.getsize(sys.argv[1]) // 1024, 'KB')


if __name__ == '__main__':
    main()
