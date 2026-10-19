//! Raw bindings generated from AetherCore's canonical C gameplay ABI.
//!
//! These types expose the unsafe ABI only. They do not provide game registration,
//! entity lifetime checks, or a gameplay runtime. Callers must follow the native
//! ABI's context, thread, ownership, and borrowed buffer rules.

#![no_std]

mod generated;

pub use generated::*;

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, size_of};

    #[test]
    fn generated_contract_uses_fixed_width_handles_and_nullable_callbacks() {
        assert_eq!(AE_GAMEPLAY_ABI_MAJOR, 1);
        assert_eq!(size_of::<AetherGameplayEntity>(), 16);
        assert_eq!(align_of::<AetherGameplayEntity>(), align_of::<u64>());
        assert_eq!(size_of::<AetherGameplayPropertyValue>(), 48);
        let callback: AetherGameplayQuery = None;
        assert!(callback.is_none());
    }
}

pub mod services;
