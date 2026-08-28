---
name: mach-syscall
description: Working in the NextBSD kernel repo — where a change belongs (patches/ vs src-overlay/), how to add or wire a Mach syscall trap, and how to build and validate a kernel change without a hot-reload loop. Use when touching src-overlay/sys/compat/mach, adding a trap, changing config/NEXTBSD, or wondering why a change compiled but nothing happened.
---

# Kernel changes and Mach syscall traps

## Where a change belongs

**`patches/` patches FreeBSD's own kernel. `src-overlay/` is NextBSD's own kernel code.**

That is the whole rule, and it is about *intent*, not about whether the file is
new — three patches (`0015`, `0018`, `0025`) add brand-new files, because they
are FreeBSD-shaped drivers.

| Change | Goes in |
|---|---|
| Edit an existing freebsd-src file | `patches/` + a line in `patches/series` |
| New FreeBSD-shaped driver, wired via `sys/conf/files*` | `patches/` (wire it in the same patch) |
| Anything under `compat/mach`, `dev/iokit`, `sys/sys/mach`, `sys/apple`, `kern/vfs_pivot.c` | `src-overlay/sys/…` + a line in the matching `src-overlay/conf/files.*` |
| Option stock FreeBSD declares (`FUSEFS`, `UNIONFS`, …) | `config/NEXTBSD` |
| Option only the overlay declares (`COMPAT_MACH`, `COMPAT_LINUX`, `LINPROCFS`, `LINSYSFS`) | a wiring step in `build.yml` — **never** `config/NEXTBSD` |

### The trap in that last row

`nextbsd-kernel-modules` reuses `patches/series` and `config/NEXTBSD` to build a
KBI-matched kernel, and it **never lays in `src-overlay/`**. So an option the
overlay declares makes that repo's `config(8)` die with `unknown option`. Naming
them in the shared file is what broke both legs of the modules build on
2026-08-03. `config/NEXTBSD:114-122` says so; believe it.

Mechanically: `patches/` is `git apply`'d first (`build.yml:37-43`), then
`src-overlay/sys/` is `cp -R`'d over `/usr/src/sys/` (`build.yml:111`), then five
wiring steps `cat` fragments from `src-overlay/conf/` onto `sys/conf/files*`.
A patch can therefore never depend on an overlay file; the overlay may assume
patched base sources, and does.

Patches target **freebsd-src `releng/15.1`** (`build.yml:15`), baked into the
toolchain container. Application is plain `git apply` in `series` order — not
idempotent, no `--check`, no reverse detection. Safe only because every CI run
starts from a pristine tree. `series` order is authoritative and **not**
numerically sorted (`0021` currently precedes `0020`); no comments, no blank
lines.

## Adding a Mach syscall trap

### First, the trap that eats an afternoon

**`mach_module.c`'s `osx_syscalls[]` array is dead code for registration.**
`compat_shim.h` defines every `SYS_<name>` as `-1` (`NO_SYSCALL`), and
`kern_syscall_helper_register` treats `NO_SYSCALL` as the end-of-array sentinel —
so its loop exits on the first entry and registers nothing. Adding your trap
there does exactly nothing, silently.

All real wiring lives in `mach_syscall_wire.c`, which calls
`kern_syscall_register()` directly with `offset = NO_SYSCALL`, lets the kernel
pick a slot, and publishes it as `sysctl mach.syscall.<name>` for libmach's
`resolve_syscall()` to find at runtime.

Slot supply: 58 (10 stock `lkmnosys` + 48 from `patches/0001`), 15 in use. You
will not need `wire_one_at()` — it exists and has never been called.

### The procedure

**1. Args struct** — `src-overlay/sys/sys/mach/_mach_sysproto.h`.

Use the `PADL_/PADR_` pattern for *every* field:

```c
struct mach_port_get_attributes_trap_args {
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char flavor_l_[PADL_(int)]; int flavor; char flavor_r_[PADR_(int)];
	...
};
```

Bare fields **silently corrupt arguments** — 4-byte fields do not line up with
the kernel's 8-byte `register_t` slot writes, and values bleed across fields.
Zero-arg traps use `syscallarg_t dummy;`.

**Maximum 6 arguments.** FreeBSD's libc `syscall()` passes only 6 reliably on
amd64. If you need more, drop an argument instead — `mach_port_request_notification`
was redesigned to drop its `target`, and traps that need the task read
`current_task()->itk_space` rather than taking it as an argument.

**2. Handler** — `mach_traps.c` for the Apple trap family, or a new file under
`src-overlay/sys/compat/mach/` for a NextBSD-original subsystem.

Return `0` and put the Mach `kern_return_t` in `td->td_retval[0]`. Return
non-zero only for real errno propagation (a failed `copyin`/`copyout`).

**3. New `.c` file?** Add it to `src-overlay/conf/files.compat_mach`, keeping the
exact form and the alphabetical order:

```
compat/mach/<file>.c    optional compat_mach compile-with "${NORMAL_C} ${COMPAT_MACH_C}"
```

Its header claims it is generated; there is no generator in this repo. Hand-maintain it.

**4. Apple trap family?** Add `_MACH_KMOD_APPLE_SYSCALL_STUB(<name>);` to
`compat_shim.h`. Skip for NextBSD-original traps — `mach_wait_quiet` is not there.

**5. Six edits in `mach_syscall_wire.c`**, all required:

| | What |
|---|---|
| a | `struct <name>_args;` forward decl |
| b | `int sys_<name>(struct thread *, struct <name>_args *);` prototype |
| c | `static int sys_<name>_guarded(...)` wrapper |
| d | `static struct sysent <name>_sysent = { .sy_narg = N, .sy_call = …_guarded, … };` |
| e | `static int <name>_offset = NO_SYSCALL;` **and** `static struct sysent <name>_old_sysent;` |
| f | `SYSCTL_INT(_mach_syscall, OID_AUTO, <name>, CTLFLAG_RD, &<name>_offset, 0, "…");` |

**6. Register and deregister**, and this is the pair most often half-done:

- `wire_one("<name>", …)` — **appended last** in `mach_syscall_wire_register()`
- `unwire_one("<name>", …)` — **prepended first** in `mach_syscall_wire_deregister()`

The deregister list is the **exact reverse** of the register list. Check it:

```sh
sed -n '/^mach_syscall_wire_register/,/^}/p'   f | grep -oE 'wire_one\("[a-z_]+"'
sed -n '/^mach_syscall_wire_deregister/,/^}/p' f | grep -oE 'unwire_one\("[a-z_]+"'
```

The counts must match and the second must be the first reversed.

### Guard-wrapper shapes

Three established forms — pick the closest, don't invent an error code:

- **Needs Mach task state** — `mach_task_init_lazy()`, then bail with
  `KERN_INVALID_ARGUMENT` (4). The `_kernelrpc_mach_port_*` family uses
  `KERN_RESOURCE_SHORTAGE` (6); the `*_self_trap` family returns `0`
  (`MACH_PORT_NULL`).
- **Needs task *and* thread state** — also `mach_thread_init_lazy()`.
- **Needs neither** — thin pass-through, kept for symmetry (`mach_wait_quiet`).

### Naming

**The sysctl uses the userland name; the kernel symbol keeps Apple's
`_kernelrpc_*_trap` form.** `sysctl mach.syscall.mach_port_move_member` publishes
`_kernelrpc_mach_port_move_member_offset`, because libmach does
`resolve_syscall("mach_port_move_member")`. The string passed to `wire_one()` is
the userland form.

### Include order

`#include <sys/sysproto.h>` **must precede** `<sys/mach/_mach_sysproto.h>` in
every `.c`. Getting this backwards silently corrupts syscall arguments. There are
shouting comments about it in both `mach_syscall_wire.c` and `mach_busystate.c`.

### Before writing a new trap, check it isn't already there

Several handlers exist in `mach_traps.c` but were never wired — as of writing,
`_kernelrpc_mach_port_mod_refs_trap` and `_kernelrpc_mach_port_insert_member_trap`.
And several *implementations* exist in `ipc/mach_port.c` reachable only via MIG,
with no trap at all. Wiring an existing implementation is a fraction of the work
of writing one. `sysctl mach.syscall` on a booted image lists what is currently
exposed; `grep -n '^mach_port_' ipc/mach_port.c` lists what exists.

## Provenance — check before editing

Four strata under `src-overlay/sys/compat/mach/`, with different rules:

- **OSF/CMU Mach** (`ipc/*.c`, `kern/*.c`) — vendored 1991-1998. Edit in place,
  leave a `nextbsd#NNN` comment. Not frozen.
- **Apple/XNU, APSL** (`sys/sys/mach/*.h`, all of `sys/apple/`) — same.
- **NextBSD 2014-15, Matthew Macy** (`mach_traps.c`, `mach_task.c`, `mach_vm.c`, …).
- **MIG-generated** (`*_server.c`, `*_server.h`) — **do not hand-edit.** Frozen
  2015 artefacts; there are no `.defs` in this repo and no MIG in the build.

## Building and testing

**There is no hot-reload loop.** Mach is compiled *into* the kernel
(`options COMPAT_MACH`, `NO_MODULES=yes`), there is no `mach.ko`, and image
assembly deletes `/boot/kernel/*.ko`. Any change here costs a full kernel build
and a reboot. Budget ~10-13 min per CI iteration; arm64 is the long pole because
it builds two kernels.

**Open a PR — do not just push a branch.** A PR is the only event that assembles
a `disk.img` and boot-tests it. And because there is no `concurrency:` block and
`push` has no branch filter, pushing to a PR branch fires *two* full builds; the
`push` one is pure waste.

### Getting something you can run

```sh
gh run list --repo nextbsd-redux/nextbsd-kernel --branch <branch> --limit 5
# bootable image — PR runs only, amd64 only:
gh run download <run-id> -n nextbsd-ci-image-amd64      # 575 MB
# kernel binary for an existing VM — any run, either arch:
gh run download <run-id> -n nextbsd-kernel-arm64        # 153 MB
```

The kernel binary is inside at a path matching `*sys/NEXTBSD/kernel`.

Installing on a running VM:

```sh
# make a fallback FIRST — images ship without one
mkdir -p /boot/kernel.old && cp -p /boot/kernel/kernel /boot/kernel.old/kernel
install -m555 kernel /boot/kernel/kernel
reboot
```

If it does not boot, escape at the loader prompt with `boot /boot/kernel.old/kernel`.
**Know that escape before you install**, because a NextBSD image has no
`kernel.old` until you make one.

Caveat: kexts in `/System/Library/Extensions` are built against the last **main**
`continuous` obj tree. A PR kernel that changes KBI can make them fail to load,
and CI never sees it — the CI image has the same mismatched pairing.

### Green CI is not evidence

The boot test is `continue-on-error: true`. The **only** gate is grepping
`boot.log` for a getty `login:` line. Everything after it — the whole Mach and
userland marker census — is informational. A run has shipped green with
`FBSDGLUE-FAIL` and ~20 later markers never executed.

**Download the `boot-log` artifact and read it.** Compare the marker census
against a known-good run; a missing marker looks identical to a passing one in
the job summary. The Mach-specific ones are `MACH-SMOKE-OK`, `MACH-PORT-OK`,
`EVFILT-MACHPORT-OK`, `EVFILT-MACHPORT-CONCURRENT-OK`.

Also: the boot-test script is fetched **unpinned** from `nextbsd` main at run
time, so your PR's result can change without your PR changing. And the `disk.img`
is assembled from *other repos'* `continuous` artifacts — a red smoke test may be
a stale sibling, not your change. Check whether it reproduces on an unrelated PR.

### In-kernel tests nothing runs

`mach_test.c` is compiled into every kernel and exposes two sysctls that no CI
job and no on-image script ever reads:

```sh
sysctl mach.test_port_lifecycle       # returns ports cycled (default 100)
sysctl mach.test_in_kernel_mqueue     # returns kmsg enqueue/dequeue cycles
sysctl mach.test_port_lifecycle=10000 # bump N first (max 100000) to stress
```

A returned count below the requested N is a failure. Use them after any IPC
change — they are the cheapest real signal available, and currently free.

`sysctl mach.debug_enable=1` gives verbose Mach logging, but floods the emulated
UART and perturbs timing-sensitive handshakes. Do not leave it on during a boot test.

## Publishing

`publish` is main-only and refreshes the rolling `continuous` prerelease. It
needs the arm64 leg, so arm64 is load-bearing even for an amd64-only change.
A merged kernel change does **not** automatically reach an ISO — `nextbsd-pkg`
auto-triggers on `userland-updated` only, so a kernel-only change needs a manual
`workflow_dispatch` there.

`main` is not branch-protected. Nothing mechanically stops a merge on red.
