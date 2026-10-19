use std::{
    mem::{align_of, offset_of, size_of},
    slice, str,
};

#[repr(C)]
pub struct ProbeRecord {
    pub tag: u32,
    pub entity: u64,
    pub bytes: *const u8,
    pub length: usize,
}

#[repr(C)]
pub struct ProbeLayout {
    pub pointer_size: u32,
    pub record_size: u32,
    pub record_align: u32,
    pub entity_offset: u32,
    pub bytes_offset: u32,
    pub length_offset: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn ae_probe_add(left: u32, right: u32) -> u32 {
    left.wrapping_add(right)
}

// Input slices are borrowed for this call only. Null is permitted only for empty input.
unsafe fn input_bytes<'a>(bytes: *const u8, length: usize) -> Option<&'a [u8]> {
    if length == 0 {
        Some(&[])
    } else if bytes.is_null() {
        None
    } else {
        Some(unsafe { slice::from_raw_parts(bytes, length) })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ae_probe_checksum(bytes: *const u8, length: usize) -> u32 {
    let Some(bytes) = (unsafe { input_bytes(bytes, length) }) else {
        return 0;
    };
    bytes.iter().fold(2_166_136_261u32, |hash, byte| {
        (hash ^ u32::from(*byte)).wrapping_mul(16_777_619)
    })
}

// No allocation crosses the ABI. `required` includes the terminating zero.
// Status: 0 copied, 1 insufficient capacity, 2 invalid UTF-8, 3 invalid null argument.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ae_probe_format(
    bytes: *const u8,
    length: usize,
    output: *mut u8,
    capacity: usize,
    required: *mut usize,
) -> i32 {
    if required.is_null() {
        return 3;
    }
    unsafe {
        *required = 0;
    }
    let Some(bytes) = (unsafe { input_bytes(bytes, length) }) else {
        return 3;
    };
    let Ok(text) = str::from_utf8(bytes) else {
        return 2;
    };
    let result = format!("Rust owns [{text}] ({} bytes)", bytes.len());
    let needed = result.len() + 1;
    unsafe {
        *required = needed;
    }
    if capacity < needed {
        return 1;
    }
    if output.is_null() {
        return 3;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(result.as_ptr(), output, result.len());
        *output.add(result.len()) = 0;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ae_probe_callback(
    callback: Option<unsafe extern "C" fn(*mut std::ffi::c_void, u32) -> u32>,
    context: *mut std::ffi::c_void,
    value: u32,
) -> u32 {
    match callback {
        Some(callback) => unsafe { callback(context, value) },
        None => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ae_probe_layout(output: *mut ProbeLayout) {
    if let Some(output) = unsafe { output.as_mut() } {
        *output = ProbeLayout {
            pointer_size: size_of::<usize>() as u32,
            record_size: size_of::<ProbeRecord>() as u32,
            record_align: align_of::<ProbeRecord>() as u32,
            entity_offset: offset_of!(ProbeRecord, entity) as u32,
            bytes_offset: offset_of!(ProbeRecord, bytes) as u32,
            length_offset: offset_of!(ProbeRecord, length) as u32,
        };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ae_probe_record(record: *const ProbeRecord) -> u32 {
    let Some(record) = (unsafe { record.as_ref() }) else {
        return 0;
    };
    if record.tag != 0x12345678 || record.entity != 0x1122334455667788 {
        return 0;
    }
    unsafe { ae_probe_checksum(record.bytes, record.length) }
}

const _: () = {
    assert!(size_of::<ProbeLayout>() == 24);
    assert!(offset_of!(ProbeRecord, entity) == 8);
    assert!(offset_of!(ProbeRecord, bytes) == 16);
    assert!(align_of::<ProbeRecord>() == 8);
    assert!(size_of::<ProbeRecord>() == if size_of::<usize>() == 4 { 24 } else { 32 });
};
