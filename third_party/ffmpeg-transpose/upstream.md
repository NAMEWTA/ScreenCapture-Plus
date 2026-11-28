# CPU frame normalization source

The tiled 32-bit transpose traversal in
`src/services/media/FrameNormalizer.cpp` adapts `transpose_block_32_c` and the
8-by-8 traversal of [FFmpeg 9.0.2 `libavfilter/vf_transpose.c`](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b6dcd0331c8cc609192aeff5e1924f8/libavfilter/vf_transpose.c).

- Version: FFmpeg 9.0.2.
- Fixed commit: `946fcce07b6dcd0331c8cc609192aeff5e1924f8`.
- Git blob: `62c222f3c22328f79a9659137392cba2d78c8d38`.
- Copyright: 2010 Stefano Sabatini; 2008 Vitor Sessak.
- License: **LGPL-2.1-or-later**; complete original license in
  `COPYING.LGPLv2.1`.

The complete upstream file, including scalar block and edge traversal,
negotiation and direction handling, was read before adaptation. No libavfilter
runtime is introduced. The shared implementation retains the small tiled copy
strategy, uses signed horizontal and vertical source steps for all eight native
orientations, and uses `memcpy` instead of casting potentially unaligned pixels
to `uint32_t*`. The source's validated native crop is applied before orientation;
the user's selection is applied in the resulting displayed coordinates.

The project's existing `MediaFrame` and `FramePool` supply validation, lifetime
and retained-generation memory budgets. BGRA/RGBA channel order is explicit.
Color metadata and source timestamps are retained without tone mapping,
resampling or an inferred gamut conversion. Unsupported formats remain explicit
errors. The ordinary unrotated BGRA path copies rows without a second full-size
temporary image.
