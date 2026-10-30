#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "net/NetOwnership.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/GameplayServices.h"

namespace aether::app::scripting
{
	inline void QueueGameplayNetworkDespawn(World& world, Entity entity, std::vector<Entity>& pending)
	{
		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
		{
			const auto children = hierarchy->children;
			for (const auto child: children)
			{
				QueueGameplayNetworkDespawn(world, child, pending);
			}
		}
		ecs::DetachFromParent(world, entity);
		pending.push_back(entity);
	}

	// The caller validates the world and entity handle before invoking this helper.
	inline AetherGameplayStatus ExecuteGameplayNetworkService(
	        net::NetworkContext* network, World& world, aether::scripting::GameplayAccess& access, const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer, std::vector<Entity>& pending)
	{
		const Entity entity{request.entity.id};
		const std::string text = request.text.length ? std::string(request.text.data, request.text.length) : std::string{};
		switch (request.operation)
		{
			case AE_GAMEPLAY_SERVICE_NET_HOST:
			case AE_GAMEPLAY_SERVICE_NET_CONNECT:
				if (request.integer < 0 || request.integer > 65535 || (request.operation == AE_GAMEPLAY_SERVICE_NET_HOST && (request.vector[0] < 0 || request.vector[0] > 65535 || std::floor(request.vector[0]) != request.vector[0]))
				        || (request.operation == AE_GAMEPLAY_SERVICE_NET_CONNECT && (text.empty() || text.find('\0') != std::string::npos)))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = network
				                && (request.operation == AE_GAMEPLAY_SERVICE_NET_HOST ? network->StartHost(world, static_cast<uint16_t>(request.integer), static_cast<int>(request.vector[0]))
				                                                                      : network->StartClient(world, text, static_cast<uint16_t>(request.integer)));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_DISCONNECT:
				if (network)
				{
					network->Stop(world);
				}
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_IS_HOST:
			case AE_GAMEPLAY_SERVICE_NET_IS_CLIENT:
			case AE_GAMEPLAY_SERVICE_NET_IS_CONNECTED:
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = network && (request.operation == AE_GAMEPLAY_SERVICE_NET_IS_HOST ? network->IsHost() : request.operation == AE_GAMEPLAY_SERVICE_NET_IS_CLIENT ? network->IsClient() : network->IsConnected());
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_LOCAL_ID:
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = network ? network->LocalConnectionId() : 0;
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_SPAWN:
			{
				if (request.integer < 0 || request.integer > UINT32_MAX || !net::IsSafePrefabName(text))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				if (!network)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				const Entity spawned = network->SpawnPrefab(world, text, {request.vector[0], request.vector[1], request.vector[2]}, static_cast<uint32_t>(request.integer));
				if (!spawned.IsValid())
				{
					return AE_GAMEPLAY_INVALID_HANDLE;
				}
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = access.Handle(spawned);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_NET_DESPAWN:
				if (network && !network->ReleaseForDespawn(world, entity))
				{
					return AE_GAMEPLAY_NO_AUTHORITY;
				}
				QueueGameplayNetworkDespawn(world, entity, pending);
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_HAS_AUTHORITY:
			case AE_GAMEPLAY_SERVICE_NET_IS_OWNER:
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = !network || (request.operation == AE_GAMEPLAY_SERVICE_NET_HAS_AUTHORITY ? network->HasAuthority(world, entity) : network->IsOwner(world, entity));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_REQUEST_OWNERSHIP:
			case AE_GAMEPLAY_SERVICE_NET_RELEASE_OWNERSHIP:
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer =
				        !network || network->RequestOwnershipTransfer(world, entity, request.operation == AE_GAMEPLAY_SERVICE_NET_REQUEST_OWNERSHIP ? network->LocalConnectionId() : net::kInvalidConnection) != net::OwnershipTransferOutcome::Refused;
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_SET_REPLICATION_READY:
				if (network)
				{
					network->SetReplicationReady(world, request.integer != 0);
				}
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_IS_REPLICATION_READY:
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = !network || network->IsReplicationReady();
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_NET_DISCONNECT_REASON:
			{
				const std::string reason = network ? network->DisconnectReason() : std::string{};
				value.type = AE_GAMEPLAY_PROPERTY_STRING;
				return aether::scripting::CopyGameplayUtf8({reason.data(), static_cast<uint32_t>(reason.size())}, buffer);
			}
			default:
				return AE_GAMEPLAY_UNSUPPORTED;
		}
	}
} // namespace aether::app::scripting
