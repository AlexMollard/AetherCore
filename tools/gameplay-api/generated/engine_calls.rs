// Generated legacy raw exports; callers must obey engine context/lifetime rules.
use super::gameplay_abi::AetherGameplayVec3;
unsafe extern "C" {
    pub fn aether_entity_create() -> u32;
    pub fn aether_entity_destroy(id: u32) -> ();
    pub fn aether_entity_valid(id: u32) -> i32;
    pub fn aether_get_position(id: u32) -> AetherGameplayVec3;
    pub fn aether_set_position(id: u32, pos: AetherGameplayVec3) -> ();
}
