# patches/

Kernel source patches applied on top of the baked `/usr/src`
(`git apply`), in the order listed in [`series`](series).

## `series` format

One patch filename per line, relative to this directory. **No comments or
blank lines** — the build applies the file verbatim with:

```sh
for p in $(cat patches/series); do git apply patches/$p; done
```

so anything in `series` is treated as a filename. Keep it to real patch names.
An empty `series` (the initial state) builds stock `releng/15.0` + the `NEXTBSD`
config — a clean baseline.

## Adding a patch

```sh
cd /usr/src                  # a FreeBSD checkout
# ... edit files ...
git format-patch -1 -o /path/to/nextbsd-kernel/patches/
echo "0001-my-change.patch" >> /path/to/nextbsd-kernel/patches/series
git -C /path/to/nextbsd-kernel commit -am "patch: my change" && git push
```

A push that only touches `patches/**` or `config/**` builds the **kernel**
but does **not** trigger a module rebuild — that only happens when the
toolchain/upstream source actually changes (`repository_dispatch`).

## Third-party patches

Patches not authored here keep their original `From:` line so authorship and
provenance survive. Record where they came from below, and re-check them when
the base branch moves &mdash; they are carried, not owned.

| patch | origin | upstream status |
|---|---|---|
| `0010-virtio_console-*` | `networkextension/freebsd-src` @ `cf50f191e`, branch `apple-vz-virtio-console` | not submitted |

`0010` is required on Apple's Virtualization.framework: VZ publishes no GOP and
its ACPI carries no SPCR, so a virtio-console consdev is the only console a
guest can have. Verified to apply cleanly to `releng/15.0` and to reference only
symbols present there (`cnadd(9)`).

A companion GIC patch (`aabce0c83`, detect the GIC version from `GICD_PIDR2`
when the MADT leaves it unspecified) was carried briefly and then dropped: VZ's
MADT on macOS 26.5.2 reports `GicVersion = 3`, so the fallback never fires.
See the plan for the measurement.
