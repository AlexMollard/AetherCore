//! Optional services extension, separately negotiated from immutable gameplay ABI v1.
#![allow(non_snake_case)]
use crate::*;
pub const AE_GAMEPLAY_SERVICES_MAJOR: u32 = 1;
pub const AE_GAMEPLAY_SERVICE_MOUSE_DOWN: u32 = 31;
pub const AE_GAMEPLAY_SERVICE_MOUSE_DELTA: u32 = 32;
pub const AE_GAMEPLAY_SERVICE_SCROLL_DELTA: u32 = 33;
pub const AE_GAMEPLAY_SERVICE_SET_EULER: u32 = 34;
pub const AE_GAMEPLAY_SERVICE_NET_HOST: u32 = 35;
pub const AE_GAMEPLAY_SERVICE_NET_CONNECT: u32 = 36;
pub const AE_GAMEPLAY_SERVICE_NET_DISCONNECT: u32 = 37;
pub const AE_GAMEPLAY_SERVICE_NET_IS_HOST: u32 = 38;
pub const AE_GAMEPLAY_SERVICE_NET_IS_CLIENT: u32 = 39;
pub const AE_GAMEPLAY_SERVICE_NET_IS_CONNECTED: u32 = 40;
pub const AE_GAMEPLAY_SERVICE_NET_LOCAL_ID: u32 = 41;
pub const AE_GAMEPLAY_SERVICE_NET_SPAWN: u32 = 42;
pub const AE_GAMEPLAY_SERVICE_NET_DESPAWN: u32 = 43;
pub const AE_GAMEPLAY_SERVICE_NET_HAS_AUTHORITY: u32 = 44;
pub const AE_GAMEPLAY_SERVICE_NET_IS_OWNER: u32 = 45;
pub const AE_GAMEPLAY_SERVICE_NET_REQUEST_OWNERSHIP: u32 = 46;
pub const AE_GAMEPLAY_SERVICE_NET_RELEASE_OWNERSHIP: u32 = 47;
pub const AE_GAMEPLAY_SERVICE_NET_SET_REPLICATION_READY: u32 = 48;
pub const AE_GAMEPLAY_SERVICE_NET_IS_REPLICATION_READY: u32 = 49;
pub const AE_GAMEPLAY_SERVICE_NET_DISCONNECT_REASON: u32 = 50;
pub const AE_GAMEPLAY_SERVICE_STREAMING_SET_SOURCE: u32 = 54;
pub const AE_GAMEPLAY_SERVICE_STREAMING_REMOVE_SOURCE: u32 = 55;
pub const AE_GAMEPLAY_SERVICE_STREAMING_REQUEST: u32 = 56;
pub const AE_GAMEPLAY_SERVICE_STREAMING_CANCEL: u32 = 57;
pub const AE_GAMEPLAY_SERVICE_STREAMING_RELEASE: u32 = 58;
pub const AE_GAMEPLAY_SERVICE_STREAMING_STATE: u32 = 59;
pub const AE_GAMEPLAY_SERVICE_STREAMING_DIAGNOSTICS: u32 = 60;
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

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of};

    #[test]
    fn service_request_matches_native_offsets_and_pointer_width() {
        assert!(matches!(size_of::<usize>(), 4 | 8));
        assert_eq!(offset_of!(AetherGameplayServiceRequest, structSize), 0);
        assert_eq!(offset_of!(AetherGameplayServiceRequest, operation), 4);
        assert_eq!(offset_of!(AetherGameplayServiceRequest, entity), 8);
        assert_eq!(offset_of!(AetherGameplayServiceRequest, vector), 24);
        assert_eq!(offset_of!(AetherGameplayServiceRequest, integer), 40);
        assert_eq!(offset_of!(AetherGameplayServiceRequest, text), 48);
        assert_eq!(
            offset_of!(AetherGameplayServiceRequest, text2),
            if size_of::<usize>() == 8 { 64 } else { 56 }
        );
        assert_eq!(
            size_of::<AetherGameplayServiceRequest>(),
            if size_of::<usize>() == 8 { 80 } else { 64 }
        );
        assert_eq!(
            align_of::<AetherGameplayServiceRequest>(),
            align_of::<u64>().max(align_of::<usize>())
        );
    }

    #[test]
    fn service_table_matches_native_offsets_and_nullable_callback_layout() {
        assert_eq!(offset_of!(AetherGameplayServicesApi, structSize), 0);
        assert_eq!(offset_of!(AetherGameplayServicesApi, abiMajor), 4);
        assert_eq!(offset_of!(AetherGameplayServicesApi, context), 8);
        assert_eq!(offset_of!(AetherGameplayServicesApi, capabilities), 16);
        assert_eq!(offset_of!(AetherGameplayServicesApi, invoke), 24);
        // 32-bit targets with eight-byte u64 alignment retain trailing padding.
        assert_eq!(
            size_of::<AetherGameplayServicesApi>(),
            if size_of::<usize>() == 8 || align_of::<u64>() == 8 {
                32
            } else {
                28
            }
        );
        assert_eq!(
            align_of::<AetherGameplayServicesApi>(),
            align_of::<u64>().max(align_of::<usize>())
        );
        let table = AetherGameplayServicesApi {
            structSize: size_of::<AetherGameplayServicesApi>() as u32,
            abiMajor: AE_GAMEPLAY_SERVICES_MAJOR,
            context: 0,
            capabilities: 0,
            invoke: None,
        };
        assert_eq!(size_of_val(&table.invoke), size_of::<usize>());
        assert!(table.invoke.is_none());
    }
}
