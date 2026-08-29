#!/bin/sh
#
# gen-mig-servers.sh -- generate the kernel's MIG server stubs from .defs.
#
# The Mach subsystem servers under sys/compat/mach were generated in 2015 by a
# MIG we no longer have and checked in as source, with no .defs beside them.
# That left the tree with no description of its own kernel RPC wire format:
# neither side could be regenerated, and nothing guaranteed the kernel server
# and the libmach client agreed -- which is the one thing MIG exists to
# provide. See nextbsd-kernel#125.
#
# This restores the normal arrangement, and matches what build.yml already
# says about this repo: "No generated source is ever committed to this repo --
# it carries patches only." It also mirrors the existing `sysent` step, which
# regenerates the syscall tables in the build env for the same reason.
#
# usage: gen-mig-servers.sh <srcdir> <userland-checkout>
#   srcdir    -- the laid-in kernel source tree (e.g. /usr/src/sys)
#   userland  -- a nextbsd-userland checkout, for migcom + the wire type .defs
#
set -eu

SRCDIR=${1:?usage: gen-mig-servers.sh <srcdir> <userland-checkout>}
ULAND=${2:?usage: gen-mig-servers.sh <srcdir> <userland-checkout>}

MIGSRC="$ULAND/src/bootstrap_cmds/migcom.tproj"
MACHINC="$ULAND/src/libmach/include"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- migcom ----
# Built on the BUILD HOST, not the target: the x86 runner cross-builds arm64,
# so the migcom inside nextbsd-userland-$ARCH.tar.gz is the wrong architecture.
# This mirrors Apple, where bootstrap_cmds is built before xnu.
#
# handler.c is deliberately excluded -- it is stale dead code referencing
# removed MIG concepts (IsCamelot, itLongForm) and is not in Apple's build
# phase. Compiling it fails.
# Parser/lexer generators: the toolchain container is Linux and may carry
# bison/flex, while a FreeBSD build host carries byacc/lex under those names.
# Accept either rather than hard-requiring one.
YACC=""
for c in bison byacc yacc; do command -v "$c" >/dev/null 2>&1 && { YACC=$c; break; }; done
LEXER=""
for c in flex lex; do command -v "$c" >/dev/null 2>&1 && { LEXER=$c; break; }; done
[ -n "$YACC" ]  || { echo "FAIL: no yacc-alike found (tried bison, byacc, yacc)"; exit 1; }
[ -n "$LEXER" ] || { echo "FAIL: no lex-alike found (tried flex, lex)"; exit 1; }
echo "==> generators: $YACC / $LEXER"

# migcom uses BSD type spellings (u_int and friends). On FreeBSD and macOS
# those arrive transitively from headers it already includes; on the Linux
# toolchain container they do not, and -D_DEFAULT_SOURCE alone does not help,
# because the problem is that <sys/types.h> is never pulled in on that path.
#
# PROBE rather than assume: only synthesise the types when the host genuinely
# lacks them. Defining them unconditionally would macro-replace a perfectly
# good typedef on hosts that do provide it. Vendored migcom source is never
# touched either way.
: > "$WORK/bsdtypes_shim.h"
cat > "$WORK/.probe.c" <<'PROBE'
#include <sys/types.h>
u_int  a; u_char b; u_short c; u_long d;
int main(void) { return 0; }
PROBE
if ${HOSTCC:-cc} -D_DEFAULT_SOURCE -D_GNU_SOURCE -fsyntax-only "$WORK/.probe.c" 2>/dev/null; then
    echo "==> host provides the BSD type spellings"
else
    echo "==> host lacks BSD type spellings -- synthesising them"
    cat > "$WORK/bsdtypes_shim.h" <<'SHIM'
#ifndef NEXTBSD_BSDTYPES_SHIM_H
#define NEXTBSD_BSDTYPES_SHIM_H
#include <sys/types.h>
typedef unsigned int   u_int;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned long  u_long;
#endif
SHIM
fi

echo "==> building migcom from $MIGSRC"
cp "$MIGSRC"/*.c "$MIGSRC"/*.h "$MIGSRC"/*.l "$MIGSRC"/*.y "$WORK/"
rm -f "$WORK/handler.c"
( cd "$WORK"
  # -d emits the token header lexxer.l #includes as "parser.h".
  "$YACC" -d -o parser.c parser.y
  "$LEXER" -o lexxer.c lexxer.l
  ${HOSTCC:-cc} -w -o migcom \
      parser.c lexxer.c error.c global.c header.c mig.c routine.c server.c \
      statement.c string.c type.c user.c utils.c \
      -I. -I"$MACHINC" \
      -DMIG_TYPE_CHECK=1 \
      -DMIG_VERSION='"bootstrap_cmds-138-freebsd"' \
      -D__private_extern__= \
      -D_DEFAULT_SOURCE -D_GNU_SOURCE \
      -include "$WORK/bsdtypes_shim.h" )
echo "    migcom: $("$WORK/migcom" -version 2>&1 | head -1)"

# ------------------------------------------------------------- generate -----
# Include path resolution:
#   <mach/std_types.defs>, <mach/mach_types.defs>  -> userland (shared wire types)
#   <mach_debug/mach_debug_types.defs>             -> this repo
mkdir -p "$WORK/incl/mach" "$WORK/incl/mach_debug"
cp "$MACHINC"/mach/*.defs                       "$WORK/incl/mach/"
cp "$SRCDIR"/sys/mach/mach_port.defs            "$WORK/incl/mach/"
cp "$SRCDIR"/sys/mach_debug/mach_debug_types.defs "$WORK/incl/mach_debug/"

gen_one() {
    sub=$1
    out="$SRCDIR/compat/mach/${sub}_server.c"
    echo "==> generating $sub -> $out"
    # NEXTBSD_KERNEL_SERVER, deliberately NOT KERNEL_SERVER: we want the kernel
    # server WITHOUT Apple's modern *_from_user entry-point renames, which this
    # kernel does not implement.
    ${HOSTCC:-cc} -E -x c -D__MACH30__ -DNEXTBSD_KERNEL_SERVER=1 \
        -I "$WORK/incl" "$WORK/incl/mach/${sub}.defs" \
      | "$WORK/migcom" \
            -server "$out" \
            -user /dev/null \
            -header "$SRCDIR/sys/mach/${sub}_gen.h"
    [ -s "$out" ] || { echo "FAIL: $out empty"; exit 1; }
    echo "    $(wc -l < "$out") lines"
}

gen_one mach_port

echo "==> MIG generation complete"
