# Temporary debugging traces in a Dusklight / Aurora source file (native mode bring-up).
#   python3 _dbg.py FILE 'statement start' ...   a trace after every statement line that starts with one of them
#   python3 _dbg.py FILE --remove                 takes them out again (before patches/ are saved)
# Traces go to sdmc:/gc3ds/native/trace.txt (native::trace); the tag is a line comment, safe inside block comments.
import sys
p = sys.argv[1]
s = open(p).read()
TAG = '// NATIVE_DBG'
if '--remove' in sys.argv:
    s = '\n'.join(l for l in s.split('\n') if TAG not in l)
    open(p, 'w').write(s)
    print('removed from', p)
    raise SystemExit
pats = sys.argv[2:]
lines = s.split('\n')
out = []
n = 0
name = p.split('/')[-1]
for i, l in enumerate(lines):
    out.append(l)
    st = l.strip()
    if TAG in l or not st.endswith(';'):
        continue
    for pat in pats:
        if st.startswith(pat):
            ind = len(l) - len(l.lstrip())
            label = ('%s:%d %s' % (name, i + 1, st[:50])).replace('\\', '').replace('"', "'")
            out.append(' ' * ind + 'native::trace("%s"); %s' % (label, TAG))
            n += 1
            break
s = '\n'.join(out)
if 'void trace(const char* fmt, ...); }' not in s:
    k = s.index('#include')
    s = s[:k] + 'namespace native { void trace(const char* fmt, ...); } ' + TAG + '\n' + s[k:]
open(p, 'w').write(s)
print('traces added:', n, 'in', p)
