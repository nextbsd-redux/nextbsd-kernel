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
# Copy every .defs, not just the one being generated: clock.defs includes
# clock_types.defs, and the other subsystems have similar local dependencies.
cp "$SRCDIR"/sys/mach/*.defs                     "$WORK/incl/mach/"
cp "$SRCDIR"/sys/mach_debug/mach_debug_types.defs "$WORK/incl/mach_debug/"

gen_one() {
    sub=$1
    out="$SRCDIR/compat/mach/${sub}_server.c"
    echo "==> generating $sub -> $out"
    # KERNEL_SERVER must be defined. It does two unrelated jobs in Apple's
    # headers: it selects the *_from_user entry-point names, and it activates
    # the intran/destructor translations in mach_types.defs. We collapsed the
    # name blocks in mach_port.defs so only the translations remain -- without
    # them the dispatch passes the raw request port where an ipc_space_t is
    # required and the kernel page-faults in ipc_right_lookup(). See the header
    # of mach_port.defs. NEXTBSD_KERNEL_SERVER still gates the KernelServer
    # subsystem directive.
    ${HOSTCC:-cc} -E -x c -D__MACH30__ -DNEXTBSD_KERNEL_SERVER=1 -DKERNEL_SERVER=1 \
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
    #   <string.h>        -> dropped          (the frozen servers carry it only
    #                                          in their non-_KERNEL branch, so
    #                                          the kernel build never saw it
    #                                          either)
    #
    # Dropping <string.h> is safe even though mach_host, host_priv and task DO
    # call memcpy()/strlen() -- 2, 6 and 4 times respectively. The frozen
    # servers make exactly the same calls the same number of times and compile
    # today, resolving them through the kernel's own headers (sys/systm.h,
    # reached transitively via sys/mach/*.h) rather than <string.h>. Verified by
    # counting the calls on both sides, not assumed: an earlier version of this
    # comment claimed the generated servers called no str/mem function at all,
    # which was true only while mach_port and clock were the only ones
    # generated.
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
    # Two subsystems need one more header apiece, for the kernel entry points
    # their own dispatch calls:
    #
    #   task    task_deallocate(), convert_task_to_port()  -> sys/mach/task.h
    #   vm_map  the vm_*_t wire types                      -> sys/mach/vm_types.h
    #
    # The frozen servers carry these; they came from import directives in the
    # 2015 .defs that our reconstructions do not have. Without them the file
    # compiles until it reaches the subsystem's own entry points and then dies
    # on implicit declarations -- 19 of them for task alone. Determined by
    # diffing frozen against generated includes, not guessed: mach_host,
    # host_priv and mach_vm need nothing extra and get nothing.
    case "$sub" in
    task)   extra_inc="#include <sys/mach/task.h>" ;;
    vm_map) extra_inc="#include <sys/mach/vm_types.h>" ;;
    *)      extra_inc="" ;;
    esac

    awk -v anchor="$anchor" -v extra="$extra_inc" '
        { print }
        index($0, anchor) && !done {
            print "#include <sys/mach/ipc_sync.h>"
            print "#include <sys/mach/ipc/ipc_voucher.h>"
            print "#include <sys/mach/ipc_host.h>"
            print "#include <sys/mach/ipc_tt.h>"
            print "#include <sys/mach/ipc_mig.h>"
            print "#include <sys/mach_debug/mach_debug_types.h>"
            if (extra != "")
                print extra
            done = 1
        }
    ' "$out" > "$out.new" && mv "$out.new" "$out"

    if [ -n "$extra_inc" ] && ! grep -qF "$extra_inc" "$out"; then
        echo "FAIL: $sub needs '$extra_inc' and the splice did not land it"
        exit 1
    fi

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

# clock: 3 routines, base 1000, no tombstones -- the smallest subsystem, so it
# is the first conversion after mach_port. Its .defs was reconstructed in #138
# and verified ABI-identical to the frozen clock_server.c: routine table
# identical, all 3 __Request__/__Reply__ struct pairs identical field for
# field, verify-mig-abi.sh passing against both generated and frozen output.
gen_one clock

# The remaining five subsystems, which completes the conversion -- every Mach
# server the kernel builds is now generated from a .defs in this tree.
#
#   mach_host  200-225   25 slots
#   host_priv  400-426   26 slots
#   task       3400-3442 42 slots
#   vm_map     3800-3831 31 slots
#   mach_vm    4800-4820 20 slots
#
# verify-mig-abi.sh passes for all five -- base, end and every routine slot
# match the .msgids contract, tombstones included. That is the check that
# matters: msgh_id is base + index, so a misplaced `skip;` would silently point
# a message at the wrong function.
#
# The __Request__ structs are NOT byte-identical to the frozen ones, and that is
# expected. Every frozen request carries a `mach_msg_body_t msgh_body` between
# the "kernel processed data" markers with zero descriptors in it -- an artifact
# of the 2015 MIG. Modern migcom omits it for non-complex requests. mach_port is
# the control: its frozen server had the same spurious field, it was converted
# first, and its nine routines are verified working on hardware. Dropping the
# field is what the already-shipping subsystem does.
#
# These five also have no MIG client to disagree with: nextbsd-userland carries
# no .defs for them, and libmach's entry points still fabricate success (#93).
# So this changes what the kernel is BUILT from, not what any caller sends.
#
# host_priv, task, vm_map and mach_vm emit 16 unguarded ipc_port_release_send()
# calls between them, where the frozen servers guarded each one with IP_VALID.
# That is #136, fixed in this same change by moving the check into the callee
# as Apple did -- without it the first null or dead port panics the kernel.
#
# vm_map and mach_vm still carry #137's two stale LP64 wire sizes. Those are
# reproduced deliberately: the .defs encodes the frozen numbers, so generated
# and frozen agree exactly. Converting neither fixes nor worsens #137.
gen_one mach_host
gen_one host_priv
gen_one task
gen_one vm_map
gen_one mach_vm

echo "==> MIG generation complete"
