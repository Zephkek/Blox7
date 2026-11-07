"""build.py libc | boot | bridges | bridge-sources | webview | test | blox7 [--clean]"""
import hashlib, json, os, re, shlex, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.dirname(HERE)
OUT = os.path.join(WIN, 'out')
AOSP = os.environ.get('BLOX7_AOSP', 'D:/toolchains/aosp')
NDK = os.environ.get('BLOX7_NDK', 'D:/toolchains/android-ndk-r28c')
MINGW = os.environ.get('BLOX7_MINGW', 'D:/toolchains/llvm-mingw-20261020-msvcrt-x86_64')
LLVM = f'{NDK}/toolchains/llvm/prebuilt/windows-x86_64'
CLANG = f'{LLVM}/bin/clang.exe'
CLANGXX = f'{LLVM}/bin/clang++.exe'
VULKAN_HEADERS = os.path.join(os.path.dirname(WIN), 'third_party', 'Vulkan-Headers')
BIONIC = f'{AOSP}/bionic'
sys.path.insert(0, HERE)
import bp  # noqa: E402


_RESOURCE_DIR = []


def resource_dir():
    if not _RESOURCE_DIR:
        _RESOURCE_DIR.append(norm(subprocess.check_output([CLANG, '-print-resource-dir'], text=True).strip()))
    return _RESOURCE_DIR[0]


def norm(p):
    return os.path.normpath(p).replace('\\', '/')

LIBC_REPLACED = {
    'arch-x86_64/bionic/__bionic_clone.S',
    'arch-x86_64/bionic/_exit_with_stack_teardown.S',
    'arch-x86_64/bionic/__restore_rt.S',
    'arch-x86_64/bionic/syscall.S',
    'arch-x86_64/bionic/vfork.S',
    'bionic/vdso.cpp',
    'bionic/dl_iterate_phdr_static.cpp',
    'bionic/libc_init_static.cpp',
    'bionic/gwp_asan_wrappers.cpp',
    'bionic/spawn.cpp',
}
LIBC_INCLUDES = [
    norm(os.path.join(WIN, 'libc', 'override')),
    f'{BIONIC}/libc/include', f'{BIONIC}/libc/kernel/uapi', f'{BIONIC}/libc/kernel/uapi/asm-x86',
    f'{BIONIC}/libc/kernel/android/uapi', f'{BIONIC}/libc', f'{BIONIC}/libc/async_safe/include',
    f'{BIONIC}/libc/platform', f'{AOSP}/core/libcutils/include', f'{BIONIC}/libc/stdio',
    f'{BIONIC}/libm/upstream-freebsd/lib/msun/src', os.path.join(OUT, 'include'), f'{AOSP}/gwp_asan',
    f'{BIONIC}/libc/system_properties/include', f'{AOSP}/core/property_service/libpropertyinfoparser/include',
]
COMMON = ['--target=x86_64-linux-android36', '-fPIC', '-O2', '-g', '-ffunction-sections', '-fdata-sections',
          '-fno-omit-frame-pointer', '-fno-stack-protector', '-mssse3', '-msse4.2', '-mpopcnt',
          '-D__ANDROID__', '-DANDROID', '-D__W32__=1', '-Wno-unused-command-line-argument',
          '-D_FORTIFY_SOURCE=2']


def libc_sources():
    mods = bp.load([f'{BIONIC}/libc/Android.bp', f'{BIONIC}/libm/Android.bp', f'{BIONIC}/libc/async_safe/Android.bp',
                    f'{BIONIC}/libc/system_properties/Android.bp', f'{BIONIC}/libdl/Android.bp',
                    f'{AOSP}/llvm-libc/Android.bp', f'{AOSP}/arm-optimized-routines/Android.bp'])
    libc = bp.collect('libc', mods, static=True)
    libm = bp.collect('libm', mods, static=True)
    extra = [bp.collect(name, mods, static=True) for name in ('libasync_safe', 'libsystemproperties')]
    units = []
    seen = set()
    for r in [libc, libm] + extra:
        for s, module, mflags in r['units']:
            if s.startswith(':'):
                continue
            rel = norm(os.path.relpath(s, f'{BIONIC}/libc'))
            if rel in LIBC_REPLACED or 'icu4x' in rel or (s, module) in seen:
                continue
            seen.add((s, module))
            units.append((module, norm(s), mflags))
    sysprops = next(u for u in units if u[0] == 'libsystemproperties')
    units.append(('libpropertyinfoparser', norm(f'{AOSP}/core/property_service/libpropertyinfoparser/property_info_parser.cpp'),
                  sysprops[2]))
    return units, libc, libm


def split_flags(flags):
    out = []
    for f in flags:
        out += shlex.split(f) if ' ' in f else [f]
    return out


def libc_flags(module, src, mflags):
    flags = list(COMMON) + ['-nostdinc']
    for d in LIBC_INCLUDES:
        flags += ['-isystem' if 'include' in d or 'kernel' in d else '-I', d]
    flags += ['-isystem', resource_dir() + '/include']
    if src.endswith(('.cpp', '.cc')):
        flags = ['-std=gnu++20', '-fno-exceptions', '-fno-rtti', '-nostdinc++'] + flags
    elif src.endswith('.c'):
        flags = ['-std=gnu11'] + flags
    r = mflags
    extra = [] if src.endswith('.S') else [f for f in split_flags(r['cflags'])
                                           if not f.startswith('-Werror') and f not in ('-Wexit-time-destructors',)]
    if src.endswith(('.cpp', '.cc')):
        extra += split_flags(r['cppflags'])
    elif src.endswith('.c'):
        extra += [f for f in split_flags(r['conlyflags']) if not f.startswith('-std=')]
    else:
        extra += split_flags(r['asflags']) + ['-D__ASSEMBLY__']
    for d in r['local_include_dirs'] + [r.get('module_dir', '')]:
        if d:
            flags += ['-I', norm(d)]
    for d in r['include_dirs']:
        if d.startswith(('bionic/', 'system/core/', 'external/')):
            flags += ['-I', norm(f'{AOSP}/' + d.replace('system/core/', 'core/').replace('external/', ''))]
    flags += ['-I', os.path.dirname(src)]
    return flags + extra + ['-Wno-error', '-w']


def scudo_units():
    d = json.load(open(os.path.join(OUT, 'scudo.json'))) if os.path.exists(os.path.join(OUT, 'scudo.json')) else None
    if d is None:
        mods = bp.load([f'{AOSP}/scudo/Android.bp'])
        d = bp.collect('libscudo', mods, static=True)
        os.makedirs(OUT, exist_ok=True)
        json.dump(d, open(os.path.join(OUT, 'scudo.json'), 'w'))
    return [norm(s) for s in d['srcs']], d


def scudo_flags(d):
    flags = ['-std=gnu++17', '-nostdinc'] + COMMON
    for inc in LIBC_INCLUDES:
        flags += ['-isystem', inc]
    flags += ['-isystem', resource_dir() + '/include']
    flags += ['-I', f'{AOSP}/scudo/standalone/include', '-I', f'{AOSP}/scudo/standalone', '-I', f'{AOSP}/scudo/config']
    flags += [f for f in d['cflags'] if not f.startswith('-Werror')] + d['cppflags'] + ['-w']
    return flags


def win_units():
    out = []
    for root in ('libc',):
        for dirpath, _, files in os.walk(os.path.join(WIN, root)):
            for f in files:
                if f.endswith(('.c', '.cpp', '.S')):
                    out.append(norm(os.path.join(dirpath, f)))
    return sorted(out)


def win_flags(src, libc):
    bionic_unit = next(u for u in libc['units'] if u[1] == 'libc_bionic')
    flags = libc_flags('libc_bionic', f'{BIONIC}/libc/bionic/x.cpp' if src.endswith('.cpp') else src, bionic_unit[2])
    flags = [f for f in flags if f != '-w'] + ['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                                               '-I', f'{BIONIC}/libc/bionic', '-I', os.path.join(WIN, 'libc'),
                                               '-I', os.path.join(WIN, 'libc', 'gen')]
    if src.endswith('.c'):
        flags = [f for f in flags if f not in ('-std=gnu++20', '-fno-exceptions', '-fno-rtti', '-nostdinc++')]
        flags = ['-std=gnu11'] + flags
    return flags


def compile_all(jobs):
    """jobs are (src, obj, flags), gives back the objects or stops on the first errors"""
    def up_to_date(src, obj, deps):
        if not os.path.exists(obj) or not os.path.exists(deps):
            return False
        t = os.path.getmtime(obj)
        text = open(deps).read().replace('\\\n', ' ')
        if ': ' not in text:
            return False
        files = [f.replace('\\ ', ' ') for f in re.split(r'(?<!\\)\s+', text.split(': ', 1)[1]) if f]
        for f in files:
            try:
                if os.path.getmtime(f) > t:
                    return False
            except OSError:
                return False
        return True

    def one(job):
        src, obj, flags = job
        stamp = obj + '.flags'
        deps = obj + '.d'
        key = hashlib.sha1(' '.join(flags).encode()).hexdigest()
        if up_to_date(src, obj, deps) and os.path.exists(stamp) and open(stamp).read() == key:
            return None
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        cc = CLANGXX if src.endswith(('.cpp', '.cc')) else CLANG
        p = subprocess.run([cc, '-c', src, '-o', obj, '-MMD', '-MF', deps] + flags, capture_output=True, text=True)
        if p.returncode:
            return f'--- {src}\n{p.stderr[-6000:]}'
        open(stamp, 'w').write(key)
        if p.stderr.strip() and '-w' not in flags:
            return f'(warnings) {src}\n{p.stderr[-3000:]}'
        return None
    errors = []
    with ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        for msg in ex.map(one, jobs):
            if msg:
                errors.append(msg)
    hard = [e for e in errors if not e.startswith('(warnings)')]
    for e in errors:
        print(e)
    if hard:
        print(f'{len(hard)} of {len(jobs)} files failed')
        sys.exit(1)
    return [j[1] for j in jobs]


def obj_path(src, kind):
    rel = src.replace(':', '').replace('/', '_')
    return norm(os.path.join(OUT, 'obj', kind, hashlib.sha1((src + kind).encode()).hexdigest()[:8] + '_' + os.path.basename(src) + '.o'))


def port_headers():
    inc = os.path.join(OUT, 'include', 'android')
    os.makedirs(inc, exist_ok=True)
    for h in ('log.h',):
        shutil.copyfile(f'{LLVM}/sysroot/usr/include/android/{h}', os.path.join(inc, h))
    ids = []
    for line in open(f'{AOSP}/core/libcutils/include/private/android_filesystem_config.h'):
        m = re.match(r'#define AID_(\w+)\s+(\d+)', line)
        if m and not m.group(1).endswith(('_START', '_END')) and m.group(1) not in ('APP', 'USER'):
            ids.append((m.group(1).lower(), int(m.group(2))))
    with open(os.path.join(OUT, 'include', 'generated_android_ids.h'), 'w', newline='\n') as h:
        h.write('// Generated by win/tools/build.py from android_filesystem_config.h.\n#pragma once\n')
        h.write('#include <private/android_filesystem_config.h>\n\n')
        h.write('struct android_id_info {\n  const char name[17];\n  unsigned aid;\n};\n\n')
        h.write('static const struct android_id_info android_ids[] = {\n')
        for name, aid in ids:
            h.write(f'    {{"{name}", {aid}}},\n')
        h.write('};\n\n#define android_id_count (sizeof(android_ids) / sizeof(android_ids[0]))\n')


def build_libc():
    port_headers()
    gen = os.path.join(WIN, 'libc', 'gen')
    os.makedirs(gen, exist_ok=True)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'gen-syscalls.py'), f'{BIONIC}/libc/SYSCALLS.TXT',
                           f'{BIONIC}/libc/kernel/uapi/asm-x86/asm/unistd_64.h',
                           os.path.join(gen, 'w32_syscalls.c'), os.path.join(gen, 'w32_syscalls.h')])
    units, libc, libm = libc_sources()
    jobs = [(src, obj_path(src, module), libc_flags(module, src, mflags)) for module, src, mflags in units]
    ssrcs, sd = scudo_units()
    jobs += [(s, obj_path(s, 'scudo'), scudo_flags(sd)) for s in ssrcs]
    jobs += [(s, obj_path(s, 'win'), win_flags(s, libc)) for s in win_units()]
    only = [a for a in sys.argv[2:] if not a.startswith('--')]
    if only:
        jobs = [j for j in jobs if any(o in j[0] for o in only)]
    objs = compile_all(jobs)
    if only:
        return
    version_script = os.path.join(OUT, 'libc.map')
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'gen-version-script.py'), BIONIC, version_script])
    builtins = f'{resource_dir()}/lib/linux/libclang_rt.builtins-x86_64-android.a'
    unwind = f'{resource_dir()}/lib/linux/x86_64/libunwind.a'
    rsp = os.path.join(OUT, 'libc.rsp')
    open(rsp, 'w').write('\n'.join(f'"{o}"' for o in objs))
    cmd = [CLANGXX, '--target=x86_64-linux-android36', '-shared', '-nostdlib', '-fuse-ld=lld', '-o', os.path.join(OUT, 'libc.so'),
           '-Wl,-soname,libc.so', '-Wl,-Bsymbolic', '-Wl,-e,__w32_libc_start', '-Wl,--gc-sections', '-Wl,-z,max-page-size=4096',
           '-Wl,--hash-style=both', '-Wl,-z,now', '-Wl,--no-undefined', '-Wl,--build-id=sha1',
           f'-Wl,--version-script={version_script}', '-Wl,--undefined-version', f'@{rsp}', unwind, builtins]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        lines = p.stderr.splitlines()
        undef = sorted({l.split('symbol: ')[1] for l in lines if 'undefined symbol:' in l})
        print('\n'.join(lines[:60]))
        if undef:
            print(f'{len(undef)} undefined:', ' '.join(undef))
        sys.exit(1)
    print('built', os.path.join(OUT, 'libc.so'))


def build_boot():
    os.makedirs(OUT, exist_ok=True)
    cmd = [f'{MINGW}/bin/clang.exe', '-O2', '-Wall', '-Wextra', '-municode', '-mwindows', '-o', os.path.join(OUT, 'blox7.exe'),
           os.path.join(WIN, 'boot', 'blox7.c'), os.path.join(WIN, 'boot', 'boot.c'),
           '-Wl,--major-subsystem-version,6', '-Wl,--minor-subsystem-version,1',
           '-Wl,--major-os-version,6', '-Wl,--minor-os-version,1', '-static']
    subprocess.check_call(cmd)
    print('built', os.path.join(OUT, 'blox7.exe'))
    console = [c.replace('blox7.exe', 'blox7-console.exe') if c.endswith('blox7.exe') else c for c in cmd]
    console = [c for c in console if c != '-mwindows'] + ['-mconsole']
    subprocess.check_call(console)
    print('built', os.path.join(OUT, 'blox7-console.exe'))


def build_bridge_sources():
    src = os.environ.get('BLOX7_DEPS_SRC', 'D:/toolchains/src')
    work = os.path.join(OUT, 'bridge-ast')
    os.makedirs(work, exist_ok=True)
    gen = os.path.join(WIN, 'bridge', 'gen')
    with open(os.path.join(work, 'sdl.c'), 'w') as f:
        f.write('#define VK_NO_PROTOTYPES 1\n#include <vulkan/vulkan_core.h>\n#include <SDL3/SDL.h>\n#include <SDL3/SDL_vulkan.h>\n'
                '#include <SDL3_ttf/SDL_ttf.h>\n#include <SDL3_ttf/SDL_textengine.h>\n')
    with open(os.path.join(work, 'gl.c'), 'w') as f:
        f.write('#define GL_GLEXT_PROTOTYPES 1\n#include <GLES3/gl32.h>\n#include <GLES2/gl2ext.h>\n')
    for name, extra in (('sdl', [f'-I{src}/SDL/include', f'-I{src}/SDL_ttf/include', f'-I{VULKAN_HEADERS}/include']),
                        ('gl', [])):
        with open(os.path.join(work, f'{name}.json'), 'w', encoding='utf-8') as out:
            subprocess.check_call([CLANG, '--target=x86_64-linux-android28', '-fsyntax-only', '-Xclang', '-ast-dump=json']
                                  + extra + [os.path.join(work, f'{name}.c')], stdout=out)
    tool = [sys.executable, '-I', os.path.join(HERE, 'gen-bridge.py')]
    sdl_ast, gl_ast = os.path.join(work, 'sdl.json'), os.path.join(work, 'gl.json')
    subprocess.check_call(tool + [sdl_ast, 'SDL/include/SDL3/', 'SDL3.dll', os.path.join(gen, 'sdl3_bridge.c'),
                                  '--manual', 'SDL_GL_CreateContext,SDL_GL_MakeCurrent,SDL_GL_SwapWindow,SDL_IOFromFile,'
                                  'SDL_LoadFile,'
                                  'SDL_Vulkan_LoadLibrary,SDL_Vulkan_GetVkGetInstanceProcAddr,'
                                  'SDL_Vulkan_CreateSurface,SDL_Vulkan_DestroySurface'])
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'gen-vulkan-bridge.py'),
                           f'{VULKAN_HEADERS}/include/vulkan', f'{VULKAN_HEADERS}/registry/vk.xml',
                           os.path.join(gen, 'vulkan_bridge.c')])
    subprocess.check_call(tool + [sdl_ast, 'SDL_ttf/include/', 'SDL3_ttf.dll', os.path.join(gen, 'ttf_bridge.c'),
                                  '--manual', 'TTF_OpenFont'])
    gles = os.path.join(gen, 'gles_bridge.c')
    subprocess.check_call(tool + [gl_ast, 'GLES3/,GLES2/', 'opengl', gles, '--resolver', 'gles_bridge_resolve',
                                  '--include', 'GLES3/gl32.h,GLES2/gl2ext.h',
                                  '--hook', 'glShaderSource=gles_shader_source,glCompileShader=gles_compile_shader,'
                                  'glTexImage2D=gles_tex_image_2d,glTexSubImage2D=gles_tex_sub_image_2d,'
                                  'glCompressedTexImage2D=gles_compressed_tex_image_2d,'
                                  'glCompressedTexSubImage2D=gles_compressed_tex_sub_image_2d,'
                                  'glCompressedTexSubImage3D=gles_compressed_tex_sub_image_3d,'
                                  'glTexSubImage3D=gles_tex_sub_image_3d,'
                                  'glCompressedTexImage3D=gles_compressed_tex_image_3d,'
                                  'glTexStorage2D=gles_tex_storage_2d,glTexStorage3D=gles_tex_storage_3d,'
                                  'glLinkProgram=gles_link_program,glBufferData=gles_buffer_data,'
                                  'glBufferSubData=gles_buffer_sub_data,glMapBufferRange=gles_map_buffer_range,'
                                  'glUnmapBuffer=gles_unmap_buffer,glClientWaitSync=gles_client_wait_sync,'
                                  'glFinish=gles_finish,glFlush=gles_flush,glReadPixels=gles_read_pixels'])
    with open(gles, encoding='utf-8') as f:
        lines = f.read().split('\n')
    lines.insert(1, '#define GL_GLEXT_PROTOTYPES 1')
    with open(gles, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('generated', gen)


def build_bridges():
    src = os.environ.get('BLOX7_DEPS_SRC', 'D:/toolchains/src')
    flags = ['--target=x86_64-linux-android28', '-O2', '-fPIC', '-shared', '-fuse-ld=lld', '-Wl,--pack-dyn-relocs=relr',
             '-Wl,-z,now', '-Wno-deprecated-declarations', '-Wl,--no-undefined', f'-L{OUT}',
             f'-I{src}/SDL/include', f'-I{src}/SDL_ttf/include', f'-I{VULKAN_HEADERS}/include', '-DVK_NO_PROTOTYPES',
             f'-I{WIN}/bridge']
    bridge = os.path.join(WIN, 'bridge')
    gles = os.path.join(OUT, 'libGLESv2.so.2')
    objdir = os.path.join(OUT, 'bridge-obj')
    os.makedirs(objdir, exist_ok=True)
    root = os.path.dirname(WIN)
    etc_objects = []
    for cc in (os.path.join(bridge, 'gles_etc.cc'), os.path.join(root, 'src', 'graphics', 'etc2_decoder.cc')):
        obj = os.path.join(objdir, os.path.basename(cc) + '.o')
        subprocess.check_call([CLANGXX, '--target=x86_64-linux-android28', '-std=c++17', '-O2', '-fPIC', '-c',
                               f'-I{root}/include', '-o', obj, cc])
        etc_objects.append(obj)
    for soname, sources in (('libGLESv2.so.2', ['gen/gles_bridge.c', 'gles_manual.c'] + etc_objects + ['-lc++_static',
                                                                                                    '-lc++abi']),
                            ('libSDL3.so', ['gen/sdl3_bridge.c', 'sdl3_manual.c', 'gen/vulkan_bridge.c', 'vulkan_manual.c',
                                            gles]),
                            ('libSDL3_ttf.so', ['gen/ttf_bridge.c', 'ttf_manual.c', '-lSDL3'])):
        out = os.path.join(OUT, soname)
        files = [f if f.startswith('-') or os.path.isabs(f) else os.path.join(bridge, f) for f in sources]
        subprocess.check_call([CLANG] + flags + [f'-Wl,-soname,{soname}', '-o', out] + files)
        print('built', out)


def build_webview():
    sdk = os.environ.get('BLOX7_WEBVIEW2_SDK', 'D:/toolchains/src/webview2-sdk/pkg/build/native')
    root = os.path.dirname(WIN)
    out = os.path.join(OUT, 'blox7_webview_helper.exe')
    cmd = [f'{MINGW}/bin/clang++.exe', '-std=c++20', '-O2', '-Wall', '-municode', '-static', '-o', out,
           os.path.join(WIN, 'webview', 'blox7_webview_helper_win.cc'),
           os.path.join(root, 'src', 'webview', 'webview_helper_policy.cc'),
           f'-I{sdk}/include', f'-I{root}/src', f'-I{root}/include', f'-I{OUT}/deps/include',
           '-Wl,--subsystem,windows:6.1', '-Wl,--major-os-version,6', '-Wl,--minor-os-version,1',
           '-lole32', '-lws2_32', '-lgdi32', '-luser32', '-luuid', '-lcomctl32', '-lshell32']
    subprocess.check_call(cmd)
    shutil.copyfile(f'{sdk}/x64/WebView2Loader.dll', os.path.join(OUT, 'WebView2Loader.dll'))
    print('built', out)


def write_default_config(gen):
    with open(os.path.join(os.path.dirname(WIN), 'src', 'runtime', 'runtime_config_bootstrap.cc'), encoding='utf-8') as f:
        source = f.read()
    start = source.index('R"yaml(') + len('R"yaml(')
    yaml = source[start:source.index(')yaml"', start)]
    with open(os.path.join(gen, 'default_config.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('#pragma once\ninline constexpr char kDefaultConfig[] = R"blox7(' + yaml + ')blox7";\n')


def write_forced_flags(gen):
    names = set()
    for policy in ('performance', 'frame_rate', 'http_client', 'texture_memory', 'crash_report'):
        with open(os.path.join(os.path.dirname(WIN), 'src', 'runtime', f'{policy}_policy.cc'), encoding='utf-8') as f:
            names |= set(re.findall(r'"((?:D|S)?F(?:Flag|Int|String|Log)[A-Za-z0-9_]+)"', f.read()))
    with open(os.path.join(gen, 'forced_flags.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('#pragma once\ninline constexpr const char* kForcedFlags[] = {\n')
        f.write(''.join(f'    "{name}",\n' for name in sorted(names)))
        f.write('};\n')


def build_blox7():
    gen = os.path.join(OUT, 'blox7')
    os.makedirs(gen, exist_ok=True)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'dist.py')])
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'pack-blox7.py'), os.path.join(OUT, 'dist'), gen])
    write_default_config(gen)
    write_forced_flags(gen)
    src = os.path.join(WIN, 'blox7')
    res = os.path.join(gen, 'blox7.res.o')
    subprocess.check_call([f'{MINGW}/bin/llvm-windres.exe', '-I', src, '-I', gen, '-O', 'coff',
                           os.path.join(src, 'blox7.rc'), res])
    boot = os.path.join(gen, 'boot.o')
    subprocess.check_call([f'{MINGW}/bin/clang.exe', '-O2', '-Wall', '-Wextra', '-c', os.path.join(WIN, 'boot', 'boot.c'),
                           '-o', boot])
    # windows can't tell Blox7.exe from the runtime's blox7.exe, so the launcher gets a folder of its own
    release = os.path.join(OUT, 'release')
    os.makedirs(release, exist_ok=True)
    exe = os.path.join(release, 'Blox7.exe')
    try:
        open(exe, 'ab').close()
    except PermissionError:
        exe = os.path.join(release, 'Blox7.new.exe')
        print('Blox7.exe is running, so this build goes to Blox7.new.exe')
    cmd = [f'{MINGW}/bin/clang++.exe', '-std=c++20', '-O2', '-Wall', '-Wextra', '-municode', '-mwindows', '-I', gen,
           os.path.join(src, 'main.cpp'), os.path.join(src, 'runtime.cpp'), os.path.join(src, 'settings.cpp'),
           os.path.join(src, 'ui.cpp'), boot, res,
           '-o', exe, '-static', '-Wl,--major-subsystem-version,6', '-Wl,--minor-subsystem-version,1',
           '-Wl,--major-os-version,6', '-Wl,--minor-os-version,1']
    subprocess.check_call(cmd)
    print('built', exe, f'{os.path.getsize(exe) / 1048576:.1f} MB')


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'libc'
    if '--clean' in sys.argv:
        shutil.rmtree(os.path.join(OUT, 'obj'), ignore_errors=True)
    {'libc': build_libc, 'boot': build_boot, 'bridges': build_bridges, 'bridge-sources': build_bridge_sources, 'webview': build_webview,
     'blox7': build_blox7}[what]()
