#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::reflect
{
	enum class FieldType : std::uint8_t
	{
		Float,
		Int,
		UInt,
		Bool,
		Vec2,
		Vec3,
		Vec4,
		Color3,
		Color4,
		Enum,
		String,
		EntityRef,
		// A homogeneous list of small structs - each element is a fixed set of scalar
		// sub-fields (see FieldDesc::elementFields). Stored in FieldValue::list.
		List,
	};

	struct EnumTable
	{
		std::vector<std::pair<std::string, int>> values;
		[[nodiscard]] std::string NameOf(int value) const;
		[[nodiscard]] int ValueOf(std::string_view name, int fallback) const;
	};

	struct FieldMeta
	{
		float min = 0.0f;
		float max = 0.0f;
		float speed = 0.0f;
		bool isAngleDegrees = false;
		bool serialize = true;
		// Marks the field for network replication. One flag here is what makes
		// "mark it replicated" a one-line change in the same declaration that
		// already drives MCP, the inspector and the serializer.
		bool replicated = false;
		const EnumTable* enumTable = nullptr;
		std::string tooltip;
		// TOML key to serialize under, when it differs from the field name (e.g. an
		std::string serializeName;
	};

	struct FieldValue
	{
		FieldType type = FieldType::Float;
		double num = 0.0;
		bool boolean = false;
		glm::vec4 vec{0.0f};
		int enumValue = 0;
		std::uint64_t entity = 0;
		std::string str;
		// FieldType::List only: one inner vector per element, each holding the element's
		// sub-field values positionally matching FieldDesc::elementFields.
		std::vector<std::vector<FieldValue>> list;
	};

	// One sub-field of a FieldType::List element (name + scalar type). Elements are flat
	// structs of scalars; nested lists are not modelled.
	struct ListElementDesc
	{
		std::string name;
		FieldType type = FieldType::Float;
	};

	struct FieldDesc
	{
		std::string name;
		FieldType type = FieldType::Float;
		FieldMeta meta;
		std::function<FieldValue(const void* component)> get;
		std::function<void(void* component, const FieldValue&)> set;
		// FieldType::List only: the schema of each element's sub-fields, used by the TOML
		// codec, MCP JSON and the inspector to name/type the rows in FieldValue::list.
		std::vector<ListElementDesc> elementFields;
	};

	// Bespoke TOML for the components whose on-disk shape isn't a flat field list
	struct CustomSerializeFns
	{
		std::function<void(const void* component, void* tomlTable)> write;
		std::function<void(void* component, const void* tomlTable)> read;
	};

	struct ComponentType
	{
		std::string name;
		std::string category;
		std::string icon;
		// entt's name for the underlying C++ type, e.g. "struct aether::SpriteRendererComponent".
		// Empty for a type registered without ECS ops.
		std::string cppTypeName;
		std::vector<FieldDesc> fields;

		std::function<bool(const World&, Entity)> has;
		std::function<void*(World&, Entity)> emplaceDefault;
		std::function<void(World&, Entity)> remove;
		std::function<void*(World&, Entity)> tryGetRaw;
		std::function<const void*(const World&, Entity)> tryGetRawConst;

		bool addable = true; // false = reference-only, never added to an arbitrary entity
		bool serializable = true;
		// true = a pure data-only component (no asset resolution, physics bodies,
		// cross-entity refs, or bespoke on-disk shape) whose capture/apply/TOML codec
		// are driven entirely from the reflected fields here. This flag IS the registry
		// of generic components - the scene serializer scans ComponentTypes() for it,
		// so a new data-only component only needs its declaration plus AE_GENERIC_SERIALIZE.
		bool genericSerialize = false;
		// true = the editor's ComponentCatalog builds a bespoke palette entry for this
		// component by hand (richer add behavior - ensure Transform, seed defaults from
		// position, set conflicts). The catalog skips auto-generating a plain reflected
		// entry for it, keyed off this flag rather than a fragile DisplayName string match.
		bool hasHandAuthoredCatalogEntry = false;
		// TOML table key this component serializes under when genericSerialize is set and
		// the on-disk key differs from the display name's derived key (e.g. "Particle
		// Emitter" persists under the legacy "particles" key). Empty = derive from name.
		std::string serializeKey;

		// Scene features this component implies. Adding it auto-enables them on
		// the world (metadata only - every component is legal in every scene).
		SceneFeatureFlags requiredFeatures = SceneFeatureFlags::None;
		// Catalog-entry names that must NOT be present on the entity (e.g. the
		// 2D/3D physics domain exclusivity). Checked by ComponentAddBlockReason.
		std::vector<std::string> conflictsWith;

		std::function<void(World&, Entity)> postSet;
		CustomSerializeFns customSerialize;

		[[nodiscard]] const FieldDesc* FindField(std::string_view fieldName) const;
	};

	// A component with no reflection entry still has to be named in API output. Turns the
	// compiler's spelling - "struct aether::PhysicsStateComponent" - into "Physics State", so
	// one list does not mix human names with C++ identifiers.
	[[nodiscard]] std::string PrettyComponentName(std::string_view cppTypeName);

	const std::vector<ComponentType>& ComponentTypes();
	const ComponentType* FindComponentType(std::string_view name);
	void RegisterComponent(ComponentType type);

	inline FieldValue MakeValue(float v)
	{
		FieldValue f;
		f.type = FieldType::Float;
		f.num = v;
		return f;
	}

	inline FieldValue MakeValue(int v)
	{
		FieldValue f;
		f.type = FieldType::Int;
		f.num = v;
		return f;
	}

	inline FieldValue MakeValue(std::uint32_t v)
	{
		FieldValue f;
		f.type = FieldType::UInt;
		f.num = static_cast<double>(v);
		return f;
	}

	inline FieldValue MakeValue(bool v)
	{
		FieldValue f;
		f.type = FieldType::Bool;
		f.boolean = v;
		return f;
	}

	inline FieldValue MakeValue(const glm::vec2& v)
	{
		FieldValue f;
		f.type = FieldType::Vec2;
		f.vec = glm::vec4(v, 0.0f, 0.0f);
		return f;
	}

	inline FieldValue MakeValue(const glm::vec3& v)
	{
		FieldValue f;
		f.type = FieldType::Vec3;
		f.vec = glm::vec4(v, 0.0f);
		return f;
	}

	inline FieldValue MakeValue(const glm::vec4& v)
	{
		FieldValue f;
		f.type = FieldType::Vec4;
		f.vec = v;
		return f;
	}

	inline FieldValue MakeValue(const std::string& v)
	{
		FieldValue f;
		f.type = FieldType::String;
		f.str = v;
		return f;
	}

	inline FieldValue MakeValue(Entity v)
	{
		FieldValue f;
		f.type = FieldType::EntityRef;
		f.entity = v.id;
		return f;
	}

	inline void ApplyValue(const FieldValue& f, float& out)
	{
		out = static_cast<float>(f.num);
	}

	inline void ApplyValue(const FieldValue& f, int& out)
	{
		// Clamp before the cast: a double outside the type's range (a hand-edited file,
		// cooked binary, or MCP number) is UB to convert and wraps to an arbitrary
		// value instead of saturating; AE_ASSERT guards do not ship.
		out = static_cast<int>(std::clamp(f.num, static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())));
	}

	inline void ApplyValue(const FieldValue& f, std::uint32_t& out)
	{
		// Same clamp on the upper end the negative case already guards below zero.
		out = static_cast<std::uint32_t>(std::clamp(f.num, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
	}

	inline void ApplyValue(const FieldValue& f, bool& out)
	{
		out = f.boolean;
	}

	inline void ApplyValue(const FieldValue& f, glm::vec2& out)
	{
		out = glm::vec2(f.vec);
	}

	inline void ApplyValue(const FieldValue& f, glm::vec3& out)
	{
		out = glm::vec3(f.vec);
	}

	inline void ApplyValue(const FieldValue& f, glm::vec4& out)
	{
		out = f.vec;
	}

	inline void ApplyValue(const FieldValue& f, std::string& out)
	{
		out = f.str;
	}

	inline void ApplyValue(const FieldValue& f, Entity& out)
	{
		out = Entity{static_cast<std::uint32_t>(f.entity)};
	}

	class ComponentBuilder
	{
	public:
		ComponentBuilder(std::string name, std::string category, std::string icon)
		{
			m_type.name = std::move(name);
			m_type.category = std::move(category);
			m_type.icon = std::move(icon);
		}

		template<typename C>
		void SetEcsOps()
		{
			// The same string entt's storage reports, so a caller holding a raw storage name
			// can map it back to the catalog name callers actually pass to get/set_component.
			m_type.cppTypeName = entt::type_id<C>().name();
			m_type.has = [](const World& w, Entity e)
			{
				return w.Has<C>(e);
			};
			m_type.emplaceDefault = [](World& w, Entity e) -> void*
			{
				return &w.EmplaceOrReplace<C>(e, C{});
			};
			m_type.remove = [](World& w, Entity e)
			{
				if (w.Has<C>(e))
				{
					w.Remove<C>(e);
				}
			};
			m_type.tryGetRaw = [](World& w, Entity e) -> void*
			{
				return w.TryGet<C>(e);
			};
			m_type.tryGetRawConst = [](const World& w, Entity e) -> const void*
			{
				return w.TryGet<C>(e);
			};
		}

		template<typename C, typename M>
		ComponentBuilder& Field(const char* name, FieldType tag, M C::* member, const FieldMeta& meta = {})
		{
			FieldDesc f;
			f.name = name;
			f.type = tag;
			f.meta = meta;
			const bool angle = meta.isAngleDegrees;
			f.get = [member, tag, angle](const void* comp) -> FieldValue
			{
				FieldValue v = MakeValue(static_cast<const C*>(comp)->*member);
				v.type = tag;
				if (angle)
				{
					v.num = glm::degrees(v.num);
				}
				return v;
			};
			f.set = [member, angle](void* comp, const FieldValue& in)
			{
				FieldValue v = in;
				if (angle)
				{
					v.num = glm::radians(v.num);
				}
				ApplyValue(v, static_cast<C*>(comp)->*member);
			};
			m_type.fields.push_back(std::move(f));
			return *this;
		}

		ComponentBuilder& CustomField(const char* name, FieldType tag, std::function<FieldValue(const void*)> get, std::function<void(void*, const FieldValue&)> set, FieldMeta meta = {})
		{
			FieldDesc f;
			f.name = name;
			f.type = tag;
			f.meta = std::move(meta);
			f.get = std::move(get);
			f.set = std::move(set);
			m_type.fields.push_back(std::move(f));
			return *this;
		}

		// A FieldType::List field. `elementFields` names/types each element's sub-fields;
		// get/set convert between the component's list and FieldValue::list (rows of
		// sub-field values, positional to elementFields). The setter is the place for any
		// element validation (clamp/sort/defaults).
		ComponentBuilder& CustomListField(const char* name, std::vector<ListElementDesc> elementFields, std::function<FieldValue(const void*)> get, std::function<void(void*, const FieldValue&)> set, FieldMeta meta = {})
		{
			FieldDesc f;
			f.name = name;
			f.type = FieldType::List;
			f.meta = std::move(meta);
			f.elementFields = std::move(elementFields);
			f.get = std::move(get);
			f.set = std::move(set);
			m_type.fields.push_back(std::move(f));
			return *this;
		}

		// the declaration file - see AE_FIELD_ENUM). Must outlive the registry (statics do).
		template<typename C, typename E>
		ComponentBuilder& EnumField(const char* name, E C::* member, const EnumTable& table)
		{
			FieldDesc f;
			f.name = name;
			f.type = FieldType::Enum;
			f.meta.enumTable = &table;
			f.get = [member](const void* comp) -> FieldValue // the table is reached via f.meta, not captured
			{
				FieldValue v;
				v.type = FieldType::Enum;
				v.enumValue = static_cast<int>(static_cast<const C*>(comp)->*member);
				return v;
			};
			f.set = [member, &table](void* comp, const FieldValue& in)
			{
				// Integers reach this setter unvalidated from files and MCP (only enum
				// STRINGS are range-checked, via ValueOf's fallback in the TOML reader), and
				// an enumerator the table does not name re-serializes as NameOf(v) == "" -
				// silently blanking the key on the next save. Mirror the string fallback:
				// keep the current value rather than store an out-of-range one.
				if (!table.NameOf(in.enumValue).empty())
				{
					static_cast<C*>(comp)->*member = static_cast<E>(in.enumValue);
				}
			};
			m_type.fields.push_back(std::move(f));
			return *this;
		}
		ComponentBuilder& Addable(bool v)
		{
			m_type.addable = v;
			return *this;
		}

		ComponentBuilder& Serializable(bool v)
		{
			m_type.serializable = v;
			return *this;
		}

		ComponentBuilder& GenericSerialize(bool v = true)
		{
			m_type.genericSerialize = v;
			return *this;
		}

		ComponentBuilder& SerializeKey(std::string key)
		{
			m_type.serializeKey = std::move(key);
			return *this;
		}

		ComponentBuilder& HandAuthoredCatalogEntry(bool v = true)
		{
			m_type.hasHandAuthoredCatalogEntry = v;
			return *this;
		}

		ComponentBuilder& PostSet(std::function<void(World&, Entity)> fn)
		{
			m_type.postSet = std::move(fn);
			return *this;
		}

		ComponentBuilder& RequiresFeature(SceneFeatureFlags features)
		{
			m_type.requiredFeatures = m_type.requiredFeatures | features;
			return *this;
		}

		ComponentBuilder& ConflictsWith(std::vector<std::string> names)
		{
			m_type.conflictsWith = std::move(names);
			return *this;
		}

		ComponentType Build() &&
		{
			return std::move(m_type);
		}

	private:
		ComponentType m_type;
	};
} // namespace aether::reflect

#define AE_COMPONENT(CppType, DisplayName, Category, Icon)                            \
	namespace aether::reflect::detail                                                \
	{                                                                                \
		static const bool CppType##_registered = []                                  \
		{                                                                            \
			using C = CppType;                                                       \
			::aether::reflect::ComponentBuilder b{DisplayName, Category, Icon};       \
			b.SetEcsOps<C>();

#define AE_FIELD(member, TypeTag) b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member);

#define AE_FIELD_N(name, member, TypeTag) b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member);

// Replicated counterpart of AE_FIELD_N: identical, plus the network schema picks it up.
#define AE_FIELD_REP(name, member, TypeTag) b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.replicated = true});

#define AE_FIELD_R(member, TypeTag, lo, hi) \
	b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.min = (lo), .max = (hi)});

// Tooltip-carrying counterparts. The Inspector shows `tooltip` on hover, so a field whose
// name does not give away what it does can explain itself where it is edited rather than in
// a document nobody has open.
#define AE_FIELD_T(member, TypeTag, tip) b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.tooltip = (tip)});

#define AE_FIELD_NT(name, member, TypeTag, tip) b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.tooltip = (tip)});

#define AE_FIELD_RT(member, TypeTag, lo, hi, tip) 	b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.min = (lo), .max = (hi), .tooltip = (tip)});

// Named counterpart of AE_FIELD_R, for when the on-disk key differs from the member.
#define AE_FIELD_NR(name, member, TypeTag, lo, hi) \
	b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.min = (lo), .max = (hi)});

#define AE_FIELD_ANGLE(member) \
	b.Field(#member, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true});

// TOML key `tomlKey` - reconciles a nice editor unit with an existing on-disk format.
#define AE_FIELD_ANGLE_AS(name, member, tomlKey) \
	b.Field(name, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true, .serializeName = (tomlKey)});

#define AE_FIELD_CUSTOM(name, TypeTag, getLambda, setLambda) \
	b.CustomField(name, ::aether::reflect::FieldType::TypeTag, getLambda, setLambda);

// Replicated counterpart of AE_FIELD_CUSTOM: identical, plus the network schema picks it up.
#define AE_FIELD_CUSTOM_REP(name, TypeTag, getLambda, setLambda) \
	b.CustomField(name, ::aether::reflect::FieldType::TypeTag, getLambda, setLambda, ::aether::reflect::FieldMeta{.replicated = true});

// so the preprocessor never sees their commas).
#define AE_FIELD_ENUM(name, member, tableRef) b.EnumField(name, &C::member, tableRef);

#define AE_NOT_ADDABLE() b.Addable(false);

// Marks a pure data-only component for generic scene serialization (see
// ComponentType::genericSerialize). No per-component capture/apply/codec needed.
#define AE_GENERIC_SERIALIZE() b.GenericSerialize(true);

// Marks a component that the editor's ComponentCatalog adds a hand-authored palette
// entry for, so the catalog does not auto-generate a plain reflected one.
#define AE_HAND_AUTHORED_CATALOG() b.HandAuthoredCatalogEntry(true);

#define AE_COMPONENT_END()                                                           \
			::aether::reflect::RegisterComponent(std::move(b).Build());              \
			return true;                                                             \
		}();                                                                         \
	}
