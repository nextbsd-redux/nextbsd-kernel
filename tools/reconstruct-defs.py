#!/usr/bin/env python3
"""
reconstruct-defs.py -- recover a MIG .defs from a frozen generated server.

ONE-SHOT ARCHAEOLOGY TOOL. Not part of the build, and not needed to maintain
a .defs once it exists -- a .defs is hand-authored source from then on, the
same as Apple's own osfmk/mach/*.defs.

It exists because sys/compat/mach carries seven MIG servers that were
generated in 2015 by a MIG we no longer have, and checked in as source with
no .defs beside them (nextbsd-kernel#125). This reconstructs the missing
input so both ends can be generated from one description again.

How it works, and why it is split this way:

  ORDER comes from the frozen server's dispatch table. msgh_id is subsystem
  base + routine index, so the order IS the ABI. NextBSD's ordering is its
  own -- it differs from XNU's in 34 of 36 positions for mach_port -- so
  Apple's .defs cannot be substituted wholesale.

  SIGNATURES come from Apple's .defs for the same subsystem, because they
  carry the type qualifiers (intran, CountInOut, inout, dealloc) that cannot
  be recovered from generated C. Verify them against the frozen server's
  __Request__/__Reply__ structs afterwards -- for mach_port, 35/35 agreed.

Two things that bit us on mach_port and will bite again:

  1. Apple guards kernel entry-point names behind KERNEL_SERVER as
     <name>_from_user. This kernel implements the plain names, so those
     blocks are collapsed to the #else spelling (--collapse-from-user).
     KERNEL_SERVER must then still be DEFINED at generation time, because it
     also activates the intran/destructor translations in mach_types.defs.
     Generating without it silently passes the raw request port where an
     ipc_space_t is required, which page-faults in ipc_right_lookup(). The
     ABI gate cannot see that -- the wire format is identical. See #131/#133.

  2. The frozen servers carry a hand-adapted include prologue that pure MIG
     output does not reproduce. ci/gen-mig-servers.sh reapplies it.

usage:
  reconstruct-defs.py --subsystem mach_port \
      --frozen src-overlay/sys/compat/mach/mach_port_server.c \
      --apple  /path/to/xnu/osfmk/mach/mach_port.defs \
      --out    src-overlay/sys/sys/mach/mach_port.defs \
      --msgids src-overlay/sys/sys/mach/mach_port.msgids
"""
import argparse, io, re, sys


def frozen_table(path, subsystem):
    """(base, end, [(routine|None, argc|None), ...]) from the dispatch table."""
    s = io.open(path, encoding='utf-8', errors='replace').read()
    m = re.search(re.escape(subsystem) + r'_subsystem = \{(.*?)\n\};', s, re.S)
    if not m:
        sys.exit("no %s_subsystem block in %s" % (subsystem, path))
    body = m.group(1)
    nums = re.findall(r'^\s*(\d+),\s*$', body, re.M)
    base, end = int(nums[0]), int(nums[1])
    slots = []
    for e in re.finditer(
            r'\(mig_stub_routine_t\)\s*_X(\w+),\s*(\d+)'
            r'|\{0,\s*0,\s*0,\s*0,\s*0,\s*0\}', body):
        slots.append((e.group(1), int(e.group(2))) if e.group(1) else (None, None))
    return base, end, slots


def apple_routines(path, collapse_from_user):
    """name -> declaration text, from Apple's .defs."""
    src = re.sub(r'/\*.*?\*/', '', io.open(path, encoding='utf-8',
                                           errors='replace').read(), flags=re.S)
    marks = [m.start() for m in
             re.finditer(r'^\s*(routine|simpleroutine|skip;)', src, re.M)] + [len(src)]
    out = {}
    for a, b in zip(marks, marks[1:]):
        t = src[a:b]
        if re.match(r'^\s*skip;', t) or ');' not in t:
            continue
        t = t[:t.rfind(');') + 2]
        if collapse_from_user:
            # #ifdef KERNEL_SERVER <name>_from_user(...) #else <plain>(...) #endif
            t = re.sub(
                r'#ifdef KERNEL_SERVER\n(?:(?!#else|#endif).)*?_from_user\('
                r'(?:(?!#else|#endif).)*?#else\n((?:(?!#endif).)*?)#endif\n',
                lambda m: m.group(1), t, flags=re.S)
        ks = re.search(r'#ifdef KERNEL_SERVER(.*?)#else(.*?)#endif', t, re.S)
        name = None
        if ks:
            m2 = re.search(r'^\s*(\w+)\s*\(', ks.group(2), re.M)
            name = m2.group(1) if m2 else None
        if name is None:
            m3 = re.match(r'^\s*(?:routine|simpleroutine)\s+(\w+)\s*\(', t)
            name = m3.group(1) if m3 else None
        if name is None:
            m4 = re.search(r'^\s*(\w+)\s*\(', t, re.M)
            name = m4.group(1) if m4 else None
        if name:
            out[name] = '\n'.join(l.rstrip() for l in t.strip().split('\n') if l.strip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--subsystem', required=True)
    ap.add_argument('--frozen', required=True)
    ap.add_argument('--apple', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--msgids')
    ap.add_argument('--collapse-from-user', action='store_true', default=True)
    ap.add_argument('--subst', action='append', default=[],
                    help='OLD=NEW type substitution, repeatable')
    a = ap.parse_args()

    base, end, slots = frozen_table(a.frozen, a.subsystem)
    blocks = apple_routines(a.apple, a.collapse_from_user)
    subst = dict(s.split('=', 1) for s in a.subst)

    missing = [n for n, _ in slots if n and n not in blocks]
    if missing:
        print("WARNING: not found in Apple's .defs: %s" % ', '.join(missing),
              file=sys.stderr)

    body = []
    for i, (name, _argc) in enumerate(slots):
        mid = base + i
        if name is None:
            body.append("skip;\t/* %d -- tombstone; preserves every msgh_id after it */\n" % mid)
            continue
        if name not in blocks:
            body.append("skip;\t/* %d %s -- NOT FOUND, RECONSTRUCT BY HAND */\n" % (mid, name))
            continue
        text = blocks[name]
        for o, n in subst.items():
            text = text.replace(o, n)
        body.append("/* %d */\n%s\n" % (mid, text))

    io.open(a.out, 'w', encoding='utf-8').write(
        "subsystem\n#if\tNEXTBSD_KERNEL_SERVER\n\tKernelServer\n#endif\n"
        "\t%s %d;\n\n#include <mach/std_types.defs>\n"
        "#include <mach/mach_types.defs>\n"
        "#include <mach_debug/mach_debug_types.defs>\n\n" % (a.subsystem, base)
        + '\n'.join(body))
    print("wrote %s: %d slots (%d routines, %d skips), base %d end %d"
          % (a.out, len(slots), sum(1 for n, _ in slots if n),
             sum(1 for n, _ in slots if n is None), base, end))

    if a.msgids:
        lines = ["# %s ABI contract -- captured from the frozen server." % a.subsystem,
                 "# msgh_id is base + index; a misplaced skip; silently renumbers",
                 "# everything after it and the kernel then runs the WRONG function.",
                 "# format: <msgid> <routine|skip> <argc>", "",
                 "subsystem %s %d %d" % (a.subsystem, base, end), ""]
        for i, (n, c) in enumerate(slots):
            lines.append("%d %s %s" % (base + i, n or 'skip', c if c is not None else '-'))
        io.open(a.msgids, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
        print("wrote %s" % a.msgids)


if __name__ == '__main__':
    main()
