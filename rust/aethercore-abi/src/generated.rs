// Generated from src/engine/scripting/GameplayAbi.h. Do not edit.
#![allow(non_snake_case, dead_code)]
pub const AE_GAMEPLAY_ABI_MAJOR: u32 = 1;
pub const AE_GAMEPLAY_OK: i32 = 0;
pub const AE_GAMEPLAY_INVALID_ARGUMENT: i32 = 1;
pub const AE_GAMEPLAY_ABI_MISMATCH: i32 = 2;
pub const AE_GAMEPLAY_BUFFER_TOO_SMALL: i32 = 3;
pub const AE_GAMEPLAY_UNSUPPORTED: i32 = 4;
pub const AE_GAMEPLAY_INVALID_HANDLE: i32 = 5;
pub const AE_GAMEPLAY_WRONG_WORLD: i32 = 6;
pub const AE_GAMEPLAY_WRONG_THREAD: i32 = 7;
pub const AE_GAMEPLAY_REENTRANT: i32 = 8;
pub const AE_GAMEPLAY_SCRIPT_ERROR: i32 = 9;
pub const AE_GAMEPLAY_CAP_PHYSICS: u64 = 1;
pub const AE_GAMEPLAY_CAP_OWNERSHIP: u64 = 2;
pub const AE_GAMEPLAY_CAP_NETWORK: u64 = 4;
pub const AE_GAMEPLAY_PROPERTY_NONE: u32 = 0;
pub const AE_GAMEPLAY_PROPERTY_FLOAT: u32 = 1;
pub const AE_GAMEPLAY_PROPERTY_INT: u32 = 2;
pub const AE_GAMEPLAY_PROPERTY_BOOL: u32 = 3;
pub const AE_GAMEPLAY_PROPERTY_VECTOR3: u32 = 4;
pub const AE_GAMEPLAY_PROPERTY_STRING: u32 = 5;
pub const AE_GAMEPLAY_PROPERTY_ENUM: u32 = 6;
pub const AE_GAMEPLAY_PROPERTY_ENTITY: u32 = 7;
pub const AE_GAMEPLAY_PROPERTY_COMPONENT: u32 = 8;
pub const AE_GAMEPLAY_DIAGNOSTIC_INFO: u32 = 0;
pub const AE_GAMEPLAY_DIAGNOSTIC_WARNING: u32 = 1;
pub const AE_GAMEPLAY_DIAGNOSTIC_ERROR: u32 = 2;
pub const AE_GAMEPLAY_EVENT_PHYSICS: u32 = 1;
pub const AE_GAMEPLAY_EVENT_OWNERSHIP: u32 = 2;
pub const AE_GAMEPLAY_EVENT_NETWORK: u32 = 3;
pub type AetherGameplayStatus = i32;
pub type AetherGameplayInstance = u64;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayUtf8 {
    pub data: *const core::ffi::c_char,
    pub length: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayUtf8Buffer {
    pub data: *mut core::ffi::c_char,
    pub capacity: u32,
    pub required: *mut u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayEntity {
    pub world: u64,
    pub id: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayVec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayPropertyValue {
    pub structSize: u32,
    pub r#type: u32,
    pub vector: [f32; 4],
    pub integer: i64,
    pub entity: AetherGameplayEntity,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayTypeInfo {
    pub structSize: u32,
    pub propertyCount: u32,
    pub name: AetherGameplayUtf8,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayPropertyInfo {
    pub structSize: u32,
    pub r#type: u32,
    pub name: AetherGameplayUtf8,
    pub enumNames: AetherGameplayUtf8,
    pub componentType: AetherGameplayUtf8,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayEvent {
    pub structSize: u32,
    pub kind: u32,
    pub other: AetherGameplayEntity,
    pub code: u32,
    pub flags: u32,
    pub payload: AetherGameplayUtf8,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayDiagnostic {
    pub structSize: u32,
    pub severity: u32,
    pub code: AetherGameplayUtf8,
    pub message: AetherGameplayUtf8,
    pub file: AetherGameplayUtf8,
    pub line: u32,
    pub column: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayEngineApi {
    pub structSize: u32,
    pub abiMajor: u32,
    pub capabilities: u64,
    pub context: u64,
    pub entityValid: Option<unsafe extern "C" fn(context: u64, entity: AetherGameplayEntity, outValid: *mut u32) -> AetherGameplayStatus>,
    pub getPosition: Option<unsafe extern "C" fn(context: u64, entity: AetherGameplayEntity, outPosition: *mut AetherGameplayVec3) -> AetherGameplayStatus>,
    pub setPosition: Option<unsafe extern "C" fn(context: u64, entity: AetherGameplayEntity, position: AetherGameplayVec3) -> AetherGameplayStatus>,
    pub reportDiagnostic: Option<unsafe extern "C" fn(context: u64, diagnostic: *const AetherGameplayDiagnostic) -> AetherGameplayStatus>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayModuleApi {
    pub structSize: u32,
    pub abiMajor: u32,
    pub capabilities: u64,
    pub context: u64,
    pub initialize: Option<unsafe extern "C" fn(context: u64, engine: *const AetherGameplayEngineApi) -> AetherGameplayStatus>,
    pub shutdown: Option<unsafe extern "C" fn(context: u64) -> AetherGameplayStatus>,
    pub getTypeCount: Option<unsafe extern "C" fn(context: u64, outCount: *mut u32) -> AetherGameplayStatus>,
    pub getTypeInfo: Option<unsafe extern "C" fn(context: u64, typeIndex: u32, outInfo: *mut AetherGameplayTypeInfo) -> AetherGameplayStatus>,
    pub getPropertyInfo: Option<unsafe extern "C" fn(context: u64, typeIndex: u32, propertyIndex: u32, outInfo: *mut AetherGameplayPropertyInfo) -> AetherGameplayStatus>,
    pub create: Option<unsafe extern "C" fn(context: u64, typeIndex: u32, entity: AetherGameplayEntity, outInstance: *mut AetherGameplayInstance) -> AetherGameplayStatus>,
    pub destroy: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance) -> AetherGameplayStatus>,
    pub attach: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance) -> AetherGameplayStatus>,
    pub update: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance, deltaSeconds: f32) -> AetherGameplayStatus>,
    pub detach: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance) -> AetherGameplayStatus>,
    pub getProperty: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance, propertyIndex: u32, outValue: *mut AetherGameplayPropertyValue, stringBuffer: AetherGameplayUtf8Buffer) -> AetherGameplayStatus>,
    pub setProperty: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance, propertyIndex: u32, value: *const AetherGameplayPropertyValue, stringValue: AetherGameplayUtf8) -> AetherGameplayStatus>,
    pub getDefaultProperty: Option<unsafe extern "C" fn(context: u64, typeIndex: u32, propertyIndex: u32, outValue: *mut AetherGameplayPropertyValue, stringBuffer: AetherGameplayUtf8Buffer) -> AetherGameplayStatus>,
    pub dispatchEvent: Option<unsafe extern "C" fn(context: u64, instance: AetherGameplayInstance, event: *const AetherGameplayEvent) -> AetherGameplayStatus>,
}
pub type AetherGameplayQuery = Option<unsafe extern "C" fn(requestedMajor: u32, outSize: u32, outApi: *mut AetherGameplayModuleApi) -> AetherGameplayStatus>;
unsafe extern "C" {
    pub fn aether_gameplay_query(requestedMajor: u32, outSize: u32, outApi: *mut AetherGameplayModuleApi) -> AetherGameplayStatus;
}
