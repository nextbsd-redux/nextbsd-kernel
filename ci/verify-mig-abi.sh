#!/bin/sh
#
# verify-mig-abi.sh -- assert a generated MIG server still matches its frozen
# ABI contract.
#
# msgh_id is subsystem base + routine index, so a misplaced or omitted `skip;`
# in a .defs silently renumbers every routine after it. The failure mode is the
# kernel executing the WRONG FUNCTION for a given message -- not an error, not
# a crash, just quietly wrong. Nothing else catches that, which is why this
# runs on every build rather than once at conversion time.
#
# usage: verify-mig-abi.sh <generated_server.c> <expected.msgids>
#
set -eu

GEN=${1:?usage: verify-mig-abi.sh <generated_server.c> <expected.msgids>}
EXP=${2:?usage: verify-mig-abi.sh <generated_server.c> <expected.msgids>}

[ -s "$GEN" ] || { echo "MIG-ABI-FAIL: $GEN missing or empty"; exit 1; }
[ -s "$EXP" ] || { echo "MIG-ABI-FAIL: $EXP missing or empty"; exit 1; }

sub=$(awk '$1=="subsystem"{print $2; exit}' "$EXP")
base=$(awk '$1=="subsystem"{print $3; exit}' "$EXP")
end=$(awk '$1=="subsystem"{print $4; exit}' "$EXP")

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Slice out the subsystem block. sed with a plain (non-dynamic) address keeps
# this portable across BSD and GNU awk/sed, which dynamic awk regexes are not.
sed -n "/${sub}_subsystem = {/,/^};/p" "$GEN" > "$TMP/blk"
[ -s "$TMP/blk" ] || { echo "MIG-ABI-FAIL: no ${sub}_subsystem block in $GEN"; exit 1; }

# base/end are the first two bare "<number>," lines in the block.
gbase=$(grep -E '^[[:space:]]*[0-9]+,[[:space:]]*$' "$TMP/blk" | sed -n 1p | tr -dc '0-9')
gend=$(grep  -E '^[[:space:]]*[0-9]+,[[:space:]]*$' "$TMP/blk" | sed -n 2p | tr -dc '0-9')

# Routine table, in order: either "_X<name>, <argc>" or a zero tombstone.
grep -oE '\(mig_stub_routine_t\)[[:space:]]*_X[A-Za-z0-9_]+,[[:space:]]*[0-9]+|\{0,[[:space:]]*0,[[:space:]]*0,[[:space:]]*0,[[:space:]]*0,[[:space:]]*0\}' "$TMP/blk" \
  | sed -e 's/.*_X\([A-Za-z0-9_]*\),[[:space:]]*\([0-9]*\)/\1 \2/' \
        -e 's/^{0,.*/skip -/' > "$TMP/got"

grep -v '^#' "$EXP" | awk 'NF && $1!="subsystem" {print $2, $3}' > "$TMP/exp"

fail=0
[ "$gbase" = "$base" ] || { echo "MIG-ABI-FAIL: base $gbase != expected $base"; fail=1; }
[ "$gend"  = "$end"  ] || { echo "MIG-ABI-FAIL: end $gend != expected $end";   fail=1; }

ng=$(wc -l < "$TMP/got" | tr -d ' ')
ne=$(wc -l < "$TMP/exp" | tr -d ' ')
if [ "$ng" != "$ne" ]; then
    echo "MIG-ABI-FAIL: generated has $ng slots, contract has $ne"
    fail=1
fi

# Slot-by-slot. paste keeps the two streams aligned without a nested read.
n=0
paste "$TMP/exp" "$TMP/got" | while IFS="$(printf '\t')" read -r want got; do
    n=$((n+1)); id=$((base + n - 1))
    [ "$want" = "$got" ] || echo "MIG-ABI-DIFF: msgid $id: contract '$want' != generated '$got'"
done > "$TMP/diffs"

if [ -s "$TMP/diffs" ]; then
    cat "$TMP/diffs"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "MIG-ABI-FAIL: $sub does not match its frozen contract -- this is an ABI break."
    echo "  If the change is intentional, update $EXP in the same commit and say why."
    exit 1
fi

echo "MIG-ABI-OK: $sub $base-$end, $ne slots match the frozen contract"
