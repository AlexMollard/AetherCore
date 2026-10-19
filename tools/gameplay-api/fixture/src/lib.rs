#[path = "../../generated/gameplay_abi.rs"]
mod gameplay_abi;
use gameplay_abi::*;

use core::{mem::size_of, ptr};

// Deliberately single-instance test module. Calls are synchronous on the fixture's owner thread.
// This is a foreign ABI consumer, not the production Rust gameplay host.
const TEXT: &[u8] = "café 猫".as_bytes();
static mut ENGINE: Option<AetherGameplayEngineApi> = None;
static mut STATE: u32 = 0;
static mut ENTITY: AetherGameplayEntity = AetherGameplayEntity { world: 0, id: 0, reserved: 0 };
static mut PROPERTY: [u8; 128] = [0; 128];
static mut PROPERTY_LENGTH: u32 = 0;

fn view(bytes: &[u8]) -> AetherGameplayUtf8 {
    AetherGameplayUtf8 { data: bytes.as_ptr().cast(), length: bytes.len() as u32 }
}
unsafe fn copy_string(bytes: *const u8, length: u32, buffer: AetherGameplayUtf8Buffer) -> i32 {
    unsafe {
        if buffer.required.is_null() || (buffer.data.is_null() && buffer.capacity != 0) {
            return AE_GAMEPLAY_INVALID_ARGUMENT;
        }
        *buffer.required = length + 1;
        if buffer.capacity < length + 1 { return AE_GAMEPLAY_BUFFER_TOO_SMALL; }
        ptr::copy(bytes, buffer.data.cast(), length as usize);
        *buffer.data.add(length as usize) = 0;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn initialize(_: u64, engine: *const AetherGameplayEngineApi) -> i32 {
    unsafe {
        if engine.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        let api = *engine;
        if api.abiMajor != AE_GAMEPLAY_ABI_MAJOR || api.structSize != size_of::<AetherGameplayEngineApi>() as u32 {
            return AE_GAMEPLAY_ABI_MISMATCH;
        }
        if api.context == 0 || api.entityValid.is_none() || api.getPosition.is_none() || api.setPosition.is_none() || api.reportDiagnostic.is_none() {
            return AE_GAMEPLAY_INVALID_ARGUMENT;
        }
        ENGINE = Some(api);
        STATE = 0;
        AE_GAMEPLAY_OK
    }
}
unsafe extern "C" fn shutdown(_: u64) -> i32 {
    unsafe { ENGINE = None; STATE = 0; }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn type_count(_: u64, out: *mut u32) -> i32 {
    if out.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
    unsafe { *out = 1; }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn type_info(_: u64, index: u32, out: *mut AetherGameplayTypeInfo) -> i32 {
    if index != 0 || out.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
    unsafe { *out = AetherGameplayTypeInfo {structSize: size_of::<AetherGameplayTypeInfo>() as u32, propertyCount: 1, name: view(TEXT)}; }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn property_info(_: u64, index: u32, property: u32, out: *mut AetherGameplayPropertyInfo) -> i32 {
    if index != 0 || property != 0 || out.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
    unsafe { *out = AetherGameplayPropertyInfo {
        structSize: size_of::<AetherGameplayPropertyInfo>() as u32, r#type: AE_GAMEPLAY_PROPERTY_STRING,
        name: view("名".as_bytes()), enumNames: view(b""), componentType: view(b"")
    }; }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn create(_: u64, index: u32, entity: AetherGameplayEntity, out: *mut u64) -> i32 {
    unsafe {
        if index != 0 || out.is_null() || STATE != 0 || { let engine = ENGINE; engine.is_none() } { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        ENTITY = entity;
        STATE = 1;
        ptr::copy_nonoverlapping(TEXT.as_ptr(), ptr::addr_of_mut!(PROPERTY).cast(), TEXT.len());
        PROPERTY_LENGTH = TEXT.len() as u32;
        *out = 17;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn attach(_: u64, instance: u64) -> i32 {
    unsafe {
        if instance != 17 || STATE != 1 { return AE_GAMEPLAY_INVALID_HANDLE; }
        if ENTITY.id == 99 { return AE_GAMEPLAY_SCRIPT_ERROR; }
        STATE = 2;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn update(_: u64, instance: u64, delta: f32) -> i32 {
    unsafe {
        if instance != 17 || STATE != 2 { return AE_GAMEPLAY_INVALID_HANDLE; }
        if delta < 0.0 { return AE_GAMEPLAY_SCRIPT_ERROR; }
        let Some(api) = ENGINE else { return AE_GAMEPLAY_INVALID_HANDLE; };
        let mut valid = 0;
        let status = (api.entityValid.unwrap())(api.context, ENTITY, &mut valid);
        if status != AE_GAMEPLAY_OK { return status; }
        if valid == 0 { return AE_GAMEPLAY_INVALID_HANDLE; }
        let mut position = AetherGameplayVec3 {x: 0.0, y: 0.0, z: 0.0};
        let status = (api.getPosition.unwrap())(api.context, ENTITY, &mut position);
        if status != AE_GAMEPLAY_OK { return status; }
        position.x += delta;
        let status = (api.setPosition.unwrap())(api.context, ENTITY, position);
        if status != AE_GAMEPLAY_OK { return status; }
        let diagnostic = AetherGameplayDiagnostic {
            structSize: size_of::<AetherGameplayDiagnostic>() as u32, severity: AE_GAMEPLAY_DIAGNOSTIC_INFO,
            code: view(b"fixture.update"), message: view(TEXT), file: view(b"fixture.rs"), line: 1, column: 1
        };
        (api.reportDiagnostic.unwrap())(api.context, &diagnostic)
    }
}
unsafe extern "C" fn detach(_: u64, instance: u64) -> i32 {
    unsafe {
        if instance != 17 || STATE != 2 { return AE_GAMEPLAY_INVALID_HANDLE; }
        STATE = 3;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn destroy(_: u64, instance: u64) -> i32 {
    unsafe {
        if instance != 17 || (STATE != 1 && STATE != 3) { return AE_GAMEPLAY_INVALID_HANDLE; }
        STATE = 0;
    }
    AE_GAMEPLAY_OK
}
unsafe fn string_value(out: *mut AetherGameplayPropertyValue) -> i32 {
    if out.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
    unsafe {
        *out = core::mem::zeroed();
        (*out).structSize = size_of::<AetherGameplayPropertyValue>() as u32;
        (*out).r#type = AE_GAMEPLAY_PROPERTY_STRING;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn get_property(_: u64, instance: u64, property: u32, out: *mut AetherGameplayPropertyValue, buffer: AetherGameplayUtf8Buffer) -> i32 {
    unsafe {
        if instance != 17 || STATE == 0 { return AE_GAMEPLAY_INVALID_HANDLE; }
        if property != 0 || string_value(out) != AE_GAMEPLAY_OK { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        copy_string(ptr::addr_of!(PROPERTY).cast(), PROPERTY_LENGTH, buffer)
    }
}
unsafe extern "C" fn set_property(_: u64, instance: u64, property: u32, value: *const AetherGameplayPropertyValue, string: AetherGameplayUtf8) -> i32 {
    unsafe {
        if instance != 17 || STATE == 0 { return AE_GAMEPLAY_INVALID_HANDLE; }
        if property != 0 || value.is_null() || (*value).r#type != AE_GAMEPLAY_PROPERTY_STRING || string.length > 128 || (string.length != 0 && string.data.is_null()) {
            return AE_GAMEPLAY_INVALID_ARGUMENT;
        }
        if string.length != 0 { ptr::copy_nonoverlapping(string.data.cast::<u8>(), ptr::addr_of_mut!(PROPERTY).cast::<u8>(), string.length as usize); }
        PROPERTY_LENGTH = string.length;
    }
    AE_GAMEPLAY_OK
}
unsafe extern "C" fn default_property(_: u64, index: u32, property: u32, out: *mut AetherGameplayPropertyValue, buffer: AetherGameplayUtf8Buffer) -> i32 {
    unsafe {
        if index != 0 || property != 0 || string_value(out) != AE_GAMEPLAY_OK { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        copy_string(TEXT.as_ptr(), TEXT.len() as u32, buffer)
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_gameplay_query(major: u32, out_size: u32, out: *mut AetherGameplayModuleApi) -> i32 {
    unsafe {
        if out.is_null() { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        ptr::write_bytes(out.cast::<u8>(), 0, (out_size as usize).min(size_of::<AetherGameplayModuleApi>()));
        if major != AE_GAMEPLAY_ABI_MAJOR { return AE_GAMEPLAY_ABI_MISMATCH; }
        if out_size < size_of::<AetherGameplayModuleApi>() as u32 { return AE_GAMEPLAY_BUFFER_TOO_SMALL; }
        if out_size != size_of::<AetherGameplayModuleApi>() as u32 { return AE_GAMEPLAY_INVALID_ARGUMENT; }
        *out = AetherGameplayModuleApi {
            structSize: size_of::<AetherGameplayModuleApi>() as u32, abiMajor: AE_GAMEPLAY_ABI_MAJOR, capabilities: 0, context: 5,
            initialize: Some(initialize), shutdown: Some(shutdown), getTypeCount: Some(type_count), getTypeInfo: Some(type_info),
            getPropertyInfo: Some(property_info), create: Some(create), destroy: Some(destroy), attach: Some(attach), update: Some(update), detach: Some(detach),
            getProperty: Some(get_property), setProperty: Some(set_property), getDefaultProperty: Some(default_property), dispatchEvent: None
        };
    }
    AE_GAMEPLAY_OK
}
#[unsafe(no_mangle)]
pub extern "C" fn fixture_layout(index: u32) -> usize {
    let values = [size_of::<AetherGameplayUtf8>(),
        core::mem::align_of::<AetherGameplayUtf8>(),
        core::mem::offset_of!(AetherGameplayUtf8, data),
        core::mem::offset_of!(AetherGameplayUtf8, length),
        size_of::<AetherGameplayUtf8Buffer>(),
        core::mem::align_of::<AetherGameplayUtf8Buffer>(),
        core::mem::offset_of!(AetherGameplayUtf8Buffer, data),
        core::mem::offset_of!(AetherGameplayUtf8Buffer, capacity),
        core::mem::offset_of!(AetherGameplayUtf8Buffer, required),
        size_of::<AetherGameplayEntity>(),
        core::mem::align_of::<AetherGameplayEntity>(),
        core::mem::offset_of!(AetherGameplayEntity, world),
        core::mem::offset_of!(AetherGameplayEntity, id),
        core::mem::offset_of!(AetherGameplayEntity, reserved),
        size_of::<AetherGameplayVec3>(),
        core::mem::align_of::<AetherGameplayVec3>(),
        core::mem::offset_of!(AetherGameplayVec3, x),
        core::mem::offset_of!(AetherGameplayVec3, y),
        core::mem::offset_of!(AetherGameplayVec3, z),
        size_of::<AetherGameplayPropertyValue>(),
        core::mem::align_of::<AetherGameplayPropertyValue>(),
        core::mem::offset_of!(AetherGameplayPropertyValue, structSize),
        core::mem::offset_of!(AetherGameplayPropertyValue, r#type),
        core::mem::offset_of!(AetherGameplayPropertyValue, vector),
        core::mem::offset_of!(AetherGameplayPropertyValue, integer),
        core::mem::offset_of!(AetherGameplayPropertyValue, entity),
        size_of::<AetherGameplayTypeInfo>(),
        core::mem::align_of::<AetherGameplayTypeInfo>(),
        core::mem::offset_of!(AetherGameplayTypeInfo, structSize),
        core::mem::offset_of!(AetherGameplayTypeInfo, propertyCount),
        core::mem::offset_of!(AetherGameplayTypeInfo, name),
        size_of::<AetherGameplayPropertyInfo>(),
        core::mem::align_of::<AetherGameplayPropertyInfo>(),
        core::mem::offset_of!(AetherGameplayPropertyInfo, structSize),
        core::mem::offset_of!(AetherGameplayPropertyInfo, r#type),
        core::mem::offset_of!(AetherGameplayPropertyInfo, name),
        core::mem::offset_of!(AetherGameplayPropertyInfo, enumNames),
        core::mem::offset_of!(AetherGameplayPropertyInfo, componentType),
        size_of::<AetherGameplayEvent>(),
        core::mem::align_of::<AetherGameplayEvent>(),
        core::mem::offset_of!(AetherGameplayEvent, structSize),
        core::mem::offset_of!(AetherGameplayEvent, kind),
        core::mem::offset_of!(AetherGameplayEvent, other),
        core::mem::offset_of!(AetherGameplayEvent, code),
        core::mem::offset_of!(AetherGameplayEvent, flags),
        core::mem::offset_of!(AetherGameplayEvent, payload),
        size_of::<AetherGameplayDiagnostic>(),
        core::mem::align_of::<AetherGameplayDiagnostic>(),
        core::mem::offset_of!(AetherGameplayDiagnostic, structSize),
        core::mem::offset_of!(AetherGameplayDiagnostic, severity),
        core::mem::offset_of!(AetherGameplayDiagnostic, code),
        core::mem::offset_of!(AetherGameplayDiagnostic, message),
        core::mem::offset_of!(AetherGameplayDiagnostic, file),
        core::mem::offset_of!(AetherGameplayDiagnostic, line),
        core::mem::offset_of!(AetherGameplayDiagnostic, column),
        size_of::<AetherGameplayEngineApi>(),
        core::mem::align_of::<AetherGameplayEngineApi>(),
        core::mem::offset_of!(AetherGameplayEngineApi, structSize),
        core::mem::offset_of!(AetherGameplayEngineApi, abiMajor),
        core::mem::offset_of!(AetherGameplayEngineApi, capabilities),
        core::mem::offset_of!(AetherGameplayEngineApi, context),
        core::mem::offset_of!(AetherGameplayEngineApi, entityValid),
        core::mem::offset_of!(AetherGameplayEngineApi, getPosition),
        core::mem::offset_of!(AetherGameplayEngineApi, setPosition),
        core::mem::offset_of!(AetherGameplayEngineApi, reportDiagnostic),
        size_of::<AetherGameplayModuleApi>(),
        core::mem::align_of::<AetherGameplayModuleApi>(),
        core::mem::offset_of!(AetherGameplayModuleApi, structSize),
        core::mem::offset_of!(AetherGameplayModuleApi, abiMajor),
        core::mem::offset_of!(AetherGameplayModuleApi, capabilities),
        core::mem::offset_of!(AetherGameplayModuleApi, context),
        core::mem::offset_of!(AetherGameplayModuleApi, initialize),
        core::mem::offset_of!(AetherGameplayModuleApi, shutdown),
        core::mem::offset_of!(AetherGameplayModuleApi, getTypeCount),
        core::mem::offset_of!(AetherGameplayModuleApi, getTypeInfo),
        core::mem::offset_of!(AetherGameplayModuleApi, getPropertyInfo),
        core::mem::offset_of!(AetherGameplayModuleApi, create),
        core::mem::offset_of!(AetherGameplayModuleApi, destroy),
        core::mem::offset_of!(AetherGameplayModuleApi, attach),
        core::mem::offset_of!(AetherGameplayModuleApi, update),
        core::mem::offset_of!(AetherGameplayModuleApi, detach),
        core::mem::offset_of!(AetherGameplayModuleApi, getProperty),
        core::mem::offset_of!(AetherGameplayModuleApi, setProperty),
        core::mem::offset_of!(AetherGameplayModuleApi, getDefaultProperty),
        core::mem::offset_of!(AetherGameplayModuleApi, dispatchEvent)];
    values.get(index as usize).copied().unwrap_or(usize::MAX)
}
