#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AE_GAMEPLAY_ABI_MAJOR 1u
#define AE_GAMEPLAY_OK 0
#define AE_GAMEPLAY_INVALID_ARGUMENT 1
#define AE_GAMEPLAY_ABI_MISMATCH 2
#define AE_GAMEPLAY_BUFFER_TOO_SMALL 3
#define AE_GAMEPLAY_UNSUPPORTED 4
#define AE_GAMEPLAY_INVALID_HANDLE 5
#define AE_GAMEPLAY_WRONG_WORLD 6
#define AE_GAMEPLAY_WRONG_THREAD 7
#define AE_GAMEPLAY_REENTRANT 8
#define AE_GAMEPLAY_SCRIPT_ERROR 9
// The target exists but the engine object the call acts on is not created yet (a physics body
// queued this frame); retry on a later tick.
#define AE_GAMEPLAY_NOT_READY 10
// The call would write an entity another peer owns.
#define AE_GAMEPLAY_NO_AUTHORITY 11
#define AE_GAMEPLAY_CAP_PHYSICS UINT64_C(1)
#define AE_GAMEPLAY_CAP_OWNERSHIP UINT64_C(2)
#define AE_GAMEPLAY_CAP_NETWORK UINT64_C(4)
#define AE_GAMEPLAY_PROPERTY_NONE 0u
#define AE_GAMEPLAY_PROPERTY_FLOAT 1u
#define AE_GAMEPLAY_PROPERTY_INT 2u
#define AE_GAMEPLAY_PROPERTY_BOOL 3u
#define AE_GAMEPLAY_PROPERTY_VECTOR3 4u
#define AE_GAMEPLAY_PROPERTY_STRING 5u
#define AE_GAMEPLAY_PROPERTY_ENUM 6u
#define AE_GAMEPLAY_PROPERTY_ENTITY 7u
#define AE_GAMEPLAY_PROPERTY_COMPONENT 8u
#define AE_GAMEPLAY_DIAGNOSTIC_INFO 0u
#define AE_GAMEPLAY_DIAGNOSTIC_WARNING 1u
#define AE_GAMEPLAY_DIAGNOSTIC_ERROR 2u
#define AE_GAMEPLAY_EVENT_PHYSICS 1u
#define AE_GAMEPLAY_EVENT_OWNERSHIP 2u
#define AE_GAMEPLAY_EVENT_NETWORK 3u
// AetherGameplayEvent::flags of an AE_GAMEPLAY_EVENT_PHYSICS event: set when it comes from 3D
// (Jolt) physics, clear for 2D (Box2D). `code` is 0 collision enter, 1 collision exit, 2 trigger
// enter, 3 trigger exit, 4 overlapping, for both.
#define AE_GAMEPLAY_PHYSICS_FLAG_3D 1u

	typedef int32_t AetherGameplayStatus;
	typedef uint64_t AetherGameplayInstance;

	// All pointers are process-local borrowed views. No ABI record is a serialized format.
	// UTF-8 lengths exclude NUL. Inputs may contain NUL and need not be terminated.
	typedef struct AetherGameplayUtf8
	{
		const char* data;
		uint32_t length;
	} AetherGameplayUtf8;

// required includes the trailing NUL. Short buffers return BUFFER_TOO_SMALL, clear data[0]
// when storage exists, and never copy partial UTF-8 text. Always check the returned status.
	typedef struct AetherGameplayUtf8Buffer
	{
		char* data;
		uint32_t capacity;
		uint32_t* required;
	} AetherGameplayUtf8Buffer;

	typedef struct AetherGameplayEntity
	{
		uint64_t world;
		uint32_t id;
		uint32_t reserved;
	} AetherGameplayEntity;

	typedef struct AetherGameplayVec3
	{
		float x;
		float y;
		float z;
	} AetherGameplayVec3;

	// String contents use the separate caller-owned buffer; never store pointers in a value.
	// Scalar and vector lanes not selected by type are zero. Entity/component references use the entity lane, with full generation bits.
	typedef struct AetherGameplayPropertyValue
	{
		uint32_t structSize;
		uint32_t type;
		float vector[4];
		int64_t integer;
		AetherGameplayEntity entity;
	} AetherGameplayPropertyValue;

	typedef struct AetherGameplayTypeInfo
	{
		uint32_t structSize;
		uint32_t propertyCount;
		AetherGameplayUtf8 name;
	} AetherGameplayTypeInfo;

	typedef struct AetherGameplayPropertyInfo
	{
		uint32_t structSize;
		uint32_t type;
		AetherGameplayUtf8 name;
		AetherGameplayUtf8 enumNames;
		AetherGameplayUtf8 componentType;
	} AetherGameplayPropertyInfo;

	typedef struct AetherGameplayEvent
	{
		uint32_t structSize;
		uint32_t kind;
		AetherGameplayEntity other;
		uint32_t code;
		uint32_t flags;
		AetherGameplayUtf8 payload;
	} AetherGameplayEvent;

	typedef struct AetherGameplayDiagnostic
	{
		uint32_t structSize;
		uint32_t severity;
		AetherGameplayUtf8 code;
		AetherGameplayUtf8 message;
		AetherGameplayUtf8 file;
		uint32_t line;
		uint32_t column;
	} AetherGameplayDiagnostic;

	typedef struct AetherGameplayEngineApi
	{
		uint32_t structSize;
		uint32_t abiMajor;
		uint64_t capabilities;
		uint64_t context;
		AetherGameplayStatus (*entityValid)(uint64_t context, AetherGameplayEntity entity, uint32_t* outValid);
		AetherGameplayStatus (*getPosition)(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3* outPosition);
		AetherGameplayStatus (*setPosition)(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3 position);
		AetherGameplayStatus (*reportDiagnostic)(uint64_t context, const AetherGameplayDiagnostic* diagnostic);
	} AetherGameplayEngineApi;

	// Required callbacks return status; errors/panics/exceptions MUST be contained by the provider.
	// Calls run synchronously on the world owner thread, without retained ECS references.
	// Instance lifecycle: create -> attach -> update* -> detach -> destroy. Failed attach still destroys.
	// Descriptor views remain immutable and valid until shutdown. No callbacks after shutdown/unload.
	// Optional events return UNSUPPORTED when their capability bit is absent, never silent success.
	typedef struct AetherGameplayModuleApi
	{
		uint32_t structSize;
		uint32_t abiMajor;
		uint64_t capabilities;
		uint64_t context;
		AetherGameplayStatus (*initialize)(uint64_t context, const AetherGameplayEngineApi* engine);
		AetherGameplayStatus (*shutdown)(uint64_t context);
		AetherGameplayStatus (*getTypeCount)(uint64_t context, uint32_t* outCount);
		AetherGameplayStatus (*getTypeInfo)(uint64_t context, uint32_t typeIndex, AetherGameplayTypeInfo* outInfo);
		AetherGameplayStatus (*getPropertyInfo)(uint64_t context, uint32_t typeIndex, uint32_t propertyIndex, AetherGameplayPropertyInfo* outInfo);
		AetherGameplayStatus (*create)(uint64_t context, uint32_t typeIndex, AetherGameplayEntity entity, AetherGameplayInstance* outInstance);
		AetherGameplayStatus (*destroy)(uint64_t context, AetherGameplayInstance instance);
		AetherGameplayStatus (*attach)(uint64_t context, AetherGameplayInstance instance);
		AetherGameplayStatus (*update)(uint64_t context, AetherGameplayInstance instance, float deltaSeconds);
		AetherGameplayStatus (*detach)(uint64_t context, AetherGameplayInstance instance);
		AetherGameplayStatus (*getProperty)(uint64_t context, AetherGameplayInstance instance, uint32_t propertyIndex, AetherGameplayPropertyValue* outValue, AetherGameplayUtf8Buffer stringBuffer);
		AetherGameplayStatus (*setProperty)(uint64_t context, AetherGameplayInstance instance, uint32_t propertyIndex, const AetherGameplayPropertyValue* value, AetherGameplayUtf8 stringValue);
		AetherGameplayStatus (*getDefaultProperty)(uint64_t context, uint32_t typeIndex, uint32_t propertyIndex, AetherGameplayPropertyValue* outValue, AetherGameplayUtf8Buffer stringBuffer);
		AetherGameplayStatus (*dispatchEvent)(uint64_t context, AetherGameplayInstance instance, const AetherGameplayEvent* event);
	} AetherGameplayModuleApi;

	typedef AetherGameplayStatus (*AetherGameplayQuery)(uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi);
	// Modules export this symbol. The caller clears the table before invoking and validates on return.
	AetherGameplayStatus aether_gameplay_query(uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi);

#ifdef __cplusplus
}
#endif
