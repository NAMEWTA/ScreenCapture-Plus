# Shared CPU frame normalization

`services/media/FrameNormalizer` is the one worker-owned geometry conversion
used by live-source previews and available to the scroll and recording workers.
It accepts owned `Core::FrameLease` input and produces another bounded lease;
Qt is not part of this interface. Native buffers have already been returned by
the capture adapter before these CPU leases reach a consumer.

The operation order is native crop, native orientation, then the user's
selection in the displayed source image. All eight native orientations are
supported, including reflection before counter-clockwise rotation. Positive,
negative and padded strides are validated by `MediaFrame`. BGRA8 and RGBA8 are
the actual negotiated PipeWire formats supported here. The common BGRA path
uses a row copy; transpose paths use the attributed FFmpeg tiled traversal.
Unknown formats return `UnsupportedFormat`.

The output is BGRA. An opaque source produces alpha 255; straight and
premultiplied alpha retain their actual semantics. The original source ID,
session ID, clock domain, timestamp and color description are copied. Native
crop and orientation are cleared only after their pixels have been transformed.
This layer does not infer sRGB, tone-map HDR or perform color-space conversion.
Encoding color conversion remains the fixed codec library's responsibility.

Every retained output generation counts against the `FramePool` budget.
Changing the source or selection cannot overwrite storage held by an earlier
consumer. A busy pool returns `Busy`; budget and allocation failures are
distinct. Callers preserve their last complete image on any failed conversion.

## Validation actually performed

The C++20 tests have been built and run with GCC 14 and
`-Wall -Wextra -Wpedantic -Werror`. They check all eight transforms for BGRA and
RGBA, padded and bottom-up input, native crop followed by selection, exact
opaque/straight/premultiplied alpha, preservation of source metadata, held
leases and pool reuse. Their pixel fixtures are generated in the test from
documented coordinate colors and explicit expected orientation tables.

This verifies the shared CPU operation. It does not establish physical monitor
rotation, compositor color accuracy or hardware HDR support. See
`third_party/ffmpeg-transpose/upstream.md` for exact source and license.
