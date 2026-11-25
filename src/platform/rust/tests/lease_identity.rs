//! The identity boundary uses real OS handles and namespace changes. A lease
//! must keep describing its open object after its original path is replaced.
use huhucapture_native::{huhu_media_lease_free, huhu_media_lease_info, huhu_media_lease_open,
    HuhuMediaLease, HuhuMediaLeaseInfo};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

const PNG: &[u8] = include_bytes!("../../../../third_party/localsend/core/tests/fixtures/one-pixel.png");
const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = -1;

struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("huhu-lease-身份-{}-{}-{stamp}",
            std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        for name in ["selected.png", "moved.png", "alias.png"] {
            let _ = std::fs::remove_file(self.0.join(name));
        }
        let _ = std::fs::remove_dir(&self.0);
    }
}

struct Lease(*mut HuhuMediaLease);
impl Lease {
    fn open(path: &Path) -> Self {
        let path = path.to_str().unwrap();
        let mut raw = ptr::null_mut();
        assert_eq!(unsafe { huhu_media_lease_open(path.as_ptr(), path.len(), &mut raw) }, OK);
        Self(raw)
    }
    fn info(&self) -> HuhuMediaLeaseInfo {
        let mut info = HuhuMediaLeaseInfo::default();
        assert_eq!(unsafe { huhu_media_lease_info(self.0, &mut info) }, OK);
        info
    }
}
impl Drop for Lease { fn drop(&mut self) { unsafe { huhu_media_lease_free(self.0); } } }

#[test]
fn open_object_survives_rename_and_path_replacement_and_matches_its_hardlink() {
    let directory = Directory::new();
    let selected = directory.0.join("selected.png");
    let moved = directory.0.join("moved.png");
    let alias = directory.0.join("alias.png");
    std::fs::write(&selected, PNG).unwrap();
    let original = Lease::open(&selected);
    let first = original.info();
    assert_eq!((first.bytes, first.media_kind, first.reserved), (PNG.len() as u64, 1, 0));
    #[cfg(unix)] {
        use std::os::unix::fs::MetadataExt;
        let metadata = std::fs::metadata(&selected).unwrap();
        assert_eq!((first.volume, first.file), (metadata.dev(), metadata.ino()));
    }
    std::fs::rename(&selected, &moved).unwrap();
    std::fs::write(&selected, PNG).unwrap(); // Same bytes, different live object.
    let replacement = Lease::open(&selected);
    assert_ne!((replacement.info().volume, replacement.info().file), (first.volume, first.file));
    assert_eq!(original.info(), first, "lease info must not resolve the replaced pathname");
    std::fs::hard_link(&moved, &alias).unwrap();
    let linked = Lease::open(&alias);
    assert_eq!(linked.info(), first);
}

#[test]
fn bad_handle_info_is_rejected_and_never_leaves_an_apparent_identity() {
    let mut info = HuhuMediaLeaseInfo { volume: u64::MAX, file: u64::MAX, bytes: u64::MAX,
        media_kind: u32::MAX, reserved: u32::MAX };
    assert_eq!(unsafe { huhu_media_lease_info(ptr::null(), &mut info) }, INVALID_ARGUMENT);
    assert_eq!(info, HuhuMediaLeaseInfo::default());
    assert_eq!(unsafe { huhu_media_lease_info(ptr::null(), ptr::null_mut()) }, INVALID_ARGUMENT);
}
