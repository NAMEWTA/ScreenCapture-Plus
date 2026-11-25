use crate::status::{self, Error, Result};
use localsend::model::transfer::FileDto;
use std::fs::{File, OpenOptions};
use std::io::{Seek, SeekFrom};
use std::path::Path;
use std::sync::{Arc, Mutex, atomic::{AtomicBool, Ordering}};

pub const MAX_FILE_BYTES: u64 = 32 * 1024 * 1024 * 1024;

pub struct HuhuMediaLease(pub Arc<Lease>);

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct HuhuMediaLeaseInfo {
    pub volume: u64,
    pub file: u64,
    pub bytes: u64,
    pub media_kind: u32,
    pub reserved: u32,
}

pub struct Lease {
    file: Mutex<File>,
    busy: AtomicBool,
    pub name: String,
    pub size: u64,
    pub kind: u32,
}

pub struct LeaseUse {
    lease: Arc<Lease>,
    pub file: Option<File>,
    pub dto: FileDto,
}

pub fn mime(kind: u32) -> Result<&'static str> {
    match kind {
        1 => Ok("image/png"), 2 => Ok("image/gif"), 3 => Ok("video/mp4"),
        _ => Err(Error::new(status::INVALID_MEDIA, "Unsupported media kind")),
    }
}

pub fn kind_from_mime(value: &str) -> Result<u32> {
    match value {
        "image/png" => Ok(1), "image/gif" => Ok(2), "video/mp4" => Ok(3),
        _ => Err(Error::new(status::INVALID_MEDIA, "Unsupported media type")),
    }
}

pub(crate) fn open_regular_file(path: &Path) -> Result<(File, std::fs::Metadata)> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)] {
        use std::os::unix::fs::OpenOptionsExt;
        // NONBLOCK prevents a substituted FIFO from hanging before fstat.
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC);
    }
    #[cfg(windows)] {
        use std::os::windows::fs::OpenOptionsExt;
        // Inspect the object, never follow a junction/symlink. Allow path
        // movement but deny a concurrent writer while this handle is alive.
        options.custom_flags(0x0020_0000).share_mode(0x1 | 0x4);
    }
    let file = options.open(path)?;
    let metadata = file.metadata()?;
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 {
            return Err(Error::new(status::INVALID_ARGUMENT, "Reparse points cannot be media files"));
        }
    }
    if !metadata.is_file() {
        return Err(Error::new(status::INVALID_ARGUMENT, "Media requires an ordinary file"));
    }
    Ok((file, metadata))
}

impl Lease {
    pub fn open(path: &str) -> Result<Arc<Self>> {
        if !status::text_is_bounded(path, 32768) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid media path"));
        }
        let path = Path::new(path);
        let name = path.file_name().and_then(|name| name.to_str())
            .ok_or_else(|| Error::new(status::INVALID_ARGUMENT, "Media name is not UTF-8"))?;
        if !status::text_is_bounded(name, 1024) || name.contains(['/', '\\', ':']) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Unsafe media name"));
        }
        let kind = match path.extension().and_then(|extension| extension.to_str())
            .unwrap_or("").to_ascii_lowercase().as_str() {
            "png" => 1, "gif" => 2, "mp4" => 3,
            _ => return Err(Error::new(status::INVALID_MEDIA, "Only PNG, GIF and MP4 may be leased")),
        };
        let (file, metadata) = open_regular_file(path)?;
        if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_FILE_BYTES {
            return Err(Error::new(status::INVALID_ARGUMENT, "Lease requires an ordinary bounded nonempty media file"));
        }
        Ok(Arc::new(Self { file: Mutex::new(file), busy: AtomicBool::new(false),
            name: name.to_owned(), size: metadata.len(), kind }))
    }

    pub fn info(&self) -> Result<HuhuMediaLeaseInfo> {
        let file = self.file.lock().map_err(|_| Error::new(status::PANIC, "Media lease lock was poisoned"))?;
        #[cfg(unix)] {
            use std::os::unix::fs::MetadataExt;
            let metadata = file.metadata()?;
            Ok(HuhuMediaLeaseInfo { volume: metadata.dev(), file: metadata.ino(),
                bytes: metadata.len(), media_kind: self.kind, reserved: 0 })
        }
        #[cfg(windows)] {
            use std::os::windows::io::AsRawHandle;
            use windows_sys::Win32::Storage::FileSystem::{GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION};
            // Rust 1.97.1's MetadataExt file_index/volume_serial_number remain
            // unstable. Reuse Microsoft's generated binding for this exact
            // already-open HANDLE; never look the pathname up a second time.
            let mut information = std::mem::MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::uninit();
            if unsafe { GetFileInformationByHandle(file.as_raw_handle() as isize, information.as_mut_ptr()) } == 0 {
                return Err(std::io::Error::last_os_error().into());
            }
            let information = unsafe { information.assume_init() };
            Ok(HuhuMediaLeaseInfo { volume: u64::from(information.dwVolumeSerialNumber),
                file: (u64::from(information.nFileIndexHigh) << 32) | u64::from(information.nFileIndexLow),
                bytes: (u64::from(information.nFileSizeHigh) << 32) | u64::from(information.nFileSizeLow),
                media_kind: self.kind, reserved: 0 })
        }
        #[cfg(not(any(unix, windows)))] {
            let _ = file;
            Err(Error::new(status::INVALID_STATE, "Media identity is unsupported on this platform"))
        }
    }

    pub fn acquire(self: &Arc<Self>, id: String, limit: u64) -> Result<LeaseUse> {
        if self.size > limit {
            return Err(Error::new(status::INVALID_ARGUMENT, "Media exceeds configured file budget"));
        }
        if self.busy.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire).is_err() {
            return Err(Error::new(status::BUSY, "Media lease already participates in a send"));
        }
        let mut use_lease = LeaseUse { lease: self.clone(), file: None,
            dto: FileDto { id, file_name: self.name.clone(), size: self.size,
                file_type: mime(self.kind)?.to_owned(), sha256: None, preview: None, metadata: None } };
        let mut original = self.file.lock().map_err(|_| Error::new(status::PANIC, "Media lease lock was poisoned"))?;
        let metadata = original.metadata()?;
        if !metadata.is_file() || metadata.len() != self.size {
            return Err(Error::new(status::LENGTH_MISMATCH, "Leased media changed size"));
        }
        original.seek(SeekFrom::Start(0))?;
        use_lease.file = Some(original.try_clone()?);
        use_lease.dto.metadata = localsend::model::transfer::FileMetadata::from_fs_metadata(&metadata);
        Ok(use_lease)
    }
}

impl Drop for LeaseUse {
    fn drop(&mut self) { self.lease.busy.store(false, Ordering::Release); }
}
