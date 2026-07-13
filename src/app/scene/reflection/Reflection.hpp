#pragma once

// Component reflection core.
//
// One AE_COMPONENT declaration per component type describes its authored fields;
// every per-component surface (MCP get/set, ComponentCatalog, SceneSerializer,
// inspector) reads the resulting ComponentType table instead of hand-wiring each
// component four times. Fields convert to a NEUTRAL FieldValue - not json / TOML /
// ImGui - so this core carries no serialization or UI dependency and compiles into
// GameRuntime (the scene loader needs it). Each consumer adapts FieldValue to its
// own format.
//
// The data model (FieldValue / FieldDesc / ComponentType) is deliberately separate
// from the macro DSL that populates it: when C++26 static reflection is available
// on all compilers, only the declaration layer changes - consumers are untouched.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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
		Color3, // glm::vec3 shown as an RGB colour
		Color4, // glm::vec4 shown as an RGBA colour
		Enum,
		String,
		EntityRef,
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
		float max = 0.0f;            // min==max==0 => unbounded
		float speed = 0.0f;          // 0 => a sensible default per type
		bool isAngleDegrees = false; // member stored in radians, exposed/edited in degrees
		bool serialize = true;       // editable-but-not-persisted fields set false
		const EnumTable* enumTable = nullptr;
		std::string tooltip;
		// TOML key to serialize under, when it differs from the field name (e.g. an
		// angle field exposed as "inner_angle_deg" that persists as "inner_rad").
		// Empty => the field name is the key. Angle fields (isAngleDegrees) always
		// serialize in radians regardless.
		std::string serializeName;
	};

	// A neutral field value. Consumers read the member matching `type`.
	struct FieldValue
	{
		FieldType type = FieldType::Float;
		double num = 0.0;         // Float / Int / UInt
		bool boolean = false;     // Bool
		glm::vec4 vec{0.0f};      // Vec2 / Vec3 / Vec4 / Color3 / Color4
		int enumValue = 0;        // Enum
		std::uint64_t entity = 0; // EntityRef
		std::string str;          // String
	};

	struct FieldDesc
	{
		std::string name;
		FieldType type = FieldType::Float;
		FieldMeta meta;
		std::function<FieldValue(const void* component)> get;
		std::function<void(void* component, const FieldValue&)> set;
	};

	// Bespoke TOML for the components whose on-disk shape isn't a flat field list
	// (Mesh source, Material textures). Null members => use the generic field loop.
	struct CustomSerializeFns
	{
		// The types are opaque here (toml++ is an app/scene dep, not a core one); the
		// serializer casts. Kept as void* to keep this header toml-free.
		std::function<void(const void* component, void* tomlTable)> write;
		std::function<void(void* component, const void* tomlTable)> read;
	};

	struct ComponentType
	{
		std::string name;     // "Point Light" - matches catalog + MCP type
		std::string category; // "Rendering", "Physics", "Behaviors", ...
		std::string icon;     // ICON_FA_* string (no ImGui dependency)
		std::vector<FieldDesc> fields;

		std::function<bool(const World&, Entity)> has;
		std::function<void*(World&, Entity)> emplaceDefault;
		std::function<void(World&, Entity)> remove;
		std::function<void*(World&, Entity)> tryGetRaw;
		std::function<const void*(const World&, Entity)> tryGetRawConst;

		bool addable = true;      // false = reference-only, never added to an arbitrary entity
		bool serializable = true; // false = runtime/editor-only component (not persisted)

		std::function<void(World&, Entity)> postSet; // e.g. Material -> AssignMaterial after a field write
		CustomSerializeFns customSerialize;

		// Convenience: current value of a field by name (null get => component absent).
		[[nodiscard]] const FieldDesc* FindField(std::string_view fieldName) const;
	};

	// The registry. Populated by AE_COMPONENT declarations at static-init.
	const std::vector<ComponentType>& ComponentTypes();
	const ComponentType* FindComponentType(std::string_view name);
	void RegisterComponent(ComponentType type); // used by the DSL; safe to call directly

	// ── FieldValue <-> C++ conversions (member-type driven) ─────────────────────
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
		out = static_cast<int>(f.num);
	}

	inline void ApplyValue(const FieldValue& f, std::uint32_t& out)
	{
		out = static_cast<std::uint32_t>(f.num < 0.0 ? 0.0 : f.num);
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

	// ── Builder used by the DSL ─────────────────────────────────────────────────
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

		// A plain member field. `Tag` is the semantic FieldType (Color3 vs Vec3 etc.);
		// the actual member type drives the value conversion.
		template<typename C, typename M>
		ComponentBuilder& Field(const char* name, FieldType tag, M C::* member, FieldMeta meta = {})
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

		// A computed field (e.g. Transform position/euler/scale over a matrix). The
		// caller supplies value get/set directly against the component pointer.
		ComponentBuilder& CustomField(const char* name, FieldType tag, std::function<FieldValue(const void*)> get, std::function<void(void*, const FieldValue&)> set, FieldMeta meta = {})
		{
			FieldDesc f;
			f.name = name;
			f.type = tag;
			f.meta = meta;
			f.get = std::move(get);
			f.set = std::move(set);
			m_type.fields.push_back(std::move(f));
			return *this;
		}

		// An enum member. `table` maps names <-> values (define it as a C++ static in
		// the declaration file - see AE_FIELD_ENUM). Must outlive the registry (statics do).
		template<typename C, typename E>
		ComponentBuilder& EnumField(const char* name, E C::* member, const EnumTable& table)
		{
			FieldDesc f;
			f.name = name;
			f.type = FieldType::Enum;
			f.meta.enumTable = &table;
			f.get = [member, &table](const void* comp) -> FieldValue
			{
				FieldValue v;
				v.type = FieldType::Enum;
				v.enumValue = static_cast<int>(static_cast<const C*>(comp)->*member);
				return v;
			};
			f.set = [member](void* comp, const FieldValue& in)
			{
				static_cast<C*>(comp)->*member = static_cast<E>(in.enumValue);
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

		ComponentBuilder& PostSet(std::function<void(World&, Entity)> fn)
		{
			m_type.postSet = std::move(fn);
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

// ── Declaration DSL ─────────────────────────────────────────────────────────
// AE_COMPONENT(CppType, "Display Name", "Category", ICON_FA_*)
//     AE_FIELD(member, TypeTag)
//     AE_FIELD_R(member, TypeTag, lo, hi)   // ranged
//     AE_FIELD_ANGLE(member)                // float radians <-> exposed degrees
//     AE_FIELD_CUSTOM("name", TypeTag, getLambda, setLambda)
// AE_COMPONENT_END()
#define AE_COMPONENT(CppType, DisplayName, Category, Icon)                            \
	namespace aether::reflect::detail                                                \
	{                                                                                \
		static const bool CppType##_registered = []                                  \
		{                                                                            \
			using C = CppType;                                                       \
			::aether::reflect::ComponentBuilder b{DisplayName, Category, Icon};       \
			b.SetEcsOps<C>();

#define AE_FIELD(member, TypeTag) b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member);

// Same as AE_FIELD but with an explicit field name (used to match existing scene
// TOML keys, e.g. member castsShadow -> key "shadow").
#define AE_FIELD_N(name, member, TypeTag) b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member);

#define AE_FIELD_R(member, TypeTag, lo, hi) \
	b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.min = (lo), .max = (hi)});

#define AE_FIELD_ANGLE(member) \
	b.Field(#member, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true});

// Angle field exposed in degrees under `name` but persisted (in radians) under the
// TOML key `tomlKey` - reconciles a nice editor unit with an existing on-disk format.
#define AE_FIELD_ANGLE_AS(name, member, tomlKey) \
	b.Field(name, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true, .serializeName = tomlKey});

#define AE_FIELD_CUSTOM(name, TypeTag, getLambda, setLambda) \
	b.CustomField(name, ::aether::reflect::FieldType::TypeTag, getLambda, setLambda);

// An enum field. `tableRef` is a reference to a reflect::EnumTable defined as a
// static in the declaration file (its name<->value pairs written as normal C++
// so the preprocessor never sees their commas).
#define AE_FIELD_ENUM(name, member, tableRef) b.EnumField(name, &C::member, tableRef);

// Marks the component as not addable from the Add-Component palette (it comes with
// an entity or model - e.g. Skinned Mesh, Orbit Camera - and would be broken or
// redundant if slapped onto an arbitrary entity). It stays fully get/set-able.
#define AE_NOT_ADDABLE() b.Addable(false);

#define AE_COMPONENT_END()                                                           \
			::aether::reflect::RegisterComponent(std::move(b).Build());              \
			return true;                                                             \
		}();                                                                         \
	}
