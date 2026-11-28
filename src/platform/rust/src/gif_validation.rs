//! Strict, bounded use of the pinned gif decoder. No GIF/LZW parser is
//! implemented here; see third_party/gif/upstream.json for the reviewed API.

use crate::cancellation::HuhuCancellation;
use crate::lease::{open_regular_file, MAX_FILE_BYTES};
use crate::receive::HuhuReceive;
use crate::status::{self, Error, Result};
use gif::{ColorOutput, DecodeOptions, DecodingError, MemoryLimit};
use std::fs::File;
use std::io::{self, Read};
use std::num::NonZeroU64;
use std::path::Path;

const CHUNK_PIXELS: usize = 64 * 1024;
const MAX_METADATA_BYTES: u32 = 16 * 1024 * 1024;
const MAX_GIF_PIXELS: u64 = (u16::MAX as u64) * (u16::MAX as u64);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HuhuGifLimits {
    pub max_input_bytes: u64,
    pub max_canvas_pixels: u64,
    pub max_total_pixels: u64,
    pub max_frames: u32,
    pub max_metadata_bytes: u32,
}

impl HuhuGifLimits {
    fn validate(&self) -> Result<()> {
        if self.max_input_bytes == 0 || self.max_input_bytes > MAX_FILE_BYTES
            || self.max_canvas_pixels == 0 || self.max_canvas_pixels > MAX_GIF_PIXELS
            || self.max_total_pixels == 0 || self.max_frames == 0
            || self.max_metadata_bytes == 0 || self.max_metadata_bytes > MAX_METADATA_BYTES {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid GIF validation budgets"));
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HuhuGifInfo {
    pub width: u32,
    pub height: u32,
    pub frames: u32,
    pub reserved: u32,
    pub decoded_pixels: u64,
    pub duration_ms: u64,
}

#[derive(Debug)]
struct InputFailure(i32);
impl std::fmt::Display for InputFailure {
    fn fmt(&self, output: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(output, "GIF input stopped ({})", self.0)
    }
}
impl std::error::Error for InputFailure {}

fn check_cancel(receive: Option<&HuhuReceive>, cancellation: Option<&HuhuCancellation>) -> Result<()> {
    if cancellation.is_some_and(HuhuCancellation::is_requested) {
        return Err(Error::new(status::CANCELLED, "GIF validation cancelled"));
    }
    if let Some(receive) = receive {
        if receive.is_cancelled()? {
            return Err(Error::new(status::CANCELLED, "GIF validation cancelled"));
        }
    }
    Ok(())
}

struct CheckedInput<'a> {
    file: File,
    bytes: u64,
    maximum: u64,
    receive: Option<&'a HuhuReceive>,
    cancellation: Option<&'a HuhuCancellation>,
}

impl Read for CheckedInput<'_> {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        if output.is_empty() { return Ok(0); }
        loop {
            check_cancel(self.receive, self.cancellation)
                .map_err(|error| io::Error::other(InputFailure(error.status)))?;
            if self.bytes > self.maximum {
                return Err(io::Error::other(InputFailure(status::LIMIT_EXCEEDED)));
            }
            // At most one byte beyond the limit is inspected, then rejected.
            // This also handles an externally growing file after the fstat.
            let capacity = output.len().min((self.maximum - self.bytes + 1) as usize);
            let read = match self.file.read(&mut output[..capacity]) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                result => result?,
            };
            self.bytes += read as u64;
            if self.bytes > self.maximum {
                return Err(io::Error::other(InputFailure(status::LIMIT_EXCEEDED)));
            }
            return Ok(read);
        }
    }
}

fn decode_error(error: DecodingError) -> Error {
    let code = match &error {
        DecodingError::Io(error) => {
            if let Some(failure) = error.get_ref().and_then(|error| error.downcast_ref::<InputFailure>()) {
                failure.0
            } else if matches!(error.kind(), io::ErrorKind::UnexpectedEof | io::ErrorKind::InvalidData) {
                status::INVALID_MEDIA
            } else { status::IO }
        }
        // gif 0.13.3 exposes its explicit MemoryLimit rejection as a format
        // message, not a separate error variant. Keep this classification tied
        // to that pinned upstream source instead of treating a budget as EOF.
        DecodingError::Format(_) if error.to_string() == "memory limit reached" => status::LIMIT_EXCEEDED,
        DecodingError::Format(_) => status::INVALID_MEDIA,
    };
    Error::new(code, error.to_string())
}

pub fn validate_file(path: &str, limits: &HuhuGifLimits, receive: Option<&HuhuReceive>,
    cancellation: Option<&HuhuCancellation>) -> Result<HuhuGifInfo> {
    limits.validate()?;
    check_cancel(receive, cancellation)?;
    if path.is_empty() { return Err(Error::new(status::INVALID_ARGUMENT, "GIF path is empty")); }
    let (file, original) = open_regular_file(Path::new(path))?;
    if original.len() == 0 { return Err(Error::new(status::INVALID_MEDIA, "GIF is empty")); }
    if original.len() > limits.max_input_bytes {
        return Err(Error::new(status::LIMIT_EXCEEDED, "GIF exceeds its input budget"));
    }
    let mut options = DecodeOptions::new();
    options.set_color_output(ColorOutput::Indexed);
    options.set_memory_limit(MemoryLimit::Bytes(NonZeroU64::new(limits.max_metadata_bytes as u64)
        .ok_or_else(|| Error::new(status::INVALID_ARGUMENT, "GIF metadata budget is empty"))?));
    options.check_frame_consistency(true);
    options.check_lzw_end_code(true);
    options.skip_frame_decoding(false);
    options.allow_unknown_blocks(false);
    let input = CheckedInput { file, bytes: 0, maximum: limits.max_input_bytes, receive, cancellation };
    let mut decoder = options.read_info(input).map_err(decode_error)?;
    let mut info = HuhuGifInfo { width: decoder.width() as u32, height: decoder.height() as u32,
        ..HuhuGifInfo::default() };
    let canvas = u64::from(info.width) * u64::from(info.height);
    if canvas == 0 { return Err(Error::new(status::INVALID_MEDIA, "GIF canvas has zero dimensions")); }
    if canvas > limits.max_canvas_pixels {
        return Err(Error::new(status::LIMIT_EXCEEDED, "GIF canvas exceeds its pixel budget"));
    }
    let mut pixels = [0_u8; CHUNK_PIXELS];
    loop {
        check_cancel(receive, cancellation)?;
        let (width, height, delay, transparent) = match decoder.next_frame_info().map_err(decode_error)? {
            Some(frame) => (frame.width, frame.height, frame.delay, frame.transparent),
            None => break, // The upstream reader reaches None only after Trailer.
        };
        let count = u64::from(width) * u64::from(height);
        if count == 0 { return Err(Error::new(status::INVALID_MEDIA, "GIF frame has zero dimensions")); }
        info.frames = info.frames.checked_add(1)
            .filter(|frames| *frames <= limits.max_frames)
            .ok_or_else(|| Error::new(status::LIMIT_EXCEEDED, "GIF exceeds its frame budget"))?;
        info.decoded_pixels = info.decoded_pixels.checked_add(count)
            .filter(|pixels| *pixels <= limits.max_total_pixels)
            .ok_or_else(|| Error::new(status::LIMIT_EXCEEDED, "GIF exceeds its cumulative pixel budget"))?;
        info.duration_ms = info.duration_ms.checked_add(u64::from(delay) * 10)
            .ok_or_else(|| Error::new(status::LIMIT_EXCEEDED, "GIF duration overflow"))?;
        let colors = decoder.palette().map_err(decode_error)?.len() / 3;
        if colors == 0 || transparent.is_some_and(|index| index as usize >= colors) {
            return Err(Error::new(status::INVALID_MEDIA, "GIF has an invalid palette index"));
        }
        let mut remaining = count;
        while remaining != 0 {
            check_cancel(receive, cancellation)?;
            let amount = remaining.min(CHUNK_PIXELS as u64) as usize;
            if !decoder.fill_buffer(&mut pixels[..amount]).map_err(decode_error)? {
                return Err(Error::new(status::INVALID_MEDIA, "GIF frame is truncated"));
            }
            if pixels[..amount].iter().any(|index| *index as usize >= colors) {
                return Err(Error::new(status::INVALID_MEDIA, "GIF pixels exceed their palette"));
            }
            remaining -= amount as u64;
        }
        // Drive LZW through DataEnd with a nonempty output, rather than let
        // next_frame_info discard remaining compressed bytes with Output::None.
        // Any additional decoded pixel is inconsistent with the descriptor.
        if decoder.fill_buffer(&mut pixels[..1]).map_err(decode_error)? {
            return Err(Error::new(status::INVALID_MEDIA, "GIF frame contains excess pixels"));
        }
    }
    if info.frames == 0 { return Err(Error::new(status::INVALID_MEDIA, "GIF contains no image frames")); }
    let mut input = decoder.into_inner();
    let mut extra = [0_u8; 1];
    if input.read(&mut extra).map_err(|error| decode_error(error.into()))? != 0 {
        return Err(Error::new(status::INVALID_MEDIA, "GIF has data after its trailer"));
    }
    let input = input.into_inner();
    if input.bytes != original.len() || input.file.metadata()?.len() != original.len() {
        return Err(Error::new(status::LENGTH_MISMATCH, "GIF changed during validation"));
    }
    check_cancel(receive, cancellation)?;
    Ok(info)
}
