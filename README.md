# nextbsd-kernel

NextBSD kernel: source patches (`patches/`), custom config (`config/NEXTBSD`),
and the build workflow. Builds run inside the
[`nextbsd-kernel-toolchain`](https://github.com/nextbsd-redux/nextbsd-kernel-toolchain)
container, which already carries the exact baked `/usr/src`.

## Layout

```
patches/
  series      # ordered patch list (one filename per line, no comments)
  *.patch     # git format-patch diffs
config/
  NEXTBSD     # kernel config (include GENERIC; ident NEXTBSD)
.github/workflows/build.yml
```

## Triggers

- `repository_dispatch: toolchain-updated` — full rebuild on a new toolchain
  image (new upstream source); uses the SHA-pinned image and **cascades to a
  module rebuild**.
- `push` to `patches/**` or `config/**` — kernel-only rebuild on the
  `amd64-latest` image; does **not** rebuild modules.
- `workflow_dispatch` — manual.

The kernel is built with `NO_MODULES=yes`; the `/usr/obj` tree is uploaded as
`kernel-obj-amd64` for the module job to layer onto.

See [`patches/README.md`](patches/README.md) for the patch workflow.
