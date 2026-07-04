#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
	};

	inline constexpr std::size_t kTonemapCount = kTonemapDefs.size();

} // namespace aether
