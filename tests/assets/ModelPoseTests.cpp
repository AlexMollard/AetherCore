#include <algorithm>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include "assets/GltfAsset.hpp"
#include "assets/ModelPose.hpp"
#include "scene/Components.hpp"

using namespace aether::assets;

namespace native_sampling
{
	using uint = std::uint32_t;
	using float4 = glm::vec4;
	using std::abs;
	using std::max;

	// Only fields read by the pure shader sampler are needed. The offsets remain
	// byte offsets into the packed arrays, exactly as AnimationDatabase writes them.
	struct GpuChannel
	{
		uint interpolation = 0;
		uint timesOffset = 0;
		uint timesCount = 0;
		uint valuesOffset = 0;
	};

	float4 lerp(float4 start, float4 end, float alpha)
	{
		return glm::mix(start, end, alpha);
	}

#include "../../src/shaders/animation_sample.slang"
} // namespace native_sampling

namespace
{
	GltfAsset AnimatedTranslation(GltfInterpolation interpolation = GltfInterpolation::Linear)
	{
		GltfAsset asset;
		asset.nodes.resize(1);
		GltfAnimationChannel channel;
		channel.interpolation = interpolation;
		channel.times = {0.0f, 1.0f, 2.0f};
		channel.values = {{0.0f, 0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f, 0.0f}};
		asset.animations.push_back({.channels = {channel}});
		return asset;
	}

	float PositionX(const GltfAsset& asset, float time, bool looping = false)
	{
		const auto pose = EvaluateModelPose(asset, 0u, time, looping);
		REQUIRE(pose.has_value());
		return pose->nodeWorld[0][3].x;
	}
} // namespace

TEST_CASE("model pose composes matrix and TRS nodes in parent order")
{
	GltfAsset asset;
	asset.nodes.resize(3);
	asset.nodes[0].parentIndex = 2; // Parent need not precede child in the asset.
	asset.nodes[0].translation = {1.0f, 0.0f, 0.0f};
	asset.nodes[1].parentIndex = 0;
	asset.nodes[1].translation = {0.0f, 3.0f, 0.0f};
	asset.nodes[2].hasMatrix = true;
	asset.nodes[2].matrix = glm::scale(glm::translate(glm::mat4{1.0f}, {10.0f, 0.0f, 0.0f}), {2.0f, 2.0f, 2.0f});
	const auto pose = EvaluateModelPose(asset);
	REQUIRE(pose.has_value());
	CHECK(pose->nodeWorld[0][3].x == doctest::Approx(12.0f));
	CHECK(pose->nodeWorld[1][3].y == doctest::Approx(6.0f));
	CHECK(pose->nodeWorld[2][0].x == doctest::Approx(2.0f));
}

TEST_CASE("model pose builds all global-joint inverse-bind skin palettes")
{
	GltfAsset asset;
	asset.nodes.resize(2);
	asset.nodes[0].translation = {2.0f, 0.0f, 0.0f};
	asset.nodes[1].parentIndex = 0;
	asset.nodes[1].translation = {0.0f, 3.0f, 0.0f};
	asset.skins.push_back({.joints = {1}, .inverseBindMatrices = {glm::translate(glm::mat4{1.0f}, {0.0f, -1.0f, 0.0f})}});
	asset.skins.push_back({.joints = {0}}); // Absent inverse binds are identity.
	const auto pose = EvaluateModelPose(asset);
	REQUIRE(pose.has_value());
	REQUIRE(pose->skins.size() == 2);
	const glm::vec4 position = pose->skins[0][0] * glm::vec4{1.0f, 0.0f, 0.0f, 1.0f};
	CHECK(position.x == doctest::Approx(3.0f));
	CHECK(position.y == doctest::Approx(2.0f));
	CHECK(pose->skins[1][0][3].x == doctest::Approx(2.0f));
}

TEST_CASE("joint offsets apply after animation and propagate to child skin matrices")
{
	auto asset = AnimatedTranslation();
	asset.nodes.resize(2);
	asset.nodes[1].parentIndex = 0;
	asset.nodes[1].translation = {1.0f, 0.0f, 0.0f};
	asset.skins.push_back({.joints = {1}});
	const std::vector<aether::JointRotationOffset> offsets{{.node = 0, .rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 0.0f, 1.0f})}};
	const auto pose = EvaluateModelPose(asset, 0u, 0.5f, false, nullptr, offsets);
	REQUIRE(pose.has_value());
	CHECK(pose->nodeWorld[1][3].x == doctest::Approx(1.0f));
	CHECK(pose->nodeWorld[1][3].y == doctest::Approx(1.0f));
	CHECK(pose->skins[0][0][3].y == doctest::Approx(1.0f));
}

TEST_CASE("model pose translation loops and clamps across clip bounds")
{
	const auto asset = AnimatedTranslation();
	CHECK(PositionX(asset, 0.5f) == doctest::Approx(1.0f));
	CHECK(PositionX(asset, 2.5f) == doctest::Approx(4.0f));
	CHECK(PositionX(asset, -0.5f) == doctest::Approx(0.0f));
	CHECK(PositionX(asset, 2.5f, true) == doctest::Approx(1.0f));
	CHECK(PositionX(asset, -0.5f, true) == doctest::Approx(3.0f));
	CHECK(PositionX(asset, 2.0f, true) == doctest::Approx(0.0f));
	CHECK(EvaluateModelPose(asset)->nodeWorld[0][3].x == doctest::Approx(0.0f));
}

TEST_CASE("model pose step selects the preceding key including exact key boundaries")
{
	const auto asset = AnimatedTranslation(GltfInterpolation::Step);
	CHECK(PositionX(asset, 0.9f) == doctest::Approx(0.0f));
	CHECK(PositionX(asset, 1.0f) == doctest::Approx(2.0f));
	CHECK(PositionX(asset, 1.9f) == doctest::Approx(2.0f));
	CHECK(PositionX(asset, 2.0f) == doctest::Approx(4.0f));
}

TEST_CASE("model pose rotation uses normalized shortest-path linear interpolation")
{
	auto asset = AnimatedTranslation();
	auto& channel = asset.animations[0].channels[0];
	channel.path = GltfAnimationPath::Rotation;
	channel.times = {0.0f, 1.0f};
	const float halfRoot = std::sqrt(0.5f);
	channel.values = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -halfRoot, -halfRoot}};
	const auto pose = EvaluateModelPose(asset, 0u, 0.5f, false);
	REQUIRE(pose.has_value());
	const glm::vec4 rotated = pose->nodeWorld[0] * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f};
	CHECK(rotated.x == doctest::Approx(halfRoot));
	CHECK(rotated.y == doctest::Approx(halfRoot));
	CHECK(glm::length(glm::vec3(rotated)) == doctest::Approx(1.0f));
}

TEST_CASE("model pose cubic spline uses key tangent triplets scaled by key interval")
{
	auto asset = AnimatedTranslation(GltfInterpolation::CubicSpline);
	auto& channel = asset.animations[0].channels[0];
	channel.times = {0.0f, 2.0f};
	channel.values = {glm::vec4{0.0f}, glm::vec4{0.0f}, {4.0f, 0.0f, 0.0f, 0.0f}, glm::vec4{0.0f}, {2.0f, 0.0f, 0.0f, 0.0f}, glm::vec4{0.0f}};
	CHECK(PositionX(asset, 0.0f) == doctest::Approx(0.0f));
	CHECK(PositionX(asset, 1.0f) == doctest::Approx(2.0f));
	CHECK(PositionX(asset, 2.0f) == doctest::Approx(2.0f));
	channel.path = GltfAnimationPath::Rotation;
	channel.values = {glm::vec4{0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, glm::vec4{0.0f}, glm::vec4{0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, glm::vec4{0.0f}};
	const auto pose = EvaluateModelPose(asset, 0u, 1.0f, false);
	REQUIRE(pose.has_value());
	const glm::vec4 rotated = pose->nodeWorld[0] * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f};
	CHECK(rotated.x == doctest::Approx(0.0f).epsilon(0.00001f));
	CHECK(rotated.y == doctest::Approx(1.0f));
}

TEST_CASE("model pose root lock preserves translation on children of node zero")
{
	auto asset = AnimatedTranslation();
	asset.nodes.resize(2);
	asset.nodes[1].parentIndex = 0;
	asset.nodes[1].translation = {7.0f, 0.0f, 0.0f};
	asset.animations[0].channels[0].nodeIndex = 1;
	asset.animations[0].rootLocked = true;
	const auto pose = EvaluateModelPose(asset, 0u, 0.5f, false);
	REQUIRE(pose.has_value());
	CHECK(pose->nodeWorld[1][3].x == doctest::Approx(7.0f));
}

TEST_CASE("model pose rejects malformed hierarchy skin and animation data")
{
	auto asset = AnimatedTranslation();
	SUBCASE("cycle")
	{
		asset.nodes[0].parentIndex = 0;
	}
	SUBCASE("invalid parent")
	{
		asset.nodes[0].parentIndex = 1;
	}
	SUBCASE("invalid negative parent")
	{
		asset.nodes[0].parentIndex = -2;
	}
	SUBCASE("invalid child")
	{
		asset.nodes[0].children = {1};
	}
	SUBCASE("invalid joint")
	{
		asset.skins.push_back({.joints = {1}});
	}
	SUBCASE("mismatched inverse binds")
	{
		asset.skins.push_back({.joints = {0, 0}, .inverseBindMatrices = {glm::mat4{1.0f}}});
	}
	SUBCASE("invalid channel target")
	{
		asset.animations[0].channels[0].nodeIndex = 1;
	}
	SUBCASE("matrix channel target")
	{
		asset.nodes[0].hasMatrix = true;
	}
	SUBCASE("missing keys")
	{
		asset.animations[0].channels[0].times.clear();
	}
	SUBCASE("missing values")
	{
		asset.animations[0].channels[0].values.pop_back();
	}
	SUBCASE("missing cubic triplets")
	{
		asset.animations[0].channels[0].interpolation = GltfInterpolation::CubicSpline;
	}
	SUBCASE("repeated keys")
	{
		asset.animations[0].channels[0].times[1] = 0.0f;
	}
	SUBCASE("non-finite keys")
	{
		asset.animations[0].channels[0].times[1] = std::numeric_limits<float>::infinity();
	}
	SUBCASE("non-finite values")
	{
		asset.animations[0].channels[0].values[0].x = std::numeric_limits<float>::quiet_NaN();
	}
	SUBCASE("unsupported weights")
	{
		asset.animations[0].channels[0].path = GltfAnimationPath::Weights;
	}
	CHECK_FALSE(EvaluateModelPose(asset, 0u).has_value());
}

TEST_CASE("model pose rejects invalid clip indices and non-finite evaluation time")
{
	const auto asset = AnimatedTranslation();
	CHECK_FALSE(EvaluateModelPose(asset, 1u).has_value());
	CHECK_FALSE(EvaluateModelPose(asset, 0u, std::numeric_limits<float>::infinity()).has_value());
	CHECK_FALSE(EvaluateModelPose(asset, {}, std::numeric_limits<float>::quiet_NaN()).has_value());
}

TEST_CASE("actual native shader sampling matches portable model pose for every TRS interpolation")
{
	for (const auto interpolation: {GltfInterpolation::Step, GltfInterpolation::Linear, GltfInterpolation::CubicSpline})
	{
		for (const auto path: {GltfAnimationPath::Translation, GltfAnimationPath::Scale, GltfAnimationPath::Rotation})
		{
			GltfAsset asset;
			asset.nodes.resize(1);
			GltfAnimationChannel channel;
			channel.path = path;
			channel.interpolation = interpolation;
			channel.times = {0.25f, 0.75f, 2.25f};
			const std::vector<glm::vec4> keyValues = path == GltfAnimationPath::Rotation ? std::vector<glm::vec4>{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -0.70710678f, -0.70710678f}, {0.0f, 0.0f, 1.0f, 0.0f}}
			                                                                             : std::vector<glm::vec4>{{1.0f, 2.0f, 3.0f, 0.0f}, {3.0f, -2.0f, 1.0f, 0.0f}, {2.0f, 3.0f, 4.0f, 0.0f}};
			for (const auto& value: keyValues)
			{
				if (interpolation == GltfInterpolation::CubicSpline)
				{
					channel.values.push_back({0.2f, -0.3f, 0.5f, 0.0f});
				}
				channel.values.push_back(value);
				if (interpolation == GltfInterpolation::CubicSpline)
				{
					channel.values.push_back({-0.5f, 0.6f, 0.1f, 0.0f});
				}
			}
			asset.animations.push_back({.channels = {channel}});
			// Nonzero prefixes catch accidental treatment of byte offsets as indices.
			std::vector<float> packedTimes{99.0f, 99.0f};
			packedTimes.insert(packedTimes.end(), channel.times.begin(), channel.times.end());
			std::vector<glm::vec4> packedValues{glm::vec4{99.0f}};
			packedValues.insert(packedValues.end(), channel.values.begin(), channel.values.end());
			native_sampling::GpuChannel gpuChannel;
			gpuChannel.interpolation = static_cast<std::uint32_t>(interpolation);
			gpuChannel.timesOffset = 2u * sizeof(float);
			gpuChannel.timesCount = static_cast<std::uint32_t>(channel.times.size());
			gpuChannel.valuesOffset = sizeof(glm::vec4);
			for (const float time: {-1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.5f, 2.25f, 3.0f})
			{
				INFO("interpolation=" << static_cast<int>(interpolation) << " path=" << static_cast<int>(path) << " time=" << time);
				const auto pose = EvaluateModelPose(asset, 0u, time, false);
				REQUIRE(pose.has_value());
				glm::mat4 nativeMatrix{1.0f};
				if (path == GltfAnimationPath::Rotation)
				{
					const auto sample = native_sampling::SampleRotation(gpuChannel, time, packedTimes.data(), packedValues.data());
					nativeMatrix = glm::mat4_cast(glm::quat{sample.w, sample.x, sample.y, sample.z});
				}
				else
				{
					const auto sample = native_sampling::SampleChannelValue(gpuChannel, time, packedTimes.data(), packedValues.data());
					nativeMatrix = path == GltfAnimationPath::Translation ? glm::translate(nativeMatrix, glm::vec3(sample)) : glm::scale(nativeMatrix, glm::vec3(sample));
				}
				for (int column = 0; column < 4; ++column)
				{
					for (int row = 0; row < 4; ++row)
					{
						CHECK(pose->nodeWorld[0][column][row] == doctest::Approx(nativeMatrix[column][row]).epsilon(0.00001f));
					}
				}
			}
		}
	}
}

TEST_CASE("actual native shader handles one-key cubic triplets and sub-microsecond key intervals")
{
	auto asset = AnimatedTranslation(GltfInterpolation::CubicSpline);
	auto& channel = asset.animations[0].channels[0];
	channel.times = {0.0f};
	channel.values = {{10.0f, 0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f, 0.0f}};
	native_sampling::GpuChannel gpuChannel;
	gpuChannel.interpolation = static_cast<std::uint32_t>(channel.interpolation);
	gpuChannel.timesCount = 1;
	CHECK(native_sampling::SampleChannelValue(gpuChannel, 0.0f, channel.times.data(), channel.values.data()).x == doctest::Approx(PositionX(asset, 0.0f)));

	channel.interpolation = GltfInterpolation::Linear;
	channel.times = {0.0f, 0.0000005f};
	channel.values = {glm::vec4{0.0f}, {4.0f, 0.0f, 0.0f, 0.0f}};
	gpuChannel.interpolation = static_cast<std::uint32_t>(channel.interpolation);
	gpuChannel.timesCount = 2;
	CHECK(native_sampling::SampleChannelValue(gpuChannel, 0.00000025f, channel.times.data(), channel.values.data()).x == doctest::Approx(2.0f));
	CHECK(PositionX(asset, 0.00000025f) == doctest::Approx(2.0f));
}
