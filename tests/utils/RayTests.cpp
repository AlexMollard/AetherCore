#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "camera/Camera.hpp"
#include "utils/Ray.hpp"

using namespace aether;

TEST_CASE("RayVsAabb: frontal hit reports the entry distance") {
    float t = -1.0f;
    CHECK(RayVsAabb(Ray{{-5, 0, 0}, {1, 0, 0}}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(t == doctest::Approx(4.0f));
}

TEST_CASE("RayVsAabb: origin inside the box hits with t == 0") {
    float t = -1.0f;
    CHECK(RayVsAabb(Ray{{0.25f, -0.5f, 0.0f}, {0, 1, 0}}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(t == doctest::Approx(0.0f));
}

TEST_CASE("RayVsAabb: box behind the origin is rejected") {
    float t = -1.0f;
    CHECK(!RayVsAabb(Ray{{5, 0, 0}, {1, 0, 0}}, glm::vec3(-1), glm::vec3(1), t));
}

TEST_CASE("RayVsAabb: parallel offset ray misses; parallel on-boundary ray hits (no NaN)") {
    float t = -1.0f;
    CHECK(!RayVsAabb(Ray{{-5, 2, 0}, {1, 0, 0}}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(RayVsAabb(Ray{{-5, 1, 0}, {1, 0, 0}}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(t == doctest::Approx(4.0f));
}

TEST_CASE("RayVsAabb: diagonal and negative-direction hits") {
    float t = -1.0f;
    CHECK(RayVsAabb(Ray{{-3, -3, -3}, glm::normalize(glm::vec3(1, 1, 1))}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(t == doctest::Approx(glm::length(glm::vec3(2.0f))));

    CHECK(RayVsAabb(Ray{{0, 0, 5}, {0, 0, -1}}, glm::vec3(-1), glm::vec3(1), t));
    CHECK(t == doctest::Approx(4.0f));
}

TEST_CASE("BuildCameraRay round-trips points projected by the real camera") {
    CameraDesc desc;
    desc.mode = CameraMode::Free;
    desc.position = {3.0f, 4.0f, 10.0f};
    desc.yaw = 25.0f;
    desc.pitch = -15.0f;
    const Camera cam(desc);

    const float aspects[] = {16.0f / 9.0f, 4.0f / 3.0f, 2.35f};
    const glm::vec3 points[] = {{0, 0, 0}, {2, 1, -3}, {-4, 2, 5}, {3.5f, 4.0f, 2.0f}};

    for (const float aspect: aspects)
    {
        const glm::mat4 vp = cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix();
        const glm::mat4 invVp = glm::inverse(vp);
        for (const glm::vec3& p: points)
        {
            const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
            REQUIRE(clip.w > 0.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const glm::vec2 uv = (glm::vec2(ndc) + 1.0f) * 0.5f;

            const Ray ray = BuildCameraRay(invVp, uv, cam.GetPosition());
            const glm::vec3 toP = p - ray.origin;
            const float along = glm::dot(toP, ray.dir);
            const glm::vec3 closest = ray.origin + ray.dir * along;

            CHECK(along > 0.0f);
            CHECK(glm::length(p - closest) < 1e-3f * std::max(1.0f, glm::length(toP)));
        }
    }
}

TEST_CASE("Camera orthographic projection uses the authored vertical size") {
    CameraDesc desc;
    desc.mode = CameraMode::Manual;
    desc.projection = CameraProjection::Orthographic;
    desc.orthographicHeight = 10.0f;
    desc.nearPlane = 0.1f;
    desc.farPlane = 100.0f;
    const Camera cam(desc);

    constexpr float aspect = 2.0f;
    const glm::mat4 projection = cam.GetProjectionMatrix(aspect);
    const glm::vec4 left = projection * glm::vec4(-10.0f, 0.0f, -1.0f, 1.0f);
    const glm::vec4 right = projection * glm::vec4(10.0f, 0.0f, -1.0f, 1.0f);
    const glm::vec4 top = projection * glm::vec4(0.0f, 5.0f, -1.0f, 1.0f);

    CHECK(left.x == doctest::Approx(-1.0f));
    CHECK(right.x == doctest::Approx(1.0f));
    CHECK(top.y == doctest::Approx(-1.0f));
    CHECK(cam.GetProjection() == CameraProjection::Orthographic);
    CHECK(cam.GetOrthographicHeight() == doctest::Approx(10.0f));
}
