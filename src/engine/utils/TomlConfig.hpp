#pragma once

#include <filesystem>
#include <map>
#include <ostream>
#include <string>
#include <string_view>

namespace aether
{
	class TomlConfig
	{
	public:
		// False when the text does not parse, leaving the config EMPTY. Anything that loads,
		// edits and writes the same file must refuse to save on false: saving would replace
		// the file with only the keys it explicitly Set, destroying every other section.
		[[nodiscard]] bool Load(std::string_view tomlText);
		void Save(std::ostream& out, std::string_view headerComment = {}) const;

		[[nodiscard]] bool LoadFile(std::string_view fileName);
		[[nodiscard]] bool SaveFile(std::string_view fileName, std::string_view headerComment = {}) const;
		bool SaveIfDirty(std::string_view fileName, std::string_view headerComment = {});

		[[nodiscard]] bool LoadFromPath(const std::filesystem::path& path);
		[[nodiscard]] bool SaveToPath(const std::filesystem::path& path, std::string_view headerComment = {}) const;

		[[nodiscard]] bool GetBool(std::string_view key, bool defaultValue) const;
		[[nodiscard]] float GetFloat(std::string_view key, float defaultValue) const;
		[[nodiscard]] std::string GetString(std::string_view key, std::string_view defaultValue = {}) const;

		void Set(std::string_view key, bool value);
		void Set(std::string_view key, float value);
		void Set(std::string_view key, std::string_view value);

		// Without this, Set(key, "some string") silently writes `true`: a const char*
		// converts to bool by a standard conversion, which beats the user-defined
		// conversion to string_view during overload resolution. Every string literal
		// passed to Set would land in the bool overload and destroy the value.
		void Set(std::string_view key, const char* value)
		{
			Set(key, std::string_view(value != nullptr ? value : ""));
		}

		[[nodiscard]] bool IsDirty() const
		{
			return m_dirty;
		}

		void MarkClean()
		{
			m_dirty = false;
		}

		[[nodiscard]] bool Has(std::string_view key) const;
		void Clear();

	private:
		std::map<std::string, std::string, std::less<>> m_values;
		bool m_dirty = false;
	};
} // namespace aether
