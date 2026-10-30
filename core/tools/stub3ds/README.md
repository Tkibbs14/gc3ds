# A stub `<3ds.h>` for syntax-checking host3ds files without devkitARM

`core/host3ds/` only builds with devkitARM, so a machine without it (a cloud session, say) cannot compile those files
at all - and an edit that does not compile wastes a round trip to whoever has the toolchain. This stub declares just
enough of libctru for `gcc -fsyntax-only` to parse a file and report its warnings:

```bash
gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -Icore/tools/stub3ds -Icore/src core/host3ds/jitsample.c
```

It is **not** a build and proves nothing about behaviour: it catches what a compiler catches - a `tot` used outside
the block that declares it (which broke a build on 2026-10-30), an unbalanced brace, a format that does not match its
argument, an unused variable. Add declarations as a file needs them; files that pull in citro3d or the portlibs
(`main.c`, `pica.c`, `relay.c`) need more than this and are still the toolchain's job.
