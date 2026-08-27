#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aether
{

	enum class TonemapMode : std::uint32_t
	{
		Reinhard = 0,
		AcesFilmic = 1,
		Uncharted2 = 2,
		HejlRichard = 3,
		Linear = 4,
		Exponential = 5,
		FilmicDice = 6,
		Lottes = 7,
		RomBinDaHouse = 8,
		Vanilla = 9,
		Agx = 10,
	};

	struct TonemapDef
	{
		TonemapMode mode;
		const char* name;
	};

	inline constexpr std::array kTonemapDefs = {
	        TonemapDef{TonemapMode::Reinhard, "Reinhard"},
	        TonemapDef{TonemapMode::AcesFilmic, "ACES Filmic"},
	        TonemapDef{TonemapMode::Uncharted2, "Uncharted 2"},
	        TonemapDef{TonemapMode::HejlRichard, "Hejl Richard"},
	        TonemapDef{TonemapMode::Linear, "Linear"},
	        TonemapDef{TonemapMode::Exponential, "Exponential"},
	        TonemapDef{TonemapMode::FilmicDice, "Filmic Dice"},
	        TonemapDef{TonemapMode::Lottes, "Lottes"},
	        TonemapDef{TonemapMode::RomBinDaHouse, "RomBinDaHouse"},
	        TonemapDef{TonemapMode::Vanilla, "Vanilla"},
	        TonemapDef{TonemapMode::Agx, "AgX"},
	};

	inline constexpr std::size_t kTonemapCount = kTonemapDefs.size();

	// The operator names as a plain list, so a settings field can offer them as choices
	// without restating them - the table above stays the only place an operator is named.
	inline constexpr auto kTonemapNames = []
	{
		std::array<std::string_view, kTonemapCount> names{};
		for (std::size_t i = 0; i < kTonemapCount; ++i)
		{
			names[i] = kTonemapDefs[i].name;
		}
		return names;
	}();

	// Resolve a stored name back to an operator. Case-insensitive because this reads a
	// hand-editable config file, and settling on a fallback rather than failing because an
	// unknown name should cost the project its chosen look, not its picture.
	[[nodiscard]] inline TonemapMode TonemapModeFromName(std::string_view name)
	{
		const auto lower = [](char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		};
		for (const TonemapDef& def: kTonemapDefs)
		{
			const std::string_view candidate{def.name};
			if (candidate.size() != name.size())
			{
				continue;
			}
			bool same = true;
			for (std::size_t i = 0; i < name.size(); ++i)
			{
				if (lower(name[i]) != lower(candidate[i]))
				{
					same = false;
					break;
				}
			}
			if (same)
			{
				return def.mode;
			}
		}
		return TonemapMode::AcesFilmic;
	}

} // namespace aether
