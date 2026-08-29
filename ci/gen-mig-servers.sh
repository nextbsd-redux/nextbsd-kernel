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

# migcom uses BSD type spellings (u_int and friends) WITHOUT including
# <sys/types.h> -- on FreeBSD and macOS the header arrives transitively from
# something else it includes, and on the Linux toolchain container it does not.
#
# So the shim ALWAYS pulls <sys/types.h> in; that is the actual fix. Only the
# typedef fallback is conditional, for a libc where the header still does not
# provide the BSD spellings. An earlier version made the whole shim conditional
# on a probe, which was the wrong question: the probe asked "does <sys/types.h>
# define u_int" (yes, everywhere) rather than "does migcom see it" (no, on
# Linux), so it passed and the shim was empty. Vendored source stays untouched.
cat > "$WORK/bsdtypes_shim.h" <<'SHIM'
#ifndef NEXTBSD_BSDTYPES_SHIM_H
#define NEXTBSD_BSDTYPES_SHIM_H
#include <sys/types.h>
SHIM

cat > "$WORK/.probe.c" <<'PROBE'
#include <sys/types.h>
u_int  a; u_char b; u_short c; u_long d;
int main(void) { return 0; }
PROBE
if ${HOSTCC:-cc} -D_DEFAULT_SOURCE -D_GNU_SOURCE -fsyntax-only "$WORK/.probe.c" 2>/dev/null; then
    echo "==> <sys/types.h> supplies the BSD spellings; shim only needs the include"
else
    echo "==> <sys/types.h> lacks the BSD spellings; shim will define them"
    cat >> "$WORK/bsdtypes_shim.h" <<'SHIM'
typedef unsigned int   u_int;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned long  u_long;
SHIM
fi
echo '#endif' >> "$WORK/bsdtypes_shim.h"

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

    # Kernel include prologue.
    #
    # migcom emits the USERLAND include set -- <string.h> and <mach/*.h>.
    # NextBSD's kernel keeps those headers under sys/mach/, so the 2015
    # servers carried a hand-adapted, _KERNEL-guarded prologue. Pure MIG
    # output cannot reproduce that, which is exactly the "may carry
    # hand-edits" risk called out in #125; this reapplies it mechanically
    # so it is reproducible instead of manual.
    #
    #   <mach/X.h>        -> <sys/mach/X.h>   (all ten exist there)
    #   <mach/boolean.h>  -> dropped          (no such kernel header; the
    #                                          frozen _KERNEL branch omits it)
    #   <string.h>        -> dropped          (frozen _KERNEL branch omits it;
    #                                          the generated server calls no
    #                                          str/mem function -- only
    #                                          mig_strncpy_zerofill, inside a
    #                                          __has_include guard that
    #                                          compiles out in the kernel)
    before=$(grep -c '#include <mach/' "$out" || true)
    [ "$before" -gt 0 ] || {
        echo "FAIL: $out has no <mach/...> includes to rewrite -- migcom output"
        echo "      changed shape; re-check this transform before trusting it."
        exit 1
    }
    sed -i.bak \
        -e '/#include <string\.h>/d' \
        -e '/#include <mach\/boolean\.h>/d' \
        -e 's|#include <mach/\(.*\)>|#include <sys/mach/\1>|' \
        "$out"
    rm -f "$out.bak"

    if grep -q '#include <mach/' "$out" || grep -q '#include <string\.h>' "$out"; then
        echo "FAIL: userland includes survived the kernel prologue rewrite:"
        grep -n '#include <mach/\|#include <string\.h>' "$out" | head
        exit 1
    fi
    # migcom also never emits the NextBSD kernel headers the frozen server
    # carried below its _KERNEL block. Without them the file rewrites cleanly
    # and then fails to compile on ipc_info_space_basic_t, IP_VALID,
    # ipc_port_check_circularity and friends. Splice them back in after the
    # first include group, matching the frozen prologue.
    #
    #   ipc_sync.h / ipc_host.h / ipc_tt.h / ipc_mig.h / ipc/ipc_voucher.h
    #       reach ipc/ipc_port.h, which defines IP_VALID and
    #       ipc_port_check_circularity
    #   mach_debug/mach_debug_types.h
    #       reaches mach_debug/ipc_info.h for the ipc_info_* types
    anchor='#include <sys/mach/mig_errors.h>'
    grep -q "$anchor" "$out" || {
        echo "FAIL: anchor '$anchor' not in $out -- migcom output changed shape."
        exit 1
    }
    awk -v anchor="$anchor" '
        { print }
        index($0, anchor) && !done {
            print "#include <sys/mach/ipc_sync.h>"
            print "#include <sys/mach/ipc/ipc_voucher.h>"
            print "#include <sys/mach/ipc_host.h>"
            print "#include <sys/mach/ipc_tt.h>"
            print "#include <sys/mach/ipc_mig.h>"
            print "#include <sys/mach_debug/mach_debug_types.h>"
            done = 1
        }
    ' "$out" > "$out.new" && mv "$out.new" "$out"

    # The frozen prologue opens with these two; migcom emits neither, and the
    # rest of the kernel headers assume them.
    awk -v anchor="$anchor" '
        NR == 1 { print "#include <sys/cdefs.h>"; print "#include <sys/types.h>" }
        { print }
    ' "$out" > "$out.new" && mv "$out.new" "$out"

    echo "    rewrote $before userland include(s) to the kernel prologue"
    echo "    spliced 6 NextBSD kernel include(s) migcom does not emit"
    echo "    $(wc -l < "$out") lines"
}

gen_one mach_port

echo "==> MIG generation complete"
