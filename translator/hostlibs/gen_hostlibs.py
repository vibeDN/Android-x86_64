#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates gen/hostlibs_tables.inc (proxy signature tables for the platform libraries Bionic lacks).

Input: the real C headers (AOSP include dirs, preprocessed with `gcc -E` so typedef chains and macros such
as GL_APIENTRY / ZEXTERN / OF() are resolved). Output: per library a table of
    { name, sig, act, cb }
sig  : proxy_ext.h signature string ("i(pp...)"), typedefs resolved from the headers.
act  : one char per argument, '.' = plain.  E guest JNIEnv*->host  C guest callback -> host thunk
       V va_list (AAPCS64 struct -> host va_list, format = previous arg)  Z z_stream allocator callbacks
       OpenSL ES: S wrapped interface -> host, I SLInterfaceID in, A SLInterfaceID array in (count = prev arg),
       D SLDataSource/Sink (unwrap OutputMix locator), W out SLObjectItf*, G out interface of GetInterface,
       Q out SLInterfaceID*, X = S + forget wrapper after call (Destroy).
       T pointer to a struct of {context, callbacks...} (camera/AImageReader listeners, VkAllocationCallbacks): the
       host gets a copy with every callback member replaced by a host thunk (the host libs copy these structs).
       N Vulkan create info whose pNext chain may hold debug-messenger/report create infos (callbacks swapped in place).
cb   : "<argidx>:<S|->:<callback sig>" for the C action, "<argidx>:T:<m0>,<m1>,..." for T (member '-' = plain).
Data symbols (`extern const char* X`, e.g. AMEDIAFORMAT_KEY_*) go to kData {table, name}: the guest gets a copy.
Functions that cannot be marshalled (struct by value, long double, float callbacks...) are listed in
gen/skipped.txt.  Usage: gen_hostlibs.py [--aosp /path/to/aosp15] [--out gen]
"""
import argparse, os, re, subprocess, sys, tempfile

QUAL = {'const', 'volatile', 'restrict', '__restrict', '__restrict__', 'extern', 'static', 'inline', '__inline',
        '__inline__', '_Nonnull', '_Nullable', '_Null_unspecified', '__extension__', 'register', '_Noreturn',
        '__THROW', 'signed_'}
BUILTIN = {'void', 'char', 'short', 'int', 'long', 'float', 'double', 'signed', 'unsigned', '_Bool', 'bool'}
INTCLASS = set('iulpzbhc')

class Skip(Exception):
    pass

def strip_balanced(s, word):
    """removes `word ( ... )` with balanced parentheses"""
    while True:
        m = re.search(r'\b' + word + r'\b\s*\(', s)
        if not m:
            return s
        i, d = m.end(), 1
        while i < len(s) and d:
            d += (s[i] == '(') - (s[i] == ')')
            i += 1
        s = s[:m.start()] + ' ' + s[i:]

def clean(s):
    for w in ('__attribute__', '__asm__', '__asm', '__declspec'):
        s = strip_balanced(s, w)
    return re.sub(r'\s+', ' ', s).strip()

def preprocess(headers, incs, defs, roots, aosp):
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(''.join('#include <%s>\n' % h for h in headers))
        src = f.name
    cmd = ['gcc', '-E', '-D__ANDROID__', '-D__ANDROID_API__=35', '-D__INTRODUCED_IN(x)=', '-D__INTRODUCED_IN_32(x)=',
           '-D__INTRODUCED_IN_64(x)=', '-D__BEGIN_DECLS=', '-D__END_DECLS=', '-D__printflike(a,b)=',
           '-D__BIONIC_AVAILABILITY_GUARD(x)=1', '-D__RENAME(x)=', '-D__nodiscard=', '-D__unused=',
           '-D__BIONIC__', '-D_GNU_SOURCE', '-D_LARGEFILE64_SOURCE'] + ['-D' + d for d in defs] + ['-I' + os.path.join(aosp, i) for i in incs] + \
          ['-idirafter', os.path.join(aosp, 'bionic/libc/include'), src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(src)
    if r.returncode:
        sys.exit('preprocess failed: %s\n%s' % (' '.join(cmd), r.stderr[:2000]))
    allt, api, cur, keep = [], [], '', False
    rootp = [os.path.join(aosp, x) for x in roots]
    for line in r.stdout.split('\n'):
        m = re.match(r'# \d+ "([^"]*)"', line)
        if m:
            cur = os.path.normpath(m.group(1))
            keep = any(cur.startswith(os.path.normpath(p)) for p in rootp)
            continue
        allt.append(line)
        if keep:
            api.append(line)
    return '\n'.join(allt), '\n'.join(api)

def statements(text):
    out, cur, depth = [], [], 0
    typedefish = False
    for ch in text:
        if ch == '{':
            depth += 1
        cur.append(ch)
        if ch == '}':
            depth -= 1
            if depth == 0:
                st = ''.join(cur).strip()
                if not re.match(r'(typedef|struct|union|enum|extern)\b', st):   # inline function body
                    out.append(('body', st)); cur = []
        elif ch == ';' and depth == 0:
            out.append(('stmt', ''.join(cur).strip().rstrip(';').strip())); cur = []
    return [s for k, s in out if k == 'stmt']

class Types:
    def __init__(self, text):
        self.td, self.members = {}, {}
        for st in statements(text):
            if st.startswith('typedef '):
                self.add(st[8:])
        self.td.setdefault('size_t', ('type', 'unsigned long'))   # used by headers that do not include stddef.h
        self.td.setdefault('ssize_t', ('type', 'long'))

    def add(self, st):
        kind = None
        m0 = m = re.match(r'(struct|union|enum)\b[^{]*(\{.*\})', st, re.S)
        if m:
            kind = m.group(1)
        body = st
        while re.search(r'\{[^{}]*\}', body):
            body = re.sub(r'\{[^{}]*\}', ' ', body)
        body = clean(body)
        m = re.search(r'\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$', body)
        if m:
            self.td[m.group(1)] = ('fnptr', body[:body.index('(')].strip(), m.group(2))
            return
        m = re.match(r'(.*?)\b(\w+)\s*\((.*)\)$', body)       # typedef R name(args)  (function type)
        if m and '*' not in m.group(0).split('(')[0] and kind is None:
            self.td[m.group(2)] = ('fn', m.group(1), m.group(3))
            return
        if ',' in body:
            body = body.split(',')[0]
        m = re.match(r'(.*?)(\*?)\s*(\w+)(\[.*\])?$', body)
        if not m:
            return
        base, star, name, arr = m.groups()
        if kind in ('struct', 'union') and not star:
            self.td[name] = ('struct',)
            inner = m0.group(2)[1:-1]
            if kind == 'struct' and '{' not in inner:
                self.members[name] = inner
        elif kind == 'enum' and not star:
            self.td[name] = ('enum',)
        elif arr:
            self.td[name] = ('type', base + ' *')
        else:
            self.td[name] = ('type', (base + ' ' + star).strip())

    def sig(self, t, depth=0):
        t = clean(t)
        if '*' in t or '[' in t:
            return 'p'
        w = [x for x in t.split() if x not in QUAL]
        if not w or depth > 12:
            raise Skip('type "%s"' % t)
        if w[0] in ('struct', 'union'):
            raise Skip('struct by value')
        if w[0] == 'enum':
            return 'i'
        if w[0] in ('__builtin_va_list', '__gnuc_va_list', 'va_list'):
            return 'V'
        s = set(w)
        if 'void' in s: return 'v'
        if 'float' in s: return 'f'
        if 'double' in s:
            if 'long' in s: raise Skip('long double')
            return 'd'
        if '_Bool' in s or 'bool' in s: return 'z'
        if 'char' in s: return 'b' if 'signed' in s else 'z'
        if 'short' in s: return 'c' if 'unsigned' in s else 'h'
        if 'long' in s: return 'l'
        if 'int' in s or 'unsigned' in s or 'signed' in s: return 'u' if 'unsigned' in s else 'i'
        if len(w) == 1 and w[0] in self.td:
            d = self.td[w[0]]
            if d[0] in ('fnptr', 'fn'): return 'p'
            if d[0] == 'struct': raise Skip('struct %s by value' % w[0])
            if d[0] == 'enum': return 'i'
            return self.sig(d[1], depth + 1)
        raise Skip('unknown type "%s"' % t)

    def fnptr_of(self, t):
        """(ret, args) if t (qualifiers stripped) names a function pointer typedef, else None"""
        w = [x for x in clean(t).split() if x not in QUAL]
        if len(w) == 1 and w[0] in self.td and self.td[w[0]][0] == 'fnptr':
            return self.td[w[0]][1:]
        return None

    def cb_sig(self, t):
        """'r(args)' if t is a function pointer type with an int-class signature, None if not a function pointer"""
        fp = self.fnptr_of(t)
        raw = clean(t)
        if fp is None and re.search(r'\(\s*\*', raw):
            m = re.match(r'(.*?)\(\s*\*\s*\w*\s*\)\s*\((.*)\)$', raw)
            fp = (m.group(1), m.group(2))
        if fp is None:
            return None
        cs = [self.sig(x) if clean(x) != 'void' else '' for x in (param(a, self)[0] for a in split_args(fp[1]))]
        cr = self.sig(fp[0])
        if any(x not in INTCLASS for x in cs) or cr not in INTCLASS | {'v'} or len(cs) > 8:
            raise Skip('callback not int-class')
        return '%s(%s)' % (cr, ''.join(cs))

    def cb_struct(self, t):
        """member list 'm0,m1,..' if t is a pointer to a struct made of pointers incl. >= 1 callback, else None"""
        raw = clean(t)
        if raw.count('*') != 1 or not raw.endswith('*'):
            return None
        w = [x for x in raw[:-1].split() if x not in QUAL and x not in ('struct',)]
        if len(w) != 1 or w[0] not in self.members:
            return None
        out = []
        for mem in self.members[w[0]].split(';'):
            mem = clean(mem)
            if not mem: continue
            pt = param(mem, self)[0]
            cs = self.cb_sig(pt)
            if cs: out.append(cs)
            elif '*' in pt: out.append('-')
            else: return None              # non-pointer member: not a plain callback table
        return ','.join(out) if any(x != '-' for x in out) else None

def split_args(a):
    out, d, cur = [], 0, ''
    for ch in a:
        if ch == ',' and d == 0:
            out.append(cur); cur = ''; continue
        d += (ch == '(') - (ch == ')')
        cur += ch
    if cur.strip():
        out.append(cur)
    return [x.strip() for x in out]

def param(p, ty):
    """-> (type text, name or '')"""
    p = clean(p)
    if re.search(r'\(\s*\*', p):                    # inline function pointer
        m = re.search(r'\(\s*\*\s*(\w*)\s*\)', p)
        return p, m.group(1)
    toks = re.findall(r'[A-Za-z_]\w*|\*|\[|\]|\d+', p)
    ty_toks = [t for t in toks if t not in QUAL]
    if len(ty_toks) >= 2 and re.match(r'[A-Za-z_]', toks[-1]) and toks[-1] not in BUILTIN and \
            toks[-2] not in ('struct', 'union', 'enum'):
        i = p.rindex(toks[-1])
        return p[:i].strip(), toks[-1]
    return p, ''

def parse_proto(st, ty):
    """-> (name, ret text, [(type, name)], variadic) or None"""
    st = clean(st)
    if st.startswith('typedef') or '(' not in st or st.endswith('}'):
        return None
    st = re.sub(r'^(extern|static)\s+', '', st)
    m = re.match(r'^(.*?)\b([A-Za-z_]\w*)\s*\((.*)\)$', st)
    if not m or '(' in m.group(1) or '=' in st:
        return None
    ret, name, args = m.groups()
    ret = ret.strip()
    if not ret:
        return None
    var = False
    ps = split_args(args)
    if ps and ps[-1] == '...':
        var = True; ps = ps[:-1]
    if len(ps) == 1 and clean(ps[0]) == 'void':
        ps = []
    return name, ret, [param(p, ty) for p in ps], var

def proto_entry(name, ret, params, var, ty, sl=False, itf=None):
    """-> (sig, act, cb)"""
    rs = ty.sig(ret)
    if rs == 'V': raise Skip('va_list return')
    chars, act, cb = [], [], ''
    for i, (t, pn) in enumerate(params):
        c = '.'
        fp = ty.fnptr_of(t)
        raw = clean(t)
        try:
            if fp is None and re.search(r'\(\s*\*', raw):
                m = re.match(r'(.*?)\(\s*\*\s*\w*\s*\)\s*\((.*)\)$', raw)
                fp = (m.group(1), m.group(2))
            if fp is not None:
                cs = [ty.sig(x) if clean(x) != 'void' else '' for x in
                      (param(a, ty)[0] for a in split_args(fp[1]))]
                cr = ty.sig(fp[0])
                if any(x not in INTCLASS for x in cs) or cr not in INTCLASS | {'v'} or len(cs) > 8:
                    raise Skip('callback not int-class')
                cb += (';' if cb else '') + '%d:%s:%s(%s)' % (i, 'S' if sl and cs and re.match(r'SL\w*Itf$', clean(param(split_args(fp[1])[0], ty)[0])) else '-', cr, ''.join(cs))
                chars.append('p'); act.append('C'); continue
            cst = ty.cb_struct(t)
            if cst:
                if name == 'ACameraManager_openCamera':   # member 3 (shared-camera callback, API 36) is absent in
                    cst = ','.join(cst.split(',')[:3] + ['0'])   # older apps' struct and unused here: pass 0
                cb += (';' if cb else '') + '%d:T:%s' % (i, cst)
                chars.append('p'); act.append('T'); continue
            s = ty.sig(t)
        except Skip as e:
            raise Skip('param %d: %s' % (i, e))
        if s == 'V':
            if i == 0: raise Skip('va_list as first arg')
            act.append('V'); chars.append('p'); continue
        if s == 'v': raise Skip('void param')
        chars.append(s)
        if sl:
            nq = re.sub(r'\s+', '', raw)
            base = re.sub(r'\bconst\b', '', raw).replace('*', ' * ').split()
            if re.fullmatch(r'SL\w*Itf', raw.replace('const', '').strip()): c = 'S'
            elif raw.replace('const', '').strip() == 'SLInterfaceID': c = 'I'
            elif 'SLInterfaceID' in raw and '*' in raw and raw.startswith('const'): c = 'A'
            elif 'SLInterfaceID' in raw and '*' in raw: c = 'Q' if 'QuerySupported' in name else 'A'
            elif re.fullmatch(r'SL\w*Itf\s*\*', raw): c = 'W'
            elif re.search(r'SLData(Source|Sink)\s*\*', raw): c = 'D'
            elif name == 'GetInterface' and pn == 'pInterface': c = 'G'
        act.append(c)
    if sl and name == 'Destroy':
        act[0] = 'X'
    if ('E' in act or True) and params:
        t0 = clean(params[0][0])
        if re.match(r'(const )?JNIEnv\s*\*', t0): act[0] = 'E'
    if name in VK_DEBUG_CHAIN:
        for i, (t, pn) in enumerate(params):
            if pn == 'pCreateInfo': act[i] = 'N'
    if name in ('deflateInit_', 'deflateInit2_', 'inflateInit_', 'inflateInit2_', 'inflateBackInit_'):
        act[0] = 'Z'
    sig = '%s(%s%s)' % (rs, ''.join(chars), '...' if var else '')
    if var and any(a != '.' for a in act):
        raise Skip('variadic with special args')
    simple = any(a != '.' for a in act)
    if simple:
        if any(c not in INTCLASS for c in chars) or rs not in INTCLASS | {'v'} or len(chars) > 12:
            raise Skip('special args need int-class signature')
        if var: raise Skip('variadic with special args')
    return sig, (''.join(act).rstrip('.') if simple else ''), cb

VK_DEBUG_CHAIN = ('vkCreateInstance', 'vkCreateDebugUtilsMessengerEXT', 'vkCreateDebugReportCallbackEXT')

def collect(api, ty, skipped, lib, sl=False, prefix=None):
    ents = {}
    for st in statements(api):
        p = parse_proto(st, ty)
        if not p:
            continue
        name, ret, params, var = p
        if prefix and not re.match(prefix, name):
            continue
        if name in ents or name.startswith('__builtin') or name.startswith('_'):
            if not name.startswith('__android'):
                continue
        try:
            ents[name] = proto_entry(name, ret, params, var, ty, sl)
        except Skip as e:
            skipped.append('%s: %s: %s' % (lib, name, e))
    return ents

# ---------------------------------------------------------------------------------------------------------
OPENGL = ['frameworks/native/opengl/include']
MEDIA_INC = ['frameworks/av/media/ndk/include', 'frameworks/native/include', 'frameworks/native/libs/nativewindow/include',
             'frameworks/native/libs/arect/include', 'libnativehelper/include_jni']
LIBS = {
    'LOG': dict(headers=['android/log.h'], incs=['system/logging/liblog/include'], roots=['system/logging/liblog/include'],
                prefix=r'(__android_|android_)'),
    'Z': dict(headers=['zlib.h'], incs=['external/zlib'], roots=['external/zlib'], prefix=r'(z|gz|inflate|deflate|crc|adler|uncompress|compress|get_crc|zError|zlib)'),
    'ANDROID': dict(headers=['android/native_window.h', 'android/native_window_jni.h', 'android/hardware_buffer.h',
                             'android/hardware_buffer_jni.h',
                             'android/asset_manager.h', 'android/asset_manager_jni.h', 'android/looper.h',
                             'android/configuration.h', 'android/sensor.h', 'android/input.h', 'android/choreographer.h',
                             'android/native_activity.h', 'android/surface_texture.h', 'android/surface_texture_jni.h',
                             'android/storage_manager.h', 'android/obb.h', 'android/trace.h', 'android/sharedmem.h',
                             'android/sharedmem_jni.h', 'android/performance_hint.h', 'android/thermal.h',
                             'android/choreographer.h', 'android/imagedecoder.h', 'android/surface_control.h',
                             'android/system_fonts.h', 'android/font.h', 'android/font_matcher.h'],
                    incs=['frameworks/native/include', 'frameworks/native/libs/nativewindow/include', 'frameworks/native/libs/arect/include', 'libnativehelper/include_jni'],
                    roots=['frameworks/native/include/android', 'frameworks/native/libs/nativewindow/include/android'],
                    prefix=r'(A[A-Z]|ANative|ALooper)'),
    'JNIGFX': dict(headers=['android/bitmap.h'], incs=['frameworks/native/include', 'libnativehelper/include_jni'],
                   roots=['frameworks/native/include/android/bitmap.h'], prefix=r'AndroidBitmap_'),
    'EGL': dict(headers=['EGL/egl.h', 'EGL/eglext.h'], incs=OPENGL, defs=['EGL_EGLEXT_PROTOTYPES'], roots=OPENGL, prefix=r'egl'),
    'GLES1': dict(headers=['GLES/gl.h', 'GLES/glext.h'], incs=OPENGL, defs=['GL_GLEXT_PROTOTYPES'], roots=OPENGL, prefix=r'gl'),
    'GLES2': dict(headers=['GLES2/gl2.h', 'GLES2/gl2ext.h', 'GLES3/gl3.h', 'GLES3/gl31.h', 'GLES3/gl32.h'], incs=OPENGL,
                  defs=['GL_GLEXT_PROTOTYPES'], roots=OPENGL, prefix=r'gl'),
    'MEDIA': dict(headers=['media/Ndk%s.h' % h for h in ('Image', 'ImageReader', 'MediaCodec', 'MediaCodecInfo',
                  'MediaCodecStore', 'MediaCrypto', 'MediaDataSource', 'MediaDrm', 'MediaError', 'MediaExtractor',
                  'MediaFormat', 'MediaMuxer')], incs=MEDIA_INC, roots=['frameworks/av/media/ndk/include'],
                  prefix=r'(AMedia|AImage)'),
    'CAMERA': dict(headers=['camera/Ndk%s.h' % h for h in ('CameraManager', 'CameraDevice', 'CameraCaptureSession',
                   'CameraMetadata', 'CaptureRequest')], incs=['frameworks/av/camera/ndk/include'] + MEDIA_INC,
                   roots=['frameworks/av/camera/ndk/include'], prefix=r'(ACamera|ACapture)'),
    'SYNC': dict(headers=['android/sync.h'], incs=['system/core/libsync/include'], roots=['system/core/libsync/include'],
                 prefix=r'sync_'),
    'AAUDIO': dict(headers=['aaudio/AAudio.h'], incs=['frameworks/av/media/libaaudio/include'],
                   roots=['frameworks/av/media/libaaudio/include'], prefix=r'AAudio'),
    'VULKAN': dict(headers=['vulkan/vulkan.h'], incs=['external/vulkan-headers/include'] + MEDIA_INC,
                   defs=['VK_USE_PLATFORM_ANDROID_KHR'], roots=['external/vulkan-headers/include'], prefix=r'vk'),
}
EXTRA_PROTOS = {   # exported by the host libs but absent from the NDK headers
    'LOG': ['int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap)',
            'int __android_log_buf_vprint(int bufID, int prio, const char* tag, const char* fmt, va_list ap)'],
}

def sles(aosp, skipped):
    cfg = dict(headers=['SLES/OpenSLES.h', 'SLES/OpenSLES_Android.h', 'SLES/OpenSLES_AndroidConfiguration.h',
                        'SLES/OpenSLES_AndroidMetadata.h'], incs=['frameworks/wilhelm/include', 'libnativehelper/include_jni'],
               defs=[], roots=['frameworks/wilhelm/include'])
    allt, api = preprocess(cfg['headers'], cfg['incs'], cfg['defs'], cfg['roots'], aosp)
    ty = Types(allt)
    funcs = collect(api, ty, skipped, 'SLES', sl=True, prefix=r'sl[A-Z]')
    itfs = []
    for m in re.finditer(r'struct\s+(SL\w+Itf)_\s*\{(.*?)\}\s*;', api, re.S):
        meths = []
        for mem in m.group(2).split(';'):
            mem = clean(mem)
            if not mem: continue
            mm = re.match(r'(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$', mem)
            if not mm: continue
            ps = [param(a, ty) for a in split_args(mm.group(3))]
            try:
                sig, act, cb = proto_entry(mm.group(2), mm.group(1), ps, False, ty, sl=True)
                if not act or act[0] not in 'SX': act = 'S' + act[1:]
                meths.append((mm.group(2), sig, act, cb))
            except Skip as e:
                skipped.append('SLES: %s::%s: %s' % (m.group(1), mm.group(2), e))
                meths.append((mm.group(2), None, '', ''))      # keep the slot index, unsupported
        itfs.append((m.group(1), meths))
    iids = sorted(set(re.findall(r'extern\s+const\s+SLInterfaceID\s+(SL_IID_\w+)', api)))
    return funcs, itfs, iids

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--aosp', default='/mnt/storage-adata/aosp15')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    skipped, o = [], []
    o.append('// SPDX-License-Identifier: GPL-2.0-or-later\n// GENERATED by gen_hostlibs.py from the AOSP headers. Do not edit.\n')
    counts, data = {}, []
    for lib, c in LIBS.items():
        allt, api = preprocess(c['headers'], c['incs'], c.get('defs', []), c['roots'], a.aosp)
        ty = Types(allt)
        ents = collect(api, ty, skipped, lib, prefix=c['prefix'])
        for p in EXTRA_PROTOS.get(lib, []):
            pr = parse_proto(p, ty)
            if pr and pr[0] not in ents:
                ents[pr[0]] = proto_entry(pr[0], pr[1], pr[2], pr[3], ty)
        counts[lib] = len(ents)
        for d in sorted(set(re.findall(r'extern\s+const\s+char\s*\*\s*(?:_Nonnull\s+|_Nullable\s+)?(\w+)\s*;', api))):
            if re.match(c['prefix'], d) or d.startswith('AMEDIA'):
                data.append((lib, d))
        o.append('static const Ent kTab_%s[] = {' % lib)
        for n in sorted(ents):
            s, act, cb = ents[n]
            o.append('  {%s,%s,%s,%s},' % (q(n), q(s), q(act), q(cb)))
        o.append('};')
    o.append('static const DataDef kData[] = {')
    o += ['  {%s, %s},' % (q(l), q(d)) for l, d in data]
    o.append('};')
    counts['data'] = len(data)
    funcs, itfs, iids = sles(a.aosp, skipped)
    counts['SLES'] = len(funcs)
    o.append('static const Ent kTab_SLES[] = {')
    for n in sorted(funcs):
        s, act, cb = funcs[n]
        o.append('  {%s,%s,%s,%s},' % (q(n), q(s), q(act), q(cb)))
    o.append('};')
    for name, meths in itfs:
        o.append('static const Ent kMeth_%s[] = {' % name)
        for n, s, act, cb in meths:
            o.append('  {%s,%s,%s,%s},' % (q(name + '::' + n), q(s or ''), q(act), q(cb)))
        o.append('};')
    o.append('static const ItfDef kItfs[] = {')
    for name, meths in itfs:
        o.append('  {%s, kMeth_%s, %d},' % (q(name), name, len(meths)))
    o.append('};')
    # SL_IID_<X> -> SL<Name>Itf by case-insensitive name match
    byup = {re.sub(r'^SL|Itf$', '', n).upper(): n for n, _ in itfs}
    byup['ANDROIDBUFFERQUEUESOURCE'] = 'SLAndroidBufferQueueItf'
    byup['LED'] = 'SLLEDArrayItf'
    o.append('static const IidDef kIids[] = {')
    for i in iids:
        k = i[len('SL_IID_'):]
        o.append('  {%s, %s},' % (q(i), q(byup[k]) if k in byup else 'nullptr'))
        if k not in byup:
            skipped.append('SLES: %s has no interface struct (IID data symbol kept, interface not wrapped)' % i)
    o.append('};')
    open(os.path.join(a.out, 'hostlibs_tables.inc'), 'w').write('\n'.join(o) + '\n')
    open(os.path.join(a.out, 'skipped.txt'), 'w').write('\n'.join(sorted(skipped)) + '\n')
    print(counts, 'skipped:', len(skipped))

if __name__ == '__main__':
    main()
