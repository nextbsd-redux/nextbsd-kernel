#!/bin/sh
# VENDORED from nextbsd-userland/ci/assemble-image.sh (nextbsd#369, nextbsd#376).
# TODO(#376): this is a copy — assemble-image.sh should live in a shared repo
# consumed by both nextbsd-userland and nextbsd-kernel, not duplicated. Keep in
# sync with the userland copy until then.
# assemble-image.sh — Phase 1: assemble a minimal, CI-only bootable NextBSD .img
# NATIVELY on the Linux runner, from the four already-cross-built artifacts, with
# no FreeBSD VM. See pkgdemon.github.io/nextbsd-native-image-assembly-plan.html.
#
# This is the "prove the assembly" step. It ports build.sh's makefs/mkimg recipe
# (the only FreeBSD-host pieces) to host-built tools, ingests the cross-built
# userland tarball instead of rebuilding it in a VM, and skips the pkg/toolchain
# phase and the live ISO entirely. amd64 first; arm64 is a follow-up (Phase 4).
#
# Inputs (env):
#   ARCH        amd64 | arm64                       (default amd64)
#   WORK        scratch dir                          (default /tmp/nbimg)
#   OUT         output dir for disk.img              (default $WORK/out)
#   BASE_TGZ    nextbsd-base-$ARCH.tar.gz            (compat base = whole rootfs)
#   KERNEL_TGZ  nextbsd-kernel-$ARCH.tar.gz          (-> /boot/kernel/kernel)
#   MODULES_TGZ space-separated kext tarball(s)      (-> /System/Library/Extensions)
#   USERLAND_TGZ nextbsd-userland-$ARCH.tar.gz       (Darwin system layer)
#   OVERLAY     dir whose contents overlay the rootfs (authoritative /etc + plists)
#   SRC         freebsd-src checkout                 (default /usr/src)
# Output: $OUT/disk.img
set -eu

ARCH="${ARCH:-amd64}"
WORK="${WORK:-/tmp/nbimg}"
OUT="${OUT:-$WORK/out}"
SRC="${SRC:-/usr/src}"
ROOTFS="$WORK/rootfs"
TOOLS="$WORK/tools"            # host-built makefs/mkimg land here
case "$ARCH" in
  amd64)  TARGET_ARCH=amd64;  EFI_NAME=BOOTX64.EFI ;;
  arm64)  TARGET_ARCH=aarch64; EFI_NAME=BOOTAA64.EFI ;;
  *) echo "unsupported ARCH=$ARCH" >&2; exit 1 ;;
esac

log() { echo "==> $*"; }

# ---------------------------------------------------------------------------
# 1. Host-build makefs + mkimg (Linux binaries) from /usr/src.
#    These are FreeBSD bootstrap tools that build+run on a non-FreeBSD host.
#    Pattern mirrors how the cross build host-builds migcom. The exact
#    incantation is the most likely Phase-1 iteration point; keep it isolated.
# ---------------------------------------------------------------------------
build_host_tools() {
    if command -v makefs >/dev/null 2>&1 && command -v mkimg >/dev/null 2>&1 \
       && command -v pwd_mkdb >/dev/null 2>&1 && command -v cap_mkdb >/dev/null 2>&1; then
        log "makefs/mkimg/pwd_mkdb/cap_mkdb already on PATH"; return
    fi
    : "${CROSS_BINDIR:?CROSS_BINDIR must be set (the kernel-toolchain image provides it)}"
    MKPY="./tools/build/make.py --cross-bindir=$CROSS_BINDIR TARGET=$ARCH TARGET_ARCH=$TARGET_ARCH"
    # makefs/mkimg are host (build) tools that already live in the baked /usr/src.
    # They compile fine in the _legacy stage but link against the build-tool libs
    # in obj-tools (libnetbsd/libutil/libsbuf). The baked kernel-toolchain built
    # libnetbsd/libutil (the kernel needs them) but NOT libsbuf — so first run
    # bootstrap-tools to populate the full build-tool lib set, then the _legacy
    # stage builds + links makefs/mkimg into WORLDTMP/legacy as runner-native bins.
    # makefs links -lsbuf; the baked kernel-toolchain has libnetbsd/libutil in
    # obj-tools but NOT libsbuf. makefs's link line also searches the general
    # -L${WORLDTMP}/legacy/usr/lib, so build libsbuf there by listing it FIRST in
    # LOCAL_LEGACY_DIRS (the legacy stage builds dirs in order), before makefs/mkimg
    # which then resolve -lsbuf from legacy/usr/lib.
    # pwd_mkdb + cap_mkdb join the list: the native image has no FreeBSD VM to
    # generate /etc/pwd.db,spwd.db (from master.passwd) and /etc/login.conf.db,
    # so build them host-native too and run them offline in fixup_rootfs. Without
    # them getpwnam/login can't resolve users and login.conf has no .db.
    log "_legacy: build libsbuf + makefs + mkimg + pwd_mkdb + cap_mkdb as host tools"
    ( cd "$SRC" && $MKPY \
        LOCAL_LEGACY_DIRS="lib/libsbuf usr.sbin/makefs usr.bin/mkimg usr.sbin/pwd_mkdb usr.bin/cap_mkdb" \
        _legacy )
    mkdir -p "$TOOLS"
    for name in makefs mkimg pwd_mkdb cap_mkdb; do
        b="$(find /usr/obj -type f -name "$name" -perm -u+x 2>/dev/null | head -1)"
        [ -n "$b" ] && install -m755 "$b" "$TOOLS/$name" \
            || { echo "ERROR: no host $name after bootstrap-tools + _legacy" >&2; exit 1; }
    done
    export PATH="$TOOLS:$PATH"
    log "host tools: $(command -v makefs) | $(command -v mkimg)"
}

# ---------------------------------------------------------------------------
# 2. Stage the rootfs: base = whole tree, then kernel/modules/userland, then
#    the authoritative overlay last (last writer wins, matching build.sh).
# ---------------------------------------------------------------------------
stage_rootfs() {
    log "staging rootfs at $ROOTFS"
    rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"
    tar -C "$ROOTFS" -xzf "$BASE_TGZ"
    apple_private_layout
    mkdir -p "$ROOTFS/boot/kernel" "$ROOTFS/System/Library/Extensions"
    # kernel -> /boot/kernel/kernel
    tmpk="$WORK/k"; rm -rf "$tmpk"; mkdir -p "$tmpk"; tar -C "$tmpk" -xzf "$KERNEL_TGZ"
    kbin="$(find "$tmpk" -type f -name kernel | head -1)"
    [ -n "$kbin" ] || { echo "ERROR: kernel binary not found in $KERNEL_TGZ" >&2; exit 1; }
    install -m555 "$kbin" "$ROOTFS/boot/kernel/kernel"
    # driver kexts -> /System/Library/Extensions
    for m in ${MODULES_TGZ:-}; do [ -f "$m" ] && tar -C "$ROOTFS/System/Library/Extensions" -xzf "$m"; done
    # Darwin userland (raw tar rooted at /) — includes the MAINTAINED overlay
    # (LaunchDaemons, services/protocols, usr/tests). The user-editable /etc
    # (accounts, sshd_config, pam.d, fstab, the loader fragment) is NOT in the
    # package any more; it is seeded from nextbsd-overlays next (mirrors build.sh).
    tar -C "$ROOTFS" -xzf "$USERLAND_TGZ"
    seed_overlays
    apple_private_runtime
}

# Apple /private layout (build.sh:107-118): /private/{etc,var,tmp} are the REAL
# dirs; /etc,/var,/tmp are relative symlinks into them. WITHOUT THIS the overlay's
# /private/etc (master.passwd, gettytab, login.conf, pam.d) is unreachable via
# /etc, so getty/login/PAM all fail and the image halts before the login banner.
apple_private_layout() {
    mkdir -p "$ROOTFS/private"
    for pd in etc var tmp; do
        if [ -d "$ROOTFS/$pd" ] && [ ! -L "$ROOTFS/$pd" ]; then
            mv "$ROOTFS/$pd" "$ROOTFS/private/$pd"      # relocate if the base shipped it
        else
            mkdir -p "$ROOTFS/private/$pd"
        fi
        ln -s "private/$pd" "$ROOTFS/$pd"
    done
    # launchctl -w job-overrides DB dir (build.sh:118) so launchd loads cleanly.
    mkdir -p "$ROOTFS/private/var/db/launchd.db/com.apple.launchd"
}

# Seed the pkg-free, admin-owned /etc + loader fragment from nextbsd-overlays —
# split out of the package so pkg upgrade can't clobber it (mirrors build.sh's
# seed step). SEED_ROOTFS is a checkout of nextbsd-overlays/rootfs provided by
# the workflow (cp -R onto the /private/etc laid out by apple_private_layout).
seed_overlays() {
    if [ -z "${SEED_ROOTFS:-}" ] || [ ! -d "$SEED_ROOTFS" ]; then
        echo "ERROR: SEED_ROOTFS not set/found — /etc seed missing, login would have no users" >&2
        exit 1
    fi
    cp -R "$SEED_ROOTFS/." "$ROOTFS/"
    [ -f "$ROOTFS/private/etc/master.passwd" ] && chmod 0600 "$ROOTFS/private/etc/master.passwd"
    log "seeded /etc from nextbsd-overlays ($SEED_ROOTFS)"
}

# /var skeleton + utmpx session files (build.sh:120-138). Without utx.active/
# utx.log, PAM's pam_open_session fails ("Unable to write the utmp record" ->
# "system error") and login aborts before execing root's shell. Runs AFTER the
# overlay; these paths follow the /var -> private/var symlink.
apple_private_runtime() {
    mkdir -p "$ROOTFS/var/run" "$ROOTFS/var/log" "$ROOTFS/var/db" \
             "$ROOTFS/var/empty" "$ROOTFS/var/tmp" "$ROOTFS/tmp" "$ROOTFS/dev"
    chmod 1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"
    : > "$ROOTFS/var/run/utx.active"
    : > "$ROOTFS/var/log/utx.lastlogin"
    : > "$ROOTFS/var/log/utx.log"
    chmod 644 "$ROOTFS/var/run/utx.active" \
              "$ROOTFS/var/log/utx.lastlogin" "$ROOTFS/var/log/utx.log"
    # root's home (master.passwd: root -> /root). Without it login warns
    # "No home directory. Logging in with home = /". build.sh:145 does the same.
    # FreeBSD ships no pam_mkhomedir, so login won't create it — we must.
    mkdir -p "$ROOTFS/root"; chmod 0700 "$ROOTFS/root"
}

# ---------------------------------------------------------------------------
# 3. Fix up the staged tree for boot: kext model, ownership, offline DBs.
#    NextBSD loads drivers as KEXTS (OSKext/kextd), not klds — so strip the
#    .ko/firmware tree and set kext perms; no kldxref needed at boot.
# ---------------------------------------------------------------------------
fixup_rootfs() {
    log "fixup: strip .ko/firmware, kext perms, offline DBs"
    rm -f "$ROOTFS"/boot/kernel/*.ko 2>/dev/null || true
    rm -rf "$ROOTFS/boot/firmware" 2>/dev/null || true
    # kext auth: root:wheel, group/other not writable
    if [ -d "$ROOTFS/System/Library/Extensions" ]; then
        chmod -R go-w "$ROOTFS/System/Library/Extensions" || true
    fi
    # offline passwd / login.conf DBs, generated into the REAL /private/etc (the
    # /etc symlink resolves there at runtime). host pwd_mkdb/cap_mkdb come from
    # build_host_tools; x86 host -> little-endian db works for amd64 + arm64.
    etc="$ROOTFS/private/etc"
    if [ -f "$etc/master.passwd" ]; then
        pwd_mkdb -p -d "$etc" "$etc/master.passwd" \
          || { echo "ERROR: pwd_mkdb failed — login cannot resolve users" >&2; exit 1; }
    else
        echo "WARN: no $etc/master.passwd — login will have no users" >&2
    fi
    [ -f "$etc/login.conf" ] && { cap_mkdb "$etc/login.conf" || echo "WARN: cap_mkdb failed" >&2; }
    # TLS trust anchor: the base ships the raw certs in /usr/share/certs/trusted
    # but /etc/ssl/cert.pem (the bundle fetch/openssl verify against) was stripped
    # with /etc, so every https fetch failed "certificate verify failed". certctl
    # is a target sh script we can't run on the Linux builder, so build the bundle
    # directly — concatenating the trusted PEMs is exactly what certctl's bundle
    # step does. (Runtime `certctl rehash` still refreshes it if certs change.)
    if ls "$ROOTFS"/usr/share/certs/trusted/*.pem >/dev/null 2>&1; then
        mkdir -p "$etc/ssl"
        cat "$ROOTFS"/usr/share/certs/trusted/*.pem > "$etc/ssl/cert.pem" \
          && log "generated /etc/ssl/cert.pem ($(ls "$ROOTFS"/usr/share/certs/trusted/*.pem | wc -l | tr -d ' ') trusted certs)" \
          || echo "WARN: could not build /etc/ssl/cert.pem" >&2
    else
        echo "WARN: no /usr/share/certs/trusted — https verification will fail" >&2
    fi
    # OSKext authentication REQUIRES kexts AND their enclosing path to be
    # root:wheel (uid/gid 0). The base/userland/kernel-modules tarballs carry
    # their build-runner uid (e.g. 1001) and `tar -x` preserves it, so the
    # staged kexts are 1001:1001 -> kextd load fails "authentication problems"
    # (OSReturn 0xdc00800d) and NO driver binds (e1000 stays unattached, 0 NICs).
    # chown the whole staged tree to 0:0; makefs has no -F manifest so it
    # packages this ownership verbatim. (No setuid bits in this tree to clear.)
    chown -R 0:0 "$ROOTFS"
}

# ---------------------------------------------------------------------------
# 4. makefs UFS root + FAT ESP, then mkimg GPT — ported verbatim from build.sh.
# ---------------------------------------------------------------------------
assemble() {
    mkdir -p "$OUT"
    log "makefs ffs (root)"
    makefs -t ffs -B little -o version=2,label=ROOTFS,softupdates=1 -b 512m \
        "$WORK/rootfs.ufs" "$ROOTFS"

    log "EFI System Partition ($EFI_NAME)"
    espdir="$WORK/esp-stage"; rm -rf "$espdir"; mkdir -p "$espdir/EFI/BOOT"
    if   [ -f "$ROOTFS/boot/loader_lua.efi" ]; then cp "$ROOTFS/boot/loader_lua.efi" "$espdir/EFI/BOOT/$EFI_NAME"
    elif [ -f "$ROOTFS/boot/loader.efi" ];     then cp "$ROOTFS/boot/loader.efi"     "$espdir/EFI/BOOT/$EFI_NAME"
    else echo "ERROR: no loader.efi in rootfs/boot" >&2; exit 1; fi
    makefs -t msdos -o fat_type=32 -o sectors_per_cluster=1 -o volume_label=EFISYS \
        -s 33292k "$WORK/esp.img" "$espdir"

    log "mkimg GPT -> $OUT/disk.img"
    if [ "$ARCH" = amd64 ]; then
        for f in boot/pmbr boot/gptboot; do
            [ -f "$ROOTFS/$f" ] || { echo "ERROR: rootfs/$f missing" >&2; exit 1; }
        done
        mkimg -s gpt -f raw \
            -b "$ROOTFS/boot/pmbr" \
            -p freebsd-boot/bootfs:="$ROOTFS/boot/gptboot" \
            -p efi/efiboot0:="$WORK/esp.img" \
            -p freebsd-ufs/ROOTFS:="$WORK/rootfs.ufs" \
            -o "$OUT/disk.img"
    else
        # arm64: UEFI only — no pmbr/gptboot
        mkimg -s gpt -f raw \
            -p efi/efiboot0:="$WORK/esp.img" \
            -p freebsd-ufs/ROOTFS:="$WORK/rootfs.ufs" \
            -o "$OUT/disk.img"
    fi
    ls -lh "$OUT/disk.img"
}

build_host_tools
stage_rootfs
fixup_rootfs
assemble
log "done: $OUT/disk.img"
