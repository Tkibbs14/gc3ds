#!/usr/bin/env python3
"""The JIT's ARM output for a hand-written guest loop, without a disc image.

    python3 core/tools/jitbench.py core/tests/jit/bench/*.S [--seconds 2] [--opt MASK] [--cycles] [--dump]

Each case is PowerPC assembly (powerpc-linux-gnu-as) wrapped into a DOL, run under qemu-arm with the real ARM build
of the JIT (core/tools/arm_build.sh), and reported as:

  * **hot ARM words per guest instruction** - the cost that decides most on the console, where instruction fetch is
    ~10 cycles a guest instruction and linked blocks cost 13 ns while the hot code fits in L1 but 53-62 ns from 32 KB
    (docs/FINDINGS.md). Smaller is better even when the instruction count does not change.
  * **modelled stall cycles** (`--cycles`, JIT_SCHEDDUMP + arm11_sched.py): the ARM11 interlock model's view.
  * `--dump` prints each big block's dump line (JIT_DUMP=big). `--opt MASK` overrides the device's mask.

What it is for: ranking a codegen idea before spending console time. What it is not: the game's instruction mix. A
case here is a loop shaped like hot game code, not the game; the model's blind spot is instruction fetch, so a change
that grows the hot code can win here and lose on the 2DS (HANDOFF: float lanes, traces). Confirm on the device.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)
TEXT_ADDR = 0x80003100
AS = 'powerpc-linux-gnu-as'
LD = 'powerpc-linux-gnu-ld'
OBJCOPY = 'powerpc-linux-gnu-objcopy'


def build_dol(src, out_dir):
    """Assemble `src` and wrap its .text into a DOL whose entry is its first instruction."""
    obj, elf, bin_, dol = (os.path.join(out_dir, n) for n in ('case.o', 'case.elf', 'case.bin', 'case.dol'))
    subprocess.run([AS, '-mregnames', '-m750cl', '-o', obj, src], check=True)  # -m750cl: the Gekko's paired singles
    subprocess.run([LD, '-Ttext=0x%08x' % TEXT_ADDR, '-e', '_start', '-o', elf, obj], check=True)
    subprocess.run([OBJCOPY, '-O', 'binary', '--only-section=.text', elf, bin_], check=True)
    text = open(bin_, 'rb').read()
    hdr = bytearray(0x100)
    struct.pack_into('>I', hdr, 0x00, 0x100)        # text section 0: file offset
    struct.pack_into('>I', hdr, 0x48, TEXT_ADDR)    # ... load address
    struct.pack_into('>I', hdr, 0x90, len(text))    # ... size
    struct.pack_into('>I', hdr, 0xE0, TEXT_ADDR)    # entry
    open(dol, 'wb').write(bytes(hdr) + text)
    return dol


# The 2DS's own mask, read from a run's `opt` column (job 1485, 2026-10-29): HOIST | KSTORE | FCACHE | RETSTUB |
# SYSCALL | HLELINK | ROTMASK | NATIVE2 | IND2 | LATCH | PIPEK | GPRINLINE | TIER. The JIT's own default is 0, so a
# bench that does not set this measures an unoptimized JIT and tells you nothing about the console.
DEVICE_OPT = 8722295


def run(dol, seconds, opt, sched_path, dump):
    env = dict(os.environ, JIT_REGSTAT='1', GC3DS_NOFAULTMEM='1',  # the device runs with fault mode off
               GC3DS_OPT=str(DEVICE_OPT if opt is None else opt))
    if sched_path:
        env['JIT_SCHEDDUMP'] = sched_path
    if dump:
        env['JIT_DUMP'] = 'big'
    cmd = ['qemu-arm', '-L', '/usr/arm-linux-gnueabihf', '-cpu', 'cortex-a7', os.path.join(CORE, 'build-arm', 'gcemu'),
           dol, '--seconds', str(seconds), '--jit']
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=CORE)
    return p.stdout + p.stderr


NUM = r'([0-9]+(?:\.[0-9]+)?)'


def summarize(out):
    """The numbers worth comparing between two builds of the JIT."""
    got = {}
    m = re.search(r'JIT: (\d+) instructions run', out)
    if m:
        got['guest instructions'] = float(m.group(1))
    m = re.search(r'kept blocks[^\n]*?' + NUM + r' hot words per guest instruction', out)
    if m:
        got['hot words / instruction'] = float(m.group(1))
    m = re.search(NUM + r' per instruction; exit stubs and shared stubs not included', out)
    if m:
        got['hot words / instruction (regstat)'] = float(m.group(1))
    m = re.search(r'JIT: blocks compiled (\d+)', out)
    if m:
        got['blocks'] = float(m.group(1))
    m = re.search(r'ran [0-9.]+ emulated s \((\d+) cycles', out)
    if m:
        got['cycles'] = float(m.group(1))
    return got


def sched_report(path, top):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return '    (no scheduler dump: JIT_REGSTAT/JIT_SCHEDDUMP not active in this build)'
    p = subprocess.run([sys.executable, os.path.join(HERE, 'arm11_sched.py'), path, '--cycles', '--top', str(top)],
                       capture_output=True, text=True)
    return '\n'.join('    ' + l for l in (p.stdout or p.stderr).strip().splitlines()[:top + 8])


def main():
    args = sys.argv[1:]
    seconds, opt, cycles, dump, top = 2, None, False, False, 12  # opt None: the device's mask (DEVICE_OPT)
    cases = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--seconds':
            seconds = float(args[i + 1]); i += 2
        elif a == '--opt':
            opt = int(args[i + 1], 0); i += 2
        elif a == '--top':
            top = int(args[i + 1]); i += 2
        elif a == '--cycles':
            cycles = True; i += 1
        elif a == '--dump':
            dump = True; i += 1
        elif a.startswith('--'):
            print(__doc__); return
        else:
            cases.append(a); i += 1
    if not cases:
        print(__doc__); return
    for src in cases:
        name = os.path.basename(src)
        with tempfile.TemporaryDirectory() as d:
            try:
                dol = build_dol(src, d)
            except subprocess.CalledProcessError as e:
                print('%-16s assembly failed: %s' % (name, e)); continue
            sched = os.path.join(d, 'sched.bin') if cycles else None
            out = run(dol, seconds, opt, sched, dump)
            got = summarize(out)
            if not got:
                print('%-16s no JIT statistics; first lines:\n%s' % (name, '\n'.join(out.splitlines()[:6])))
                continue
            print('%-16s %s' % (name, '  '.join('%s %s' % (k, ('%.3f' % v) if v % 1 else ('%d' % v))
                                                for k, v in got.items())))
            if cycles:
                print(sched_report(sched, top))
            if dump:
                for line in out.splitlines():
                    if line.startswith('JIT_DUMP'):
                        print('    ' + line)


if __name__ == '__main__':
    main()
