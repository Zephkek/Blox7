"""just enough Android.bp parsing to build bionic outside aosp
usage: bp.py <Android.bp> [...] <module> [--static] [--json]"""
import json, os, re, sys

TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|`[^`]*`|[A-Za-z_][\w.]*|-?\d+|[{}\[\]:,=+()]', re.S)


def tokenize(text):
    pos, out = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise SyntaxError(f'at {text[pos:pos + 40]!r}')
        t = m.group(0)
        pos = m.end()
        if t[0].isspace() or t.startswith('//') or t.startswith('/*'):
            continue
        out.append(t)
    return out


class Parser:
    def __init__(self, tokens, variables):
        self.t, self.i, self.vars = tokens, 0, variables

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self, want=None):
        tok = self.t[self.i]
        if want is not None and tok != want:
            raise SyntaxError(f'expected {want}, got {tok} near {self.t[self.i - 5:self.i + 5]}')
        self.i += 1
        return tok

    def value(self):
        v = self.atom()
        while self.peek() == '+':
            self.take()
            w = self.atom()
            if isinstance(v, dict) and isinstance(w, dict):
                v = merge(dict(v), w)
            else:
                v = v + w
        return v

    def atom(self):
        tok = self.take()
        if tok[0] in '"`':
            return json.loads(tok) if tok[0] == '"' else tok[1:-1]
        if tok == '[':
            items = []
            while self.peek() != ']':
                items.append(self.value())
                if self.peek() == ',':
                    self.take()
            self.take(']')
            return items
        if tok == '{':
            return self.props('}')
        if tok in ('true', 'false'):
            return tok == 'true'
        if re.match(r'-?\d+$', tok):
            return int(tok)
        if tok == 'select' and self.peek() == '(':
            depth = 0
            while True:
                x = self.take()
                if x == '(':
                    depth += 1
                elif x == ')':
                    depth -= 1
                    if depth == 0:
                        break
            return []
        return self.vars.get(tok, [])

    def props(self, end):
        d = {}
        while self.peek() != end:
            key = self.take()
            if key[0] == '"':
                key = json.loads(key)
            sep = self.take()
            if sep not in (':', '='):
                raise SyntaxError(f'bad property {key} {sep}')
            d[key] = self.value()
            if self.peek() == ',':
                self.take()
        self.take(end)
        return d

    def file(self):
        modules = []
        while self.peek() is not None:
            name = self.take()
            if self.peek() in ('=', '+'):
                op = self.take()
                if op == '+':
                    self.take('=')
                    self.vars[name] = self.vars.get(name, []) + self.value()
                else:
                    self.vars[name] = self.value()
            else:
                self.take('{')
                props = self.props('}')
                props['_type'] = name
                modules.append(props)
        return modules


def merge(a, b):
    for k, v in b.items():
        if k in a and isinstance(a[k], list) and isinstance(v, list):
            a[k] = a[k] + v
        elif k in a and isinstance(a[k], dict) and isinstance(v, dict):
            a[k] = merge(dict(a[k]), v)
        else:
            a[k] = v
    return a


def load(paths):
    modules, variables = {}, {}
    for path in paths:
        base = os.path.dirname(os.path.abspath(path))
        for m in Parser(tokenize(open(path, encoding='utf-8').read()), variables).file():
            if 'name' in m:
                if m['name'] in modules and not m['_type'].startswith(('cc_', 'filegroup', 'genrule')):
                    continue
                m['_dir'] = base
                modules[m['name']] = m
    return modules


def flatten(m, modules, static):
    """props with defaults and the arch/target/linkage sections folded in"""
    props = {}
    for d in m.get('defaults', []):
        if d in modules:
            props = merge(props, flatten(modules[d], modules, static))
    own = {k: v for k, v in m.items() if k not in ('defaults',)}
    props = merge(props, own)
    for section, keys in (('arch', ['x86_64']), ('target', ['android', 'bionic', 'not_windows', 'linux', 'linux_bionic', 'android_x86_64']),
                          ('multilib', ['lib64']), ('static' if static else 'shared', [None])):
        sec = props.pop(section, {})
        for key in keys:
            sub = sec if key is None else sec.get(key, {})
            if isinstance(sub, dict):
                props = merge(props, flatten({**sub}, modules, static) if 'defaults' in sub else sub)
    return props


def collect(name, modules, static=False, seen=None):
    """sources of a module + its whole_static_libs. units keep each source's own module flags"""
    seen = seen if seen is not None else set()
    if name.startswith('//'):
        name = name.split(':')[-1]
    out = {'srcs': [], 'cflags': [], 'cppflags': [], 'asflags': [], 'conlyflags': [], 'include_dirs': [],
           'local_include_dirs': [], 'modules': [], 'units': []}
    if name in seen or name not in modules:
        return out
    seen.add(name)
    m = modules[name]
    props = flatten(m, modules, static)
    base = m['_dir']
    out['modules'].append(name)
    excl = set(os.path.normpath(os.path.join(base, s)) for s in props.get('exclude_srcs', []))
    srcs = []
    for s in props.get('srcs', []):
        if s.startswith(':'):
            fg = modules.get(s[1:])
            if fg and fg['_type'] == 'filegroup':
                fgp = flatten(fg, modules, static)
                srcs += [os.path.normpath(os.path.join(fg['_dir'], fgp.get('path', ''), x)) for x in fgp.get('srcs', [])]
            else:
                srcs.append(s)
            continue
        if '*' in s:
            import glob
            srcs += [os.path.normpath(g) for g in sorted(glob.glob(os.path.join(base, s), recursive=True))
                     if os.path.normpath(g) not in excl]
            continue
        p = os.path.normpath(os.path.join(base, s))
        if p not in excl:
            srcs.append(p)
    own = {k: list(props.get(k, [])) for k in ('cflags', 'cppflags', 'asflags', 'conlyflags')}
    own['local_include_dirs'] = [os.path.normpath(os.path.join(base, d))
                                 for d in props.get('local_include_dirs', []) + props.get('export_include_dirs', [])]
    own['include_dirs'] = list(props.get('include_dirs', []))
    own['module_dir'] = base
    out['srcs'] += srcs
    out['units'] += [(src, name, own) for src in srcs]
    for k in own:
        if k in out:
            out[k] += own[k]
    for lib in props.get('whole_static_libs', []):
        sub = collect(lib, modules, True, seen)
        for k in out:
            out[k] += sub[k]
    return out


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    mods = load(args[:-1])
    r = collect(args[-1], mods, '--static' in sys.argv)
    if '--json' in sys.argv:
        print(json.dumps(r, indent=1))
    else:
        print('modules:', ' '.join(r['modules']))
        print(len(r['srcs']), 'sources')
        for s in r['srcs']:
            print(' ', s)
