#pragma once

#include <cstdint>
#include <functional>
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
		std::vector<FieldDesc> fields;

		std::function<bool(const World&, Entity)> has;
		std::function<void*(World&, Entity)> emplaceDefault;
		std::function<void(World&, Entity)> remove;
		std::function<void*(World&, Entity)> tryGetRaw;
		std::function<const void*(const World&, Entity)> tryGetRawConst;

		bool addable = true; // false = reference-only, never added to an arbitrary entity
		bool serializable = true;

		std::function<void(World&, Entity)> postSet;
		CustomSerializeFns customSerialize;

		[[nodiscard]] const FieldDesc* FindField(std::string_view fieldName) const;
	};

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

#define AE_FIELD_R(member, TypeTag, lo, hi) \
	b.Field(#member, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.min = (lo), .max = (hi)});

#define AE_FIELD_ANGLE(member) \
	b.Field(#member, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true});

// TOML key `tomlKey` - reconciles a nice editor unit with an existing on-disk format.
#define AE_FIELD_ANGLE_AS(name, member, tomlKey) \
	b.Field(name, ::aether::reflect::FieldType::Float, &C::member, ::aether::reflect::FieldMeta{.min = 1.0f, .max = 89.0f, .isAngleDegrees = true, .serializeName = (tomlKey)});

#define AE_FIELD_CUSTOM(name, TypeTag, getLambda, setLambda) \
	b.CustomField(name, ::aether::reflect::FieldType::TypeTag, getLambda, setLambda);

// so the preprocessor never sees their commas).
#define AE_FIELD_ENUM(name, member, tableRef) b.EnumField(name, &C::member, tableRef);

#define AE_NOT_ADDABLE() b.Addable(false);

#define AE_COMPONENT_END()                                                           \
			::aether::reflect::RegisterComponent(std::move(b).Build());              \
			return true;                                                             \
		}();                                                                         \
	}
