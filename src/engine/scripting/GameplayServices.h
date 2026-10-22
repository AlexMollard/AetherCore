#pragma once

#include "GameplayAbi.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AE_GAMEPLAY_SERVICES_ABI_MAJOR 1u
#define AE_GAMEPLAY_SERVICE_KEY_DOWN 1u
#define AE_GAMEPLAY_SERVICE_SET_SPRITE_COLOR 2u
#define AE_GAMEPLAY_SERVICE_GET_VELOCITY_2D 3u
#define AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D 4u
#define AE_GAMEPLAY_SERVICE_FIND_ENTITY 5u
#define AE_GAMEPLAY_SERVICE_LOAD_SCENE 6u
#define AE_GAMEPLAY_SERVICE_INSTANTIATE_PREFAB 7u
#define AE_GAMEPLAY_SERVICE_PLAY_AUDIO 8u
#define AE_GAMEPLAY_SERVICE_SET_UI_TEXT 9u
#define AE_GAMEPLAY_SERVICE_READ_SAVE 10u
#define AE_GAMEPLAY_SERVICE_WRITE_SAVE 11u
#define AE_GAMEPLAY_SERVICE_CREATE_ENTITY 12u
#define AE_GAMEPLAY_SERVICE_DESTROY_ENTITY 13u
// Project custom render passes (CustomPass in the C# SDK). Each frame a script re-submits every
// pass it wants drawn; a pass that is not submitted that frame does not draw.
//   REGISTER_CUSTOM_PASS:   text = pass name, text2 = project shader stem, integer = stage
//                           (0 BehindScene, 1 OverScene, 2 EmissiveOverLight). Re-registering replaces.
//   UNREGISTER_CUSTOM_PASS: text = pass name. Unknown names are a no-op.
//   SUBMIT_CUSTOM_PASS:     text = pass name, text2 = raw little-endian float32 payload that is
//                           NOT UTF-8: three header float4 (params, color0, color1) followed by the
//                           pass's data float4 elements, so the byte length is 16 * (3 + count) and
//                           at most AE_GAMEPLAY_CUSTOM_PASS_MAX_VEC4 data elements are used (extra
//                           elements are dropped, matching the C# clamp). Submitting to an
//                           unregistered name is a successful no-op.
#define AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS 14u
#define AE_GAMEPLAY_SERVICE_UNREGISTER_CUSTOM_PASS 15u
#define AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS 16u
#define AE_GAMEPLAY_CUSTOM_PASS_MAX_VEC4 4096u
#define AE_GAMEPLAY_CUSTOM_PASS_HEADER_VEC4 3u
// 3D gameplay (Jolt physics, character controller, ragdoll, skinned animation). Implemented once in
// Gameplay3D.cpp and shared by the native Rust host and the browser host. `entity` is the target
// entity for every operation except RAYCAST_3D. Failures are explicit: a missing component or system
// is UNSUPPORTED, a malformed/non-finite argument INVALID_ARGUMENT, a stale entity INVALID_HANDLE, a
// body that is not baked yet NOT_READY, a write to an entity the peer does not own NO_AUTHORITY.
//   GET_VELOCITY_3D:       result VECTOR3 = linear velocity (m/s).
//   SET_VELOCITY_3D:       vector[0..2] = linear velocity. Before the body is baked it seeds the
//                          initial velocity, so set-then-get reads back the written value.
//   ADD_FORCE_3D:          vector[0..2] = force (N) for the next physics step. NOT_READY before bake.
//   ADD_IMPULSE_3D:        vector[0..2] = impulse (N*s) at the centre of mass. NOT_READY before bake.
//   RAYCAST_3D:            text2 = raw little-endian float32 payload (NOT UTF-8) of 7 floats: origin
//                          xyz, direction xyz (non-zero, any length), max distance (> 0), so exactly 28
//                          bytes. Result: integer = 1 hit / 0 miss; on a hit vector[0..2] = hit point,
//                          vector[3] = distance along the ray, entity = hit entity (none when the body
//                          has no entity), and the string buffer carries the surface normal as three raw
//                          little-endian float32 (12 bytes + NUL). A miss leaves them zero.
//   CHARACTER_MOVE:        vector[0..2] = direction, vector[3] = speed; sets the desired horizontal
//                          velocity (direction * speed) of a CharacterController until the next call.
//   CHARACTER_JUMP:        vector[0] = jump speed; one-shot, applied when the character is grounded.
//   CHARACTER_IS_GROUNDED: result BOOL.
//   SPAWN_RAGDOLL:         text = skeleton VFS path (glTF/.mesh); builds a ragdoll on `entity`, which
//                          becomes its root. Result ENTITY = the root; integer = bone count (root included).
//   SET_ANIMATION_CLIP:    text = clip name; when empty `integer` = clip index. vector[0] = cross-fade
//                          seconds (0 switches outright). Applies to the entity's skinned mesh and
//                          its spawned child meshes. An unknown clip is INVALID_ARGUMENT.
//   SET_ANIMATION_SPEED:   vector[0] = playback speed multiplier.
//   GET_ANIMATION_TIME:    result FLOAT = seconds into the playing clip.
//   FIND_ANIMATION_CLIP:   text = clip name; result INT = clip index, INVALID_ARGUMENT when unknown.
//   GET_CURRENT_CLIP:      result INT = index of the playing clip.
// 3D collision/trigger events are not operations: they are delivered as AE_GAMEPLAY_EVENT_PHYSICS
// events with AE_GAMEPLAY_PHYSICS_FLAG_3D set (see GameplayAbi.h).
#define AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D 17u
#define AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D 18u
#define AE_GAMEPLAY_SERVICE_ADD_FORCE_3D 19u
#define AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D 20u
#define AE_GAMEPLAY_SERVICE_RAYCAST_3D 21u
#define AE_GAMEPLAY_SERVICE_CHARACTER_MOVE 22u
#define AE_GAMEPLAY_SERVICE_CHARACTER_JUMP 23u
#define AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED 24u
#define AE_GAMEPLAY_SERVICE_SPAWN_RAGDOLL 25u
#define AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP 26u
#define AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED 27u
#define AE_GAMEPLAY_SERVICE_GET_ANIMATION_TIME 28u
#define AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP 29u
#define AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP 30u
#define AE_GAMEPLAY_RAYCAST_PAYLOAD_BYTES 28u
#define AE_GAMEPLAY_SERVICE_CAP(operation) (UINT64_C(1) << (operation))

	typedef struct AetherGameplayServiceRequest
	{
		uint32_t structSize;
		uint32_t operation;
		AetherGameplayEntity entity;
		float vector[4];
		int64_t integer;
		AetherGameplayUtf8 text;
		AetherGameplayUtf8 text2;
	} AetherGameplayServiceRequest;

	// Independent optional extension; GameplayAbi.h v1 remains unchanged.
	// Inputs and output buffers are borrowed for this synchronous call only.
	typedef struct AetherGameplayServicesApi
	{
		uint32_t structSize;
		uint32_t abiMajor;
		uint64_t context;
		uint64_t capabilities;
		AetherGameplayStatus (*invoke)(uint64_t context, const AetherGameplayServiceRequest* request, AetherGameplayPropertyValue* outValue, AetherGameplayUtf8Buffer stringBuffer);
	} AetherGameplayServicesApi;

	typedef AetherGameplayStatus (*AetherGameplayBindServices)(uint32_t requestedMajor, uint32_t apiSize, const AetherGameplayServicesApi* api);
	AetherGameplayStatus aether_gameplay_bind_services(uint32_t requestedMajor, uint32_t apiSize, const AetherGameplayServicesApi* api);

#ifdef __cplusplus
}
#endif
