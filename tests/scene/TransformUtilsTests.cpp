#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "scene/TransformUtils.hpp"

using namespace aether;

namespace
{
    constexpr float kTol = 1e-3f;

    void CheckVec3(const glm::vec3& got, const glm::vec3& want)
    {
        CHECK(got.x == doctest::Approx(want.x).epsilon(kTol).scale(1.0));
        CHECK(got.y == doctest::Approx(want.y).epsilon(kTol).scale(1.0));
        CHECK(got.z == doctest::Approx(want.z).epsilon(kTol).scale(1.0));
    }

    void CheckMat4(const glm::mat4& got, const glm::mat4& want)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                CHECK(got[c][r] == doctest::Approx(want[c][r]).epsilon(kTol).scale(1.0));
            }
        }
    }
}

TEST_CASE("Compose->Decompose round-trips pos/euler/scale for regular angles") {
    const glm::vec3 poses[] = {{0, 0, 0}, {1.5f, -2.0f, 30.0f}};
    const glm::vec3 eulers[] = {{0, 0, 0}, {30, 45, 60}, {-30, 120, -75}, {10, -170, 5}};
    const glm::vec3 scales[] = {{1, 1, 1}, {2, 0.5f, 3}};

    for (const auto& p: poses)
    {
        for (const auto& e: eulers)
        {
            for (const auto& s: scales)
            {
                glm::vec3 pos{}, euler{}, scale{};
                DecomposeTRS(ComposeTransform(p, e, s), pos, euler, scale);
                CheckVec3(pos, p);
                CheckVec3(euler, e);
                CheckVec3(scale, s);
            }
        }
    }
}

TEST_CASE("ApplyTransformDelta adds per-channel onto an existing transform") {
    const glm::mat4 base = ComposeTransform({1, 2, 3}, {10, 20, 30}, {2, 2, 2});
    const TransformDelta delta{{0.5f, -1.0f, 4.0f}, {5, 0, -10}, {0.25f, 0.0f, -0.5f}};
    glm::vec3 pos{}, euler{}, scale{};
    DecomposeTRS(ApplyTransformDelta(base, delta), pos, euler, scale);
    CheckVec3(pos, {1.5f, 1.0f, 7.0f});
    CheckVec3(euler, {15, 20, 20});
    CheckVec3(scale, {2.25f, 2.0f, 1.5f});
}

TEST_CASE("ApplyTransformDelta with a zero delta preserves the matrix") {
    const glm::mat4 base = ComposeTransform({-3, 4, 0.5f}, {-30, 120, -75}, {1.5f, 0.5f, 3.0f});
    CheckMat4(ApplyTransformDelta(base, TransformDelta{}), base);
}

TEST_CASE("ApplyTransformDelta clamps scale away from zero") {
    const glm::mat4 base = ComposeTransform({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
    const TransformDelta delta{{}, {}, {-5.0f, -5.0f, -5.0f}};
    glm::vec3 pos{}, euler{}, scale{};
    DecomposeTRS(ApplyTransformDelta(base, delta), pos, euler, scale);
    CheckVec3(scale, {0.001f, 0.001f, 0.001f});
}

TEST_CASE("At and near gimbal lock the recomposed matrix still matches even if angles fold") {
    // angles must recompose to the same matrix.
    const glm::vec3 cases[] = {{90, 30, 40}, {-90, 10, -20}, {90, 0, 55}, {89.99f, -45, 15}};
    for (const auto& e: cases)
    {
        const glm::mat4 m = ComposeTransform({1, 2, 3}, e, {1, 1, 1});
        glm::vec3 pos{}, euler{}, scale{};
        DecomposeTRS(m, pos, euler, scale);
        CheckMat4(ComposeTransform(pos, euler, scale), m);
    }
}
