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
