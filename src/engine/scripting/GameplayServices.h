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
