#pragma once

#include <map>
#include <ostream>
#include <string>
#include <string_view>

namespace aether
{
	class TomlConfig
	{
	public:
		void Load(std::string_view tomlText);
		void Save(std::ostream& out, std::string_view headerComment = {}) const;

		[[nodiscard]] bool LoadFile(std::string_view fileName);
		bool SaveFile(std::string_view fileName, std::string_view headerComment = {}) const;
		bool SaveIfDirty(std::string_view fileName, std::string_view headerComment = {});

		bool GetBool(std::string_view key, bool defaultValue) const;
		float GetFloat(std::string_view key, float defaultValue) const;

		void Set(std::string_view key, bool value);
		void Set(std::string_view key, float value);

		bool IsDirty() const
		{
			return m_dirty;
		}

		void MarkClean()
		{
			m_dirty = false;
		}

		bool Has(std::string_view key) const;
		void Clear();

	private:
		std::map<std::string, std::string, std::less<>> m_values;
		bool m_dirty = false;
	};
} // namespace aether
