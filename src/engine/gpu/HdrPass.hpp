#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string>

namespace aether::gpu
{
	// Plain packet snapshot: identical vec4-only uniform layout on CPU and WGSL.
	struct alignas(16) HdrUniforms
	{
		std::array<std::array<float, 4>, 4> view{};
		std::array<float, 4> settings{}, extentAngleTime{}, projection{};
		std::array<float, 4> sunDirectionIntensity{}, sunColor{}, horizon{}, zenith{}, voidColor{}, skyParams{}, shadingParams{}, camera{};
		std::array<std::array<float, 4>, 8> stops{};
	};

	struct HdrPlan
	{
		HdrUniforms constants;
		std::uint32_t width, height;
	};

	[[nodiscard]] inline std::expected<HdrPlan, std::string> PlanHdrPass(HdrUniforms constants, std::uint32_t width, std::uint32_t height)
	{
		if (!width || !height || width > 4096 || height > 4096)
		{
			return std::unexpected("WEB_HDR_EXTENT: HDR extent must be within 1..4096");
		}
		const auto finite = [](const auto& vector)
		{
			for (float value: vector)
			{
				if (!std::isfinite(value))
				{
					return false;
				}
			}
			return true;
		};
		for (const auto& row: constants.view)
		{
			if (!finite(row))
			{
				return std::unexpected("WEB_HDR_NONFINITE: invalid view matrix");
			}
		}
		for (const auto* vector: {&constants.settings,
		             &constants.extentAngleTime,
		             &constants.projection,
		             &constants.sunDirectionIntensity,
		             &constants.sunColor,
		             &constants.horizon,
		             &constants.zenith,
		             &constants.voidColor,
		             &constants.skyParams,
		             &constants.shadingParams,
		             &constants.camera})
		{
			if (!finite(*vector))
			{
				return std::unexpected("WEB_HDR_NONFINITE: invalid compositor settings");
			}
		}
		const auto& settings = constants.settings;
		if (settings[0] < 0 || settings[0] > 10 || settings[0] != std::floor(settings[0]) || settings[1] < 0 || settings[2] < 0 || settings[2] > 2 || settings[2] != std::floor(settings[2]) || settings[3] < 0 || settings[3] > 8
		        || settings[3] != std::floor(settings[3]))
		{
			return std::unexpected("WEB_HDR_SETTINGS: invalid tonemap or background settings");
		}
		if (settings[2] != 2 && settings[3] < 1)
		{
			return std::unexpected("WEB_HDR_BACKGROUND: camera background requires a color stop");
		}
		for (std::size_t index = 0; index < static_cast<std::size_t>(settings[3]); ++index)
		{
			const auto& stop = constants.stops[index];
			if (!finite(stop) || stop[3] < 0 || stop[3] > 1 || (index && stop[3] < constants.stops[index - 1][3]))
			{
				return std::unexpected("WEB_HDR_BACKGROUND: gradient positions must be finite, ordered and within 0..1");
			}
		}
		if (settings[2] == 2
		        && (std::abs(constants.projection[0]) < 1e-6f || std::abs(constants.projection[1]) < 1e-6f
		                || constants.sunDirectionIntensity[0] * constants.sunDirectionIntensity[0] + constants.sunDirectionIntensity[1] * constants.sunDirectionIntensity[1] + constants.sunDirectionIntensity[2] * constants.sunDirectionIntensity[2]
		                           < 1e-12f))
		{
			return std::unexpected("WEB_HDR_SKY: degenerate sky projection or sun direction");
		}
		if (settings[2] == 2)
		{
			const auto& v = constants.view;
			const double determinant =
			        double(v[0][0]) * (double(v[1][1]) * v[2][2] - double(v[1][2]) * v[2][1]) - double(v[1][0]) * (double(v[0][1]) * v[2][2] - double(v[0][2]) * v[2][1]) + double(v[2][0]) * (double(v[0][1]) * v[1][2] - double(v[0][2]) * v[1][1]);
			if (std::abs(determinant) < 1e-8 || !std::isfinite(determinant))
			{
				return std::unexpected("WEB_HDR_SKY: singular camera rotation");
			}
		}
		constants.extentAngleTime[0] = static_cast<float>(width);
		constants.extentAngleTime[1] = static_cast<float>(height);
		return HdrPlan{constants, width, height};
	}
} // namespace aether::gpu
