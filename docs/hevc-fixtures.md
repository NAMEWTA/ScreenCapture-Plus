# HEVC file validation fixtures

Windows recording already selects HEVC when the native encoder supports it and
can fall back to H.264. Complete-file inspection therefore includes the fixed
FFmpeg HEVC decoder and parser on both platforms. The Windows recording backend
and Media Foundation cover adapter keep their existing roles. An unavailable
HEVC cover decoder must produce an actual failure; the tests do not skip that
failure or count an undecodable preview as compatible behavior.

## Independent encoder and shared bytes

The original 15 H.264/AAC and malformed-MOV fixtures remain in `manifest.json`.
`scripts/build-hevc-fixtures.py` creates an additional `hevc-manifest.json` from
real x265 Main and Main10 output. Both suites share the same inspection test
executable and cross-platform artifact. HEVC input is 352 × 288 at 25 fps, with
251 actual pictures, BT.709 SDR signaling and full-range samples. The Main10
encoder receives 10-bit little-endian samples; the fixture generator checks the
result's actual HEVC profile and decoded pixel format before sharing it.

The encoder tools are isolated CI dependencies. The pinned x265 4.1 source is
restored from VideoLAN's release archive and checked against the SHA-512 recorded
by [VLC's x265 dependency](https://github.com/videolan/vlc/blob/master/contrib/src/x265/SHA512SUMS).
The reviewed file hashes and exact checksum source blob are retained in
[`upstream.json`](../third_party/hevc-fixtures/upstream.json). Its CLI input,
encoder-drain and NAL-output implementations were read at the recorded 4.1
mirror commit; restoration checks their Git blob hashes against the official
archive before building. A source mismatch stops the job for review.

Two separate x265 executables provide the 8-bit and 10-bit encoders. Each links
its own x265 library statically and uses one frame thread without a worker pool.
The source still explicitly selects old CMake policies, so this build uses the
Ubuntu runner's official `/usr/bin/cmake` 3.x and records its actual package
version and executable hash. The product's CMake and dependency prefixes are
not modified to accommodate that upstream build system.

The same fixed FFmpeg 9.0.2 source already restored for software-media validation
builds a separate static CLI that only stream-copies elementary HEVC and the
existing AAC track into MP4. It retains the controlled strict-MOV patch. No
FFmpeg encoder or hardware backend is enabled in this CLI, and neither x265 nor
the CLI is linked into or distributed with the application. The application
continues to validate with its own configured libraries. The deterministic raw
pattern reuses `fill_yuv_image` from the existing MIT-licensed FFmpeg mux example;
no HEVC syntax, decoder or container writer is implemented locally.
These elementary fixtures disable B frames, so the upstream `setts` bitstream
filter can assign their explicit equal PTS/DTS and 1/25-second durations before
muxing. This avoids relying on a muxer's recovery for absent timestamps. The
original H.264 fixtures continue to exercise delayed decoder output.

## Cases and retained evidence

The ten HEVC cases cover Main, Main10, fast-start MP4, two video/audio track
pairs, a damaged late video sample at each bit depth, damage in the second video
or second audio track, a physically truncated final sample, and a truncated
terminal container box. The existing remux and corruption helpers operate on
actual encoded packet offsets. Valid files must decode all 251 or 502 video
frames. A valid first frame must still produce a cover when a later sample is
damaged. The malformed file itself must be rejected for archive admission.

`hevc-source.json` records the encoder/remux binaries' SHA-256 values, actual
version output, reviewed source hashes, exact command arguments, raw and
elementary-stream hashes, and finished MP4 hashes. These records are also embedded
in the HEVC manifest. Runtime results retain cover PNGs and actual accept/reject
outcomes, including failures, in a separate evidence directory.

After generating the original fixtures, the additional build entry point is:

```sh
python3 scripts/build-hevc-fixtures.py \
  --fixtures artifacts/video-fixtures \
  --generator artifacts/cmake/video-validation/video_inspection_fixtures
```

The shared test program selects the additional manifest with a fourth argument:

```sh
artifacts/cmake/video-validation/qt_video_inspection \
  artifacts/video-fixtures artifacts/video-inspection/hevc --hevc
```

This suite establishes file compatibility only when the actual platform run
passes. Main10 here is SDR: it does not establish HDR tone mapping, native HEVC
hardware encoder availability, capture throughput or complete recording UX.
The development dependency bundle and any local system-FFmpeg command checks
also do not replace the fixed-library Windows and Linux validation jobs.

The x265 source retains its GPL-2.0-or-later notices and
[`COPYING.x265`](../third_party/hevc-fixtures/COPYING.x265). FFmpeg's LGPL notice
and the complete MIT mux-example notice accompany the generated evidence.
