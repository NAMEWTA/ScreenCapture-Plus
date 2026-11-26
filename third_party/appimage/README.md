# Original sources for the rebuilt AppImage runtime

The reviewed input set is `runtimeSourceBuild` in
[`build/package/dependencies.lock.json`](../../build/package/dependencies.lock.json).
The existing `tools.appimageRuntime` pin is unchanged. The source builder makes
a new runtime; it does not identify or reproduce the static libraries contained
in the older official binary.

`libfuse-mount.patch` is the exact original
[`patches/libfuse/mount.c.diff`](https://github.com/AppImage/type2-runtime/blob/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/patches/libfuse/mount.c.diff)
from the fixed type-2 runtime. The original dependency script pins fuse 3.15.0
and squashfuse 0.5.2. The builder checks both original archive SHA256 values and
compares this patch with the actual restored type-2 source before building.

`toolchain-patches/` retains the 26 original patches selected by the canonical
musl-cross-make recipe for GCC 9.4.0, binutils 2.44 and musl 1.2.6. These files are
verbatim source patches, without local implementation edits.
[`toolchain-patches/upstream.json`](toolchain-patches/upstream.json) records each
original path and Git blob at
[`richfelker/musl-cross-make@5e8042e`](https://github.com/richfelker/musl-cross-make/tree/5e8042e63de29f8473fc3b0b831a587ba284e1a8).
The builder compares every retained patch with the actual fixed Git source;
the offline contract independently verifies every original Git blob hash.

[`source-reviews.json`](source-reviews.json) identifies the original canonical
cross-make archive SHA1 records and the separately reviewed SHA256 records.
Both hashes are checked against the actual downloaded archives. Library Git
sources require their full commit and original annotated tag object, including
the peeled commit. Git object identity is the claim being made here; a tag's
presence is not represented as signature verification.

Original license texts remain in every complete source input. The source builder
also produces `LICENSE.runtime-sources` by combining the original notices for
the runtime and every final static component. It retains GCC's runtime exception
text as well as its license text. Patch licensing remains the upstream licensing
of the affected sources and the original upstream distribution.

See [`docs/appimage-runtime-source-build.md`](../../docs/appimage-runtime-source-build.md)
for the exact build, static input evidence, native probe and packaging contract.
