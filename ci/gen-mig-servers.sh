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
      -D_DEFAULT_SOURCE -D_GNU_SOURCE )
      # _DEFAULT_SOURCE/_GNU_SOURCE: migcom uses BSD spellings (u_int, ...)
      # that glibc only exposes from <sys/types.h> under __USE_MISC. Its
      # Makefile targets FreeBSD, where they are unconditional, so building
      # it on the Linux toolchain container is the new case. Harmless on BSD
      # and macOS, which define them regardless.
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
