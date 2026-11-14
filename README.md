# Xenon Utils

A System V style set of 45 core utilities, written in C99 against
POSIX-2001/XSI. MIT licensed. First target: GNU/Linux.

```sh
make -f Makefile.gnulinux
make -f Makefile.gnulinux check
make -f Makefile.gnulinux install PREFIX=/usr/local
```

## The 45 utilities

"Basic 45" has no standard definition, so this is the chosen set:

    basename cat chgrp chmod chown cmp comm cp cut date dd df dirname du
    echo env expr false head id ln ls mkdir mkfifo mv nice od paste printf
    pwd rm rmdir sleep sort tail tee test touch tr true tty uname uniq wc
    who

`[` is provided as a link to `test` and is not counted. Deferred candidates:
cksum, csplit, expand, fold, join, logname, nl, nohup, pathchk, pr, split,
tsort, unexpand.

## Design

- **POSIX, not a historical system.** Where IRIX, UnixWare and POSIX differ,
  POSIX wins. Output follows the standard's formats, so `wc` and `uniq -c`
  are unpadded and `ls -l`, `du` and `df` count 512-byte units by default.
- **No libc assumptions.** The build defines `_POSIX_C_SOURCE=200112L` and
  `_XOPEN_SOURCE=600` with `-std=c99 -pedantic`, so using an extension is a
  compile error. `_GNU_SOURCE` and friends are never defined.
- **Minimal shim.** `lib/` holds only what POSIX-2001 lacks or what several
  tools would duplicate: a strict non-permuting `getopt`, `getdelim` (which
  arrived in 2008), the `chmod` mode compiler, the tree walker and
  diagnostics. Platform facts live in `lib/plat_gnulinux.c`.
- **One binary, several names.** As on IRIX, `cp`, `ln` and `mv` are hard
  links to one binary and `argv[0]` selects behaviour. That binary is linked
  statically, so it still runs while shared libraries are being moved; set
  `MV_LDFLAGS=` to disable this. `chgrp`/`chown` and `[`/`test` share binaries
  in the same way.
- **No thread-local storage.** Single threaded, with no `__thread` or
  `_Thread_local`. The only TLS involved is the C library's own `errno`.

- **Depth is unlimited.** POSIX-2001 has no `openat`, so tree tools move
  through the hierarchy with `chdir` and work on short relative names, with
  heap-allocated walker state. `..` is verified by device and inode on the
  way back up, and a walk that loses its place stops instead of carrying on
  somewhere else. `cp -R` tracks its source and destination positions
  separately. The working directory is unreliable inside a walk callback,
  so callbacks must use `ent->access` for system calls.

## Style

Follows `CODING_STYLE.md`. That guide's rules and quick reference require
four-space indentation while its examples show eight; this code uses four.
GCC attributes appear only in `include/xn.h`, behind `__GNUC__`, and expand
to nothing elsewhere.

## Known limitations

These are real gaps, listed so that they can be closed.

- **Multibyte locales.** `tr` and `sort -t` work on bytes, and `cut -n` is
  accepted without effect. `wc -m`, `cut -c`, `ls` and `expr` are
  multibyte aware. All tools are exact in single-byte locales.
- **Sub-second times.** `cp -p`, `mv` across devices and `touch -r` keep
  whole seconds, because `st_atim` and `utimensat` postdate POSIX-2001.
- **Symbolic links.** `cp -p` cannot preserve a link's own times, as
  POSIX-2001 has no `lutimes`.
- **`sort`** holds all input in memory, and `-m` sorts rather than merges.
- **`who`** reads only the system database; it takes no file operand.

## Provenance

This is an independent implementation, written from the POSIX-2001
specification. The IRIX and UnixWare trees are proprietary; no code was taken
from them. Only their command inventories, build recipes and file names were
consulted, not their C sources. The EBCDIC tables in `dd` are the ones
specified by POSIX.

## Testing

`tests/run.sh` checks exit status and output against POSIX behaviour. It
expects a GNU-like host for its harness commands (`seq`, `stat`, `awk`).
