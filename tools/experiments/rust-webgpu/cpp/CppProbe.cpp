#include "CppProbe.hpp"

#include <coroutine>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#include <box2d/box2d.h>
#include <emscripten.h>

namespace
{
	struct CoroutineProbe
	{
		struct promise_type
		{
			CoroutineProbe get_return_object()
			{
				return {std::coroutine_handle<promise_type>::from_promise(*this)};
			}

			std::suspend_never initial_suspend() noexcept
			{
				return {};
			}

			std::suspend_always final_suspend() noexcept
			{
				return {};
			}

			void return_void() noexcept
			{
			}

			void unhandled_exception()
			{
				std::terminate();
			}
		};

		std::coroutine_handle<promise_type> handle;

		~CoroutineProbe()
		{
			handle.destroy();
		}
	};

	CoroutineProbe Tick(int& ticks)
	{
		++ticks;
		co_return;
	}
} // namespace

int RunCppProbe()
{
	std::expected<int, int> value = 42;
	const auto formatted = std::format("count:{}", *value);
	int numbers[] = {1, 2, 3};
	std::span<const int> values(numbers);
	std::byte storage[256];
	std::pmr::monotonic_buffer_resource resource(storage, sizeof(storage));
	std::pmr::vector<int> allocated(&resource);
	allocated.push_back(values[2]);
	int ticks = 0;
	auto coroutine = Tick(ticks);
	const bool libraryPassed = formatted == "count:42" && allocated[0] == 3 && ticks == 1 && coroutine.handle.done() && std::filesystem::path("project/scenes/../save.dat").lexically_normal().generic_string() == "project/save.dat";

	auto worldDef = b2DefaultWorldDef();
	worldDef.gravity = {0.0f, -10.0f};
	worldDef.workerCount = 0;
	auto world = b2CreateWorld(&worldDef);
	auto bodyDef = b2DefaultBodyDef();
	bodyDef.type = b2_dynamicBody;
	auto body = b2CreateBody(world, &bodyDef);
	auto shapeDef = b2DefaultShapeDef();
	shapeDef.density = 1.0f;
	b2Circle circle = {{0.0f, 0.0f}, 0.5f};
	b2CreateCircleShape(body, &shapeDef, &circle);
	for (int i = 0; i < 60; ++i)
	{
		b2World_Step(world, 1.0f / 60.0f, 4);
	}
	const auto position = b2Body_GetPosition(body);
	const bool physicsPassed = position.y < -4.0f && position.y > -6.0f;
	b2DestroyWorld(world);
	const bool passed = libraryPassed && physicsPassed;
	std::printf("{\"probe\":\"cpp_library_box2d\",\"status\":\"%s\",\"cpp_standard\":%ld,\"library_passed\":%s,\"box2d_passed\":%s,\"body_y\":%.6f}\n",
	        passed ? "passed" : "failed",
	        static_cast<long>(__cplusplus),
	        libraryPassed ? "true" : "false",
	        physicsPassed ? "true" : "false",
	        position.y);
	return passed ? 0 : 1;
}

int main()
{
	return RunCppProbe();
}
