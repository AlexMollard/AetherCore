#include <doctest/doctest.h>
#include <ShadowCascadeFit.hpp>
#include <LocalShadowPlan.hpp>

TEST_CASE("Shadow cascade spheres enclose perspective and orthographic slices")
{
	for (const bool orthographic: {false, true})
	{
		for (const float nearZ: {0.1f, 10.0f, 80.0f})
		{
			const float farZ = nearZ + 60;
			const auto sphere = aether::FitShadowCascadeSphere(nearZ, farZ, 1.7f, 0.8f, orthographic);
			for (const float z: {nearZ, farZ})
			{
				const float scale = orthographic ? 1 : z;
				const float distance2 = (1.7f * 1.7f + 0.8f * 0.8f) * scale * scale + (z - sphere.centerDistance) * (z - sphere.centerDistance);
				CHECK(distance2 <= sphere.radius * sphere.radius + 0.01f);
			}
		}
	}
}

TEST_CASE("Orthographic cascade fit retains constant width across depth")
{
	const auto nearSlice = aether::FitShadowCascadeSphere(0.1f, 10.1f, 4, 3, true);
	const auto farSlice = aether::FitShadowCascadeSphere(100, 110, 4, 3, true);
	CHECK(nearSlice.radius == doctest::Approx(farSlice.radius));
	CHECK(farSlice.centerDistance == doctest::Approx(105));
}

TEST_CASE("Local shadow selection follows distance and spends native entry budget")
{
	struct Light
	{
		glm::vec3 position;
		float radius = 20, sourceRadius = 0;
		bool castsShadow = true;
	};
	std::vector<Light> points, spots;
	for (int i = 1; i <= 7; ++i) { points.push_back({glm::vec3(i, 0, 0)}); }
	points.push_back({glm::vec3(12, 0, 0)});
	for (int i = 8; i <= 11; ++i) { spots.push_back({glm::vec3(i, 0, 0)}); }
	spots.push_back({glm::vec3(13, 0, 0)});
	spots.push_back({glm::vec3(14, 0, 0)});
	spots.push_back({glm::vec3(15, 0, 0)});
	const auto candidates = aether::LocalShadowCandidates(points, spots, glm::vec3(0));
	const auto selected = aether::SelectLocalShadows(candidates);
	REQUIRE(selected.size() == 13);
	CHECK(selected[7].lightType == 1);
	CHECK(selected[11].lightIndex == 4);
	CHECK(selected[12].lightIndex == 5);
	unsigned entries = 0;
	for (const auto& c: selected) { entries += c.lightType ? 1 : 6; }
	CHECK(entries == 48);
	points = {{glm::vec3(1, 0, 0)}};
	spots = {{glm::vec3(1, 0, 0)}};
	const auto tied = aether::LocalShadowCandidates(points, spots, glm::vec3(0));
	REQUIRE(tied.size() == 2);
	CHECK(tied[0].lightType == 0);
}

TEST_CASE("Shadow atlas packs full mixed budget without overlaps within adapter bounds")
{
	std::vector<unsigned> sizes{4096, 2048, 2048};
	for (int i = 0; i < 42; ++i) { sizes.push_back(384); }
	for (int i = 0; i < 6; ++i) { sizes.push_back(512); }
	const auto atlas = aether::PackShadowAtlas(sizes, 8192);
	REQUIRE(atlas.regions.size() == sizes.size());
	CHECK(atlas.width <= 8192);
	CHECK(atlas.height <= 8192);
	for (std::size_t i = 0; i < atlas.regions.size(); ++i)
	{
		const auto a = atlas.regions[i];
		CHECK(a.x + a.z <= atlas.width);
		CHECK(a.y + a.w <= atlas.height);
		for (std::size_t j = i + 1; j < atlas.regions.size(); ++j)
		{
			const auto b = atlas.regions[j];
			CHECK((a.x + a.z <= b.x || b.x + b.z <= a.x || a.y + a.w <= b.y || b.y + b.w <= a.y));
		}
	}
	std::vector<unsigned> fullSpots{4096, 2048, 2048};
	fullSpots.insert(fullSpots.end(), 48, 512);
	CHECK(aether::PackShadowAtlas(fullSpots, 8192).regions.size() == 51);
	CHECK(aether::PackShadowAtlas({4096}, 2048).regions.empty());
	const auto small = aether::PackShadowAtlas({384}, 2048);
	CHECK(small.width == 384);
	CHECK(small.height == 384);
}
