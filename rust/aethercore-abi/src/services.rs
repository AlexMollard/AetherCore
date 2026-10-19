//! Optional services extension, separately negotiated from immutable gameplay ABI v1.
#![allow(non_snake_case)]
use crate::*;
pub const AE_GAMEPLAY_SERVICES_MAJOR: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayServiceRequest {
    pub structSize: u32,
    pub operation: u32,
    pub entity: AetherGameplayEntity,
    pub vector: [f32; 4],
    pub integer: i64,
    pub text: AetherGameplayUtf8,
    pub text2: AetherGameplayUtf8,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AetherGameplayServicesApi {
    pub structSize: u32,
    pub abiMajor: u32,
    pub context: u64,
    pub capabilities: u64,
    pub invoke: Option<
        unsafe extern "C" fn(
            u64,
            *const AetherGameplayServiceRequest,
            *mut AetherGameplayPropertyValue,
            AetherGameplayUtf8Buffer,
        ) -> AetherGameplayStatus,
    >,
}
