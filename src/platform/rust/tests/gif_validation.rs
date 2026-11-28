//! Strict validation via the exported ABI and ordinary OS files. Valid test
//! streams are written by the pinned upstream gif encoder; faults are explicit.
use gif::{AnyExtension, Encoder, Frame};
use huhucapture_native::{huhu_cancellation_create, huhu_cancellation_free,
    huhu_cancellation_request, huhu_gif_validate_file, HuhuCancellation, HuhuGifInfo, HuhuGifLimits};
use std::borrow::Cow;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = -1;
const CANCELLED: i32 = -4;
const INVALID_MEDIA: i32 = -9;
const LIMIT_EXCEEDED: i32 = -13;
const PALETTE: &[u8] = &[0, 0, 0, 255, 255, 255];

struct Input(PathBuf);
impl Input {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("huhu-gif-验证-{}-{}-{stamp}.part",
            std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        std::fs::File::options().create_new(true).write(true).open(&path).unwrap();
        Self(path)
    }

    fn validate(&self, data: &[u8], limits: &HuhuGifLimits) -> (i32, HuhuGifInfo) {
        std::fs::write(&self.0, data).unwrap();
        let path = self.0.to_str().unwrap();
        let mut info = HuhuGifInfo { width: 9, height: 9, frames: 9, reserved: 9, decoded_pixels: 9, duration_ms: 9 };
        let result = unsafe { huhu_gif_validate_file(path.as_ptr(), path.len(), limits,
            ptr::null(), ptr::null(), &mut info) };
        if result != OK {
            assert_eq!((info.width, info.height, info.frames, info.reserved, info.decoded_pixels, info.duration_ms),
                (0, 0, 0, 0, 0, 0), "failed validation must not expose a successful partial summary");
        }
        (result, info)
    }
}
impl Drop for Input { fn drop(&mut self) { let _ = std::fs::remove_file(&self.0); } }

fn limits() -> HuhuGifLimits {
    HuhuGifLimits { max_input_bytes: 1024 * 1024, max_canvas_pixels: 1024 * 1024,
        max_total_pixels: 10 * 1024 * 1024, max_frames: 100, max_metadata_bytes: 1024 * 1024 }
}

fn frame(width: u16, height: u16, pixels: Vec<u8>) -> Frame<'static> {
    Frame { width, height, buffer: Cow::Owned(pixels), ..Frame::default() }
}

fn encode(width: u16, height: u16, frames: &[Frame<'_>]) -> Vec<u8> {
    let mut encoder = Encoder::new(Vec::new(), width, height, PALETTE).unwrap();
    for frame in frames { encoder.write_frame(frame).unwrap(); }
    encoder.into_inner().unwrap()
}

fn animation() -> Vec<u8> {
    let mut first = frame(8, 8, (0..64).map(|i| (i % 2) as u8).collect());
    first.interlaced = true;
    first.delay = 2;
    let mut second = frame(2, 3, vec![0, 1, 1, 0, 0, 1]);
    second.left = 6;
    second.top = 5;
    second.interlaced = true;
    second.palette = Some(PALETTE.to_vec());
    second.delay = 3;
    encode(8, 8, &[first, second])
}

#[test]
fn full_interlaced_animation_and_local_rectangles_are_decoded() {
    let (code, info) = Input::new().validate(&animation(), &limits());
    assert_eq!(code, OK);
    assert_eq!((info.width, info.height, info.frames), (8, 8, 2));
    assert_eq!(info.decoded_pixels, 70);
    assert_eq!(info.duration_ms, 50);
}

#[test]
fn every_strict_prefix_is_rejected_including_missing_trailer() {
    let data = animation();
    assert_eq!(data.last(), Some(&0x3b));
    let input = Input::new();
    for prefix in 0..data.len() {
        assert_eq!(input.validate(&data[..prefix], &limits()).0, INVALID_MEDIA,
            "accepted a truncated GIF prefix of {} / {} bytes", prefix, data.len());
    }
    assert_eq!(input.validate(&data, &limits()).0, OK);
    let mut appended = data;
    appended.extend_from_slice(b"not part of the GIF");
    assert_eq!(input.validate(&appended, &limits()).0, INVALID_MEDIA);
}

#[test]
fn lzw_end_code_and_exact_pixel_count_are_required() {
    let input = Input::new();
    let mut encoder = Encoder::new(Vec::new(), 1, 1, PALETTE).unwrap();
    // min-code-size=2, CLEAR=4 and one pixel=0 fit in one byte. No END=5.
    // Upstream's encoded-frame API supplies framing; this is deliberately
    // malformed LZW data, not a parser or alternate production codec.
    encoder.write_lzw_pre_encoded_frame(&frame(1, 1, vec![2, 0x04])).unwrap();
    let without_end = encoder.into_inner().unwrap();
    assert_eq!(input.validate(&without_end, &limits()).0, INVALID_MEDIA);

    let too_many = encode(1, 1, &[frame(1, 1, vec![0, 1])]);
    // The real upstream encoder permits an oversized input buffer and encodes
    // every supplied index. The validator must reject the extra decoded pixel.
    assert_eq!(input.validate(&too_many, &limits()).0, INVALID_MEDIA);
    let mut encoder = Encoder::new(Vec::new(), 2, 1, PALETTE).unwrap();
    // CLEAR=4, pixel=0, END=5; only one pixel for a two-pixel descriptor.
    encoder.write_lzw_pre_encoded_frame(&frame(2, 1, vec![2, 0x44, 0x01])).unwrap();
    assert_eq!(input.validate(&encoder.into_inner().unwrap(), &limits()).0, INVALID_MEDIA);
}

#[test]
fn zero_dimensions_bad_palettes_and_out_of_bounds_frames_are_rejected() {
    let input = Input::new();
    assert_eq!(input.validate(&encode(0, 1, &[frame(1, 1, vec![0])]), &limits()).0, INVALID_MEDIA);
    assert_eq!(input.validate(&encode(1, 0, &[frame(1, 1, vec![0])]), &limits()).0, INVALID_MEDIA);
    assert_eq!(input.validate(&encode(1, 1, &[frame(0, 1, vec![])]), &limits()).0, INVALID_MEDIA);
    assert_eq!(input.validate(&encode(1, 1, &[frame(1, 0, vec![])]), &limits()).0, INVALID_MEDIA);
    assert_eq!(input.validate(&encode(1, 1, &[frame(1, 1, vec![2])]), &limits()).0, INVALID_MEDIA);
    let mut transparent = frame(1, 1, vec![0]);
    transparent.transparent = Some(2);
    assert_eq!(input.validate(&encode(1, 1, &[transparent]), &limits()).0, INVALID_MEDIA);
    for (left, top, width, height) in [(7, 5, 2, 3), (6, 6, 2, 3), (0, 0, 9, 1), (65535, 0, 1, 1)] {
        let mut local = frame(width, height, vec![0; width as usize * height as usize]);
        local.left = left;
        local.top = top;
        local.interlaced = true;
        assert_eq!(input.validate(&encode(8, 8, &[local]), &limits()).0, INVALID_MEDIA);
    }
}

#[test]
fn frame_input_metadata_and_overflow_budgets_fail_explicitly() {
    let input = Input::new();
    let data = animation();
    let mut budget = limits();
    budget.max_input_bytes = data.len() as u64 - 1;
    assert_eq!(input.validate(&data, &budget).0, LIMIT_EXCEEDED);
    budget = limits(); budget.max_canvas_pixels = 63;
    assert_eq!(input.validate(&data, &budget).0, LIMIT_EXCEEDED);
    budget = limits(); budget.max_total_pixels = 69;
    assert_eq!(input.validate(&data, &budget).0, LIMIT_EXCEEDED);
    budget = limits(); budget.max_frames = 1;
    assert_eq!(input.validate(&data, &budget).0, LIMIT_EXCEEDED);
    budget = limits(); budget.max_input_bytes = u64::MAX;
    assert_eq!(input.validate(&data, &budget).0, INVALID_ARGUMENT);
    budget = limits(); budget.max_canvas_pixels = u64::MAX;
    assert_eq!(input.validate(&data, &budget).0, INVALID_ARGUMENT);
    budget = limits(); budget.max_frames = 0;
    assert_eq!(input.validate(&data, &budget).0, INVALID_ARGUMENT);

    let mut encoder = Encoder::new(Vec::new(), 1, 1, PALETTE).unwrap();
    encoder.write_raw_extension(AnyExtension(0xfe), &[&[b'x'; 100]]).unwrap();
    encoder.write_frame(&frame(1, 1, vec![0])).unwrap();
    budget = limits(); budget.max_metadata_bytes = 32;
    assert_eq!(input.validate(&encoder.into_inner().unwrap(), &budget).0, LIMIT_EXCEEDED);
}

#[test]
fn null_inputs_are_rejected_at_the_same_panic_safe_abi_boundary() {
    let mut info = HuhuGifInfo::default();
    unsafe {
        assert_eq!(huhu_gif_validate_file(ptr::null(), 1, &limits(), ptr::null(), ptr::null(), &mut info), INVALID_ARGUMENT);
        assert_eq!(huhu_gif_validate_file(b"x".as_ptr(), 1, ptr::null(), ptr::null(), ptr::null(), &mut info), INVALID_ARGUMENT);
        assert_eq!(huhu_gif_validate_file(b"x".as_ptr(), 1, &limits(), ptr::null(), ptr::null(), ptr::null_mut()), INVALID_ARGUMENT);
        assert_eq!(huhu_cancellation_create(ptr::null_mut()), INVALID_ARGUMENT);
        assert_eq!(huhu_cancellation_request(ptr::null()), INVALID_ARGUMENT);
        huhu_cancellation_free(ptr::null_mut());
    }
}

#[test]
fn independent_cancellation_is_monotonic_and_does_not_require_a_receive() {
    struct Cancellation(*mut HuhuCancellation);
    impl Drop for Cancellation {
        fn drop(&mut self) { unsafe { huhu_cancellation_free(self.0); } }
    }
    let input = Input::new();
    std::fs::write(&input.0, animation()).unwrap();
    let path = input.0.to_str().unwrap();
    let mut raw = ptr::null_mut();
    assert_eq!(unsafe { huhu_cancellation_create(&mut raw) }, OK);
    let cancellation = Cancellation(raw);
    let mut info = HuhuGifInfo::default();
    assert_eq!(unsafe { huhu_gif_validate_file(path.as_ptr(), path.len(), &limits(),
        ptr::null(), cancellation.0, &mut info) }, OK);
    assert_eq!(info.frames, 2);

    // The handle is immutable apart from its atomic flag. Requesters use
    // shared references and all join before its same-side destructor runs.
    let shared = unsafe { &*cancellation.0 };
    std::thread::scope(|scope| {
        for _ in 0..4 {
            scope.spawn(move || assert_eq!(unsafe { huhu_cancellation_request(shared) }, OK));
        }
    });
    for _ in 0..2 {
        assert_eq!(unsafe { huhu_gif_validate_file(path.as_ptr(), path.len(), &limits(),
            ptr::null(), cancellation.0, &mut info) }, CANCELLED);
        assert_eq!((info.width, info.height, info.frames, info.reserved, info.decoded_pixels, info.duration_ms),
            (0, 0, 0, 0, 0, 0));
    }
    // No receive pointer or fake session participates in local import. A
    // separate ordinary validation remains usable after this handle cancels.
    assert_eq!(input.validate(&animation(), &limits()).0, OK);
}
