#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <iterator>
#include <stdexcept>

#include "editor/publish/PublishPlan.hpp"
#include "editor/publish/PublishSteps.hpp"
#include "editor/publish/PublishTransaction.hpp"
#include "io/FileUtil.hpp"

namespace
{
	using namespace aether;

	struct PublishFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / ("aether_publish_transaction_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		editor::PublishPlan plan;

		PublishFixture()
		{
			plan.outputDir = root / "Builds" / "Windows" / "Game";
			std::filesystem::create_directories(plan.outputDir);
			REQUIRE(io::file_util::WriteText(plan.outputDir / "previous.txt", "previous valid distribution").has_value());
		}

		~PublishFixture()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
	};

	editor::StepResult WriteReplacement(const editor::PublishPlan& plan, editor::PublishContext&, const editor::PublishToolchain&)
	{
		const auto result = io::file_util::WriteText(plan.outputDir / "replacement.txt", "verified replacement");
		return {.ok = result.has_value()};
	}

	editor::StepResult FailLate(const editor::PublishPlan&, editor::PublishContext&, const editor::PublishToolchain&)
	{
		return {.ok = false, .message = "injected script compilation failure"};
	}

	editor::StepResult RemoveStagedPackage(const editor::PublishPlan& plan, editor::PublishContext&, const editor::PublishToolchain&)
	{
		std::filesystem::remove_all(plan.outputDir);
		return {};
	}

	editor::StepResult ThrowLate(const editor::PublishPlan&, editor::PublishContext&, const editor::PublishToolchain&)
	{
		throw std::runtime_error("injected tool exception");
	}

	void CheckPreviousSurvives(const PublishFixture& fixture)
	{
		const auto previous = io::file_util::ReadText(fixture.plan.outputDir / "previous.txt");
		REQUIRE(previous.has_value());
		CHECK(*previous == "previous valid distribution");
		CHECK_FALSE(std::filesystem::exists(fixture.plan.outputDir / "replacement.txt"));
		CHECK(std::distance(std::filesystem::directory_iterator(fixture.plan.outputDir.parent_path()), std::filesystem::directory_iterator{}) == 1);
	}
}

TEST_CASE("Native publish preserves previous output when a later step fails")
{
	const PublishFixture fixture;
	editor::PublishContext context;
	const editor::PublishStep steps[] = {{"Stage", &WriteReplacement}, {"Build scripts", &FailLate}};
	const auto result = editor::RunNativePublishSteps(fixture.plan, steps, context, {});
	CHECK_FALSE(result.ok);
	CHECK(result.message == "injected script compilation failure");
	CheckPreviousSurvives(fixture);
}

TEST_CASE("Native publish promotes only the completed replacement")
{
	const PublishFixture fixture;
	editor::PublishContext context;
	const editor::PublishStep steps[] = {{"Stage and verify", &WriteReplacement}};
	REQUIRE(editor::RunNativePublishSteps(fixture.plan, steps, context, {}).ok);
	CHECK_FALSE(std::filesystem::exists(fixture.plan.outputDir / "previous.txt"));
	const auto replacement = io::file_util::ReadText(fixture.plan.outputDir / "replacement.txt");
	REQUIRE(replacement.has_value());
	CHECK(*replacement == "verified replacement");
	CHECK(std::distance(std::filesystem::directory_iterator(fixture.plan.outputDir.parent_path()), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Native publish restores previous output when promotion fails")
{
	const PublishFixture fixture;
	editor::PublishContext context;
	const editor::PublishStep steps[] = {{"Stage", &WriteReplacement}, {"Invalidate staging", &RemoveStagedPackage}};
	CHECK_FALSE(editor::RunNativePublishSteps(fixture.plan, steps, context, {}).ok);
	CheckPreviousSurvives(fixture);
}

TEST_CASE("Native publish creates a first distribution through the same staged pipeline")
{
	const PublishFixture fixture;
	std::filesystem::remove_all(fixture.plan.outputDir);
	editor::PublishContext context;
	const editor::PublishStep steps[] = {{"Stage and verify", &WriteReplacement}};
	REQUIRE(editor::RunNativePublishSteps(fixture.plan, steps, context, {}).ok);
	CHECK(std::filesystem::exists(fixture.plan.outputDir / "replacement.txt"));
}

TEST_CASE("An empty native publish pipeline cannot replace a previous distribution")
{
	const PublishFixture fixture;
	editor::PublishContext context;
	CHECK_FALSE(editor::RunNativePublishSteps(fixture.plan, {}, context, {}).ok);
	CheckPreviousSurvives(fixture);
}

TEST_CASE("Native publish cleans staging and preserves previous output after a tool exception")
{
	const PublishFixture fixture;
	editor::PublishContext context;
	const editor::PublishStep steps[] = {{"Stage", &WriteReplacement}, {"Throw", &ThrowLate}};
	CHECK_FALSE(editor::RunNativePublishSteps(fixture.plan, steps, context, {}).ok);
	CheckPreviousSurvives(fixture);
}
