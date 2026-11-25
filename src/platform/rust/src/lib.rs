//! One Rust static library for the existing gifski encoder and the adapted
//! official LocalSend core. All project C entries catch panics. No callback
//! into Qt, Rust allocation, exception, or borrowed input crosses ownership.
#![deny(unsafe_op_in_unsafe_fn)]

mod cancellation;
mod config;
mod engine;
mod event;
mod gif_validation;
mod lease;
mod outbound;
mod receive;
mod status;

pub use cancellation::HuhuCancellation;
pub use engine::HuhuTransfer;
pub use event::{HuhuBuffer, HuhuEvent};
pub use lease::{HuhuMediaLease, HuhuMediaLeaseInfo};
pub use receive::{HuhuReceive, HuhuVerification};
pub use gif_validation::{HuhuGifInfo, HuhuGifLimits};
pub use gifski::c_api;

use status::{Error, Result};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

// Force the exact upstream C API's crate into the aggregate archive. The Qt
// validation executable calls its original symbols directly, alongside LAN.
#[used]
static GIFSKI_LINK: unsafe extern "C" fn(*const c_api::GifskiSettings) -> *const c_api::GifskiHandle
    = c_api::gifski_new;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HuhuSlice { pub data: *const u8, pub length: usize }

#[repr(C)]
pub struct HuhuEventView {
    pub kind: u32,
    pub status: i32,
    pub operation: u64,
    pub json: HuhuSlice,
}

fn boundary(action: impl FnOnce() -> Result<()>) -> i32 {
    match catch_unwind(AssertUnwindSafe(action)) {
        Ok(Ok(())) => status::OK,
        Ok(Err(error)) => error.status,
        Err(_) => status::PANIC,
    }
}

unsafe fn reference<'a, T>(pointer: *const T) -> Result<&'a T> {
    unsafe { pointer.as_ref() }.ok_or_else(|| Error::new(status::INVALID_ARGUMENT, "Null handle"))
}

unsafe fn mutable<'a, T>(pointer: *mut T) -> Result<&'a mut T> {
    unsafe { pointer.as_mut() }.ok_or_else(|| Error::new(status::INVALID_ARGUMENT, "Null output or handle"))
}

unsafe fn input<'a>(pointer: *const u8, length: usize, maximum: usize) -> Result<&'a [u8]> {
    if length > maximum || (length != 0 && pointer.is_null()) {
        return Err(Error::new(status::INVALID_ARGUMENT, "Invalid or over-budget input buffer"));
    }
    if length == 0 { Ok(&[]) } else { Ok(unsafe { slice::from_raw_parts(pointer, length) }) }
}

unsafe fn text<'a>(pointer: *const u8, length: usize, maximum: usize) -> Result<&'a str> {
    let bytes = unsafe { input(pointer, length, maximum) }?;
    let value = std::str::from_utf8(bytes).map_err(|_| Error::new(status::INVALID_ARGUMENT, "Input is not UTF-8"))?;
    if value.contains('\0') { return Err(Error::new(status::INVALID_ARGUMENT, "Input contains NUL")); }
    Ok(value)
}

unsafe fn output<'a, T>(pointer: *mut *mut T) -> Result<&'a mut *mut T> {
    let output = unsafe { mutable(pointer) }?;
    *output = ptr::null_mut();
    Ok(output)
}

#[no_mangle]
pub extern "C" fn huhu_native_abi_version() -> u32 { 1 }

#[no_mangle]
pub unsafe extern "C" fn huhu_cancellation_create(result: *mut *mut HuhuCancellation) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        *result = Box::into_raw(Box::new(HuhuCancellation::default()));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_cancellation_request(cancellation: *const HuhuCancellation) -> i32 {
    boundary(|| {
        unsafe { reference(cancellation) }?.request();
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_cancellation_free(cancellation: *mut HuhuCancellation) {
    if !cancellation.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(cancellation) })));
    }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_gif_validate_file(path: *const u8, length: usize,
    limits: *const HuhuGifLimits, receive: *const HuhuReceive,
    cancellation: *const HuhuCancellation, result: *mut HuhuGifInfo) -> i32 {
    boundary(|| {
        let result = unsafe { mutable(result) }?;
        *result = HuhuGifInfo::default();
        let path = unsafe { text(path, length, 32768) }?;
        let limits = unsafe { reference(limits) }?;
        let receive = if receive.is_null() { None } else { Some(unsafe { reference(receive) }?) };
        let cancellation = if cancellation.is_null() { None } else { Some(unsafe { reference(cancellation) }?) };
        *result = gif_validation::validate_file(path, limits, receive, cancellation)?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_identity_generate(result: *mut *mut HuhuBuffer) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let identity = localsend::crypto::cert::generate_self_signed()
            .map_err(|_| Error::new(status::IO, "Could not generate LocalSend TLS identity"))?;
        let json = serde_json::json!({"certPem": identity.certificate_pem,
            "privateKeyPem": identity.private_key_pem, "fingerprint": identity.fingerprint});
        *result = Box::into_raw(Box::new(HuhuBuffer(json.to_string().into_bytes())));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_buffer_view(buffer: *const HuhuBuffer, result: *mut HuhuSlice) -> i32 {
    boundary(|| {
        let result = unsafe { mutable(result) }?;
        let buffer = unsafe { reference(buffer) }?;
        *result = HuhuSlice { data: buffer.0.as_ptr(), length: buffer.0.len() };
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_buffer_free(buffer: *mut HuhuBuffer) {
    if !buffer.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(buffer) }))); }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_create(config: *const u8, length: usize, result: *mut *mut HuhuTransfer) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let config = serde_json::from_slice(unsafe { input(config, length, 1024 * 1024) }?)?;
        *result = Box::into_raw(Box::new(HuhuTransfer::create(config)?));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_info(transfer: *const HuhuTransfer, result: *mut *mut HuhuBuffer) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let transfer = unsafe { reference(transfer) }?;
        *result = Box::into_raw(Box::new(HuhuBuffer(transfer.info().to_string().into_bytes())));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_poll(transfer: *mut HuhuTransfer, timeout_ms: u32, result: *mut *mut HuhuEvent) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let transfer = unsafe { reference(transfer) }?;
        *result = Box::into_raw(Box::new(transfer.poll(timeout_ms)?));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_event_view(event: *const HuhuEvent, result: *mut HuhuEventView) -> i32 {
    boundary(|| {
        let result = unsafe { mutable(result) }?;
        let event = unsafe { reference(event) }?;
        *result = HuhuEventView { kind: event.kind, status: event.status, operation: event.operation,
            json: HuhuSlice { data: event.json.as_ptr(), length: event.json.len() } };
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_event_take_receive(event: *mut HuhuEvent, result: *mut *mut HuhuReceive) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let event = unsafe { mutable(event) }?;
        let receive = event.receive.take().ok_or_else(|| Error::new(status::INVALID_STATE, "Event carries no untaken receive"))?;
        *result = Box::into_raw(receive);
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_event_free(event: *mut HuhuEvent) {
    if !event.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(event) }))); }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_accept(transfer: *mut HuhuTransfer, session: *const u8,
    session_length: usize, accepted_ids: *const u8, ids_length: usize) -> i32 {
    boundary(|| {
        let transfer = unsafe { reference(transfer) }?;
        let session = unsafe { text(session, session_length, 128) }?;
        let ids = serde_json::from_slice(unsafe { input(accepted_ids, ids_length, 16 * 1024) }?)?;
        transfer.shared.accept(session, ids)
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_cancel_receive(transfer: *mut HuhuTransfer, session: *const u8, length: usize) -> i32 {
    boundary(|| {
        let transfer = unsafe { reference(transfer) }?;
        transfer.shared.cancel_receive(unsafe { text(session, length, 128) }?)
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_media_lease_open(path: *const u8, length: usize, result: *mut *mut HuhuMediaLease) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let lease = lease::Lease::open(unsafe { text(path, length, 32768) }?)?;
        *result = Box::into_raw(Box::new(HuhuMediaLease(lease)));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_media_lease_info(lease: *const HuhuMediaLease, result: *mut HuhuMediaLeaseInfo) -> i32 {
    boundary(|| {
        let result = unsafe { mutable(result) }?;
        *result = HuhuMediaLeaseInfo::default();
        *result = unsafe { reference(lease) }?.0.info()?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_media_lease_free(lease: *mut HuhuMediaLease) {
    if !lease.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(lease) }))); }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_send(transfer: *mut HuhuTransfer, peer: *const u8, peer_length: usize,
    leases: *const *const HuhuMediaLease, lease_count: usize, operation: *mut u64) -> i32 {
    boundary(|| {
        let operation = unsafe { mutable(operation) }?;
        *operation = 0;
        let transfer = unsafe { reference(transfer) }?;
        if leases.is_null() || lease_count == 0 || lease_count > 64 {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid media lease array"));
        }
        let leases = unsafe { slice::from_raw_parts(leases, lease_count) }.iter()
            .map(|lease| unsafe { reference(*lease) }.map(|lease| lease.0.clone())).collect::<Result<Vec<_>>>()?;
        let peer = serde_json::from_slice(unsafe { input(peer, peer_length, 4096) }?)?;
        *operation = outbound::send(&transfer.shared, peer, leases)?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_discover(transfer: *mut HuhuTransfer, address: *const u8,
    address_length: usize, port: u16, operation: *mut u64) -> i32 {
    boundary(|| {
        let operation = unsafe { mutable(operation) }?;
        *operation = 0;
        let transfer = unsafe { reference(transfer) }?;
        *operation = outbound::discover(&transfer.shared, unsafe { text(address, address_length, 15) }?, port)?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_devices(transfer: *const HuhuTransfer, result: *mut *mut HuhuBuffer) -> i32 {
    boundary(|| {
        let result = unsafe { output(result) }?;
        let transfer = unsafe { reference(transfer) }?;
        let devices = transfer.shared.discovery.as_ref().map(|discovery|
            discovery.devices().iter().map(engine::device_json).collect::<Vec<_>>()).unwrap_or_default();
        *result = Box::into_raw(Box::new(HuhuBuffer(serde_json::to_vec(&devices)?)));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_cancel(transfer: *mut HuhuTransfer, operation: u64) -> i32 {
    boundary(|| {
        let transfer = unsafe { reference(transfer) }?;
        let operations = transfer.shared.operations.lock().map_err(|_| Error::new(status::PANIC, "Operation lock was poisoned"))?;
        let active = operations.get(&operation).ok_or_else(|| Error::new(status::CLOSED, "Operation already ended"))?;
        active.cancel.cancel();
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_receive_read(receive: *mut HuhuReceive, destination: *mut u8,
    capacity: usize, written: *mut usize, timeout_ms: u32) -> i32 {
    boundary(|| {
        let written = unsafe { mutable(written) }?;
        *written = 0;
        let receive = unsafe { reference(receive) }?;
        if destination.is_null() || capacity == 0 || capacity > 512 * 1024 {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid receive buffer"));
        }
        *written = receive.read(unsafe { slice::from_raw_parts_mut(destination, capacity) }, timeout_ms)?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_receive_verify(receive: *mut HuhuReceive, timeout_ms: u32,
    result: *mut HuhuVerification) -> i32 {
    boundary(|| {
        let result = unsafe { mutable(result) }?;
        *result = HuhuVerification::default();
        *result = unsafe { reference(receive) }?.verify(timeout_ms)?;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn huhu_receive_is_cancelled(receive: *const HuhuReceive) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| unsafe { reference(receive) }?.is_cancelled())) {
        Ok(Ok(cancelled)) => i32::from(cancelled),
        Ok(Err(error)) => error.status,
        Err(_) => status::PANIC,
    }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_receive_complete(receive: *mut HuhuReceive, status: i32, media_kind: u32) -> i32 {
    boundary(|| unsafe { reference(receive) }?.complete(status, media_kind))
}

#[no_mangle]
pub unsafe extern "C" fn huhu_receive_free(receive: *mut HuhuReceive) {
    if !receive.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(receive) }))); }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_stop(transfer: *mut HuhuTransfer) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| unsafe { mutable(transfer) }.map(HuhuTransfer::stop))) {
        Ok(Ok(status)) => status,
        Ok(Err(error)) => error.status,
        Err(_) => status::PANIC,
    }
}

#[no_mangle]
pub unsafe extern "C" fn huhu_transfer_free(transfer: *mut HuhuTransfer) {
    if !transfer.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(transfer) }))); }
}
