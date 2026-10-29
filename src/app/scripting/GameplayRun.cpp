#include "scripting/GameplayRun.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include "ProjectScripting.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "platform/Input.hpp"
#include "project/ProjectCommon.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/RustGameplayHost.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app::scripting
{
	namespace
	{
		using Json = nlohmann::json;
		using Status = aether::scripting::AgentJobStatus;

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::runtime_error(message);
			}
		}

		void ValidateConfig(const GameplayRunRequest& request)
		{
			Require(request.ticks > 0 && request.ticks <= 1000000, "ticks must be in [1, 1000000]");
			Require(std::isfinite(request.step) && request.step > 0 && request.step <= 1, "step must be finite and in (0, 1]");
			const auto& config = request.launchConfig;
			Require(config.is_object(), "launchConfig must be an object");
			for (const auto& [key, value]: config.items())
			{
				Require(key == "readSaves" || key == "input" || key == "assertions" || key == "profile" || key == "checkpoints" || key == "renderScale" || key == "changes", "Unknown launchConfig field: " + key);
			}
			Require(!config.contains("profile") || (config["profile"].is_string() && (config["profile"] == "native-headless-2d-v1" || config["profile"] == "native-desktop-2d-v1")), "Unsupported gameplay run profile");
			Require(!config.contains("readSaves") || config["readSaves"].is_boolean(), "readSaves must be a boolean");
			if (config.contains("renderScale"))
			{
				Require(config.value("profile", "native-headless-2d-v1") == "native-desktop-2d-v1", "renderScale override requires the desktop profile");
				const auto& scale = config["renderScale"];
				Require(scale.is_number() && std::isfinite(scale.get<double>()) && scale.get<double>() >= 0.25 && scale.get<double>() <= 2.0, "renderScale must be a finite number in [0.25, 2]");
			}
			for (const char* key: {"input", "assertions"})
			{
				Require(!config.contains(key) || config[key].is_array(), std::string(key) + " must be an array");
			}
			if (config.contains("checkpoints"))
			{
				Require(config.value("profile", "native-headless-2d-v1") == "native-desktop-2d-v1" && config["checkpoints"].is_array(), "Checkpoints require the desktop profile and an array");
				std::set<std::uint32_t> ticks;
				for (const auto& checkpoint: config["checkpoints"])
				{
					Require(checkpoint.is_object() && checkpoint.contains("tick") && checkpoint["tick"].is_number_integer(), "Checkpoint requires an integer tick");
					const auto tick = checkpoint["tick"].get<std::int64_t>();
					Require(tick > 0 && tick <= request.ticks && ticks.insert(static_cast<std::uint32_t>(tick)).second, "Checkpoint tick is repeated or outside the run");
					Require(checkpoint.contains("assertions") && checkpoint["assertions"].is_array() && !checkpoint["assertions"].empty(), "Checkpoint requires assertions");
					auto copied = request;
					copied.launchConfig = {{"assertions", checkpoint["assertions"]}};
					ValidateConfig(copied);
				}
			}
			if (config.contains("changes"))
			{
				Require(config.value("profile", "native-headless-2d-v1") == "native-desktop-2d-v1" && config["changes"].is_array(), "Scheduled changes require the desktop profile and an array");
				for (const auto& change: config["changes"])
				{
					Require(change.is_object() && change.contains("tick") && change["tick"].is_number_integer() && change["tick"].get<std::int64_t>() >= 0 && change["tick"].get<std::uint64_t>() < request.ticks,
					        "changes.tick must be an integer inside the run");
					Require(change.contains("graphics") || change.contains("resize"), "A scheduled change requires graphics or resize");
					for (const auto& [key, value]: change.items())
					{
						Require(key == "tick" || key == "graphics" || key == "resize", "Unknown scheduled change field: " + key);
					}
					if (change.contains("graphics"))
					{
						EngineSettings probe;
						Require(!AssignScheduledGraphics(probe, change["graphics"]).empty(), "changes.graphics must not be empty");
					}
					if (change.contains("resize"))
					{
						const auto& size = change["resize"];
						Require(size.is_array() && size.size() == 2 && size[0].is_number_integer() && size[1].is_number_integer() && size[0].get<std::int64_t>() >= 1 && size[0].get<std::int64_t>() <= 4096 &&
						                size[1].get<std::int64_t>() >= 1 && size[1].get<std::int64_t>() <= 4096,
						        "changes.resize must be [width, height] in 1..4096");
					}
				}
			}

			for (const auto& event: config.value("input", Json::array()))
			{
				Require(event.is_object() && event.contains("tick") && event["tick"].is_number_integer() && event["tick"].get<std::int64_t>() >= 0, "input.tick must be a nonnegative integer");
				Require(event["tick"].get<std::uint64_t>() < request.ticks, "input.tick is outside the run");
				Require(event.contains("key") && event["key"].is_string() && Input::KeyCodeFromName(event["key"].get<std::string>()) >= 0, "input.key is unknown");
				Require(event.contains("down") && event["down"].is_boolean(), "input.down must be a boolean");
			}
			for (const auto& assertion: config.value("assertions", Json::array()))
			{
				Require(assertion.is_object(), "assertion must be an object");
				for (const char* field: {"entity", "type", "property"})
				{
					Require(assertion.contains(field) && assertion[field].is_string() && !assertion[field].get<std::string>().empty(), std::string("assertion requires ") + field);
				}
				Require(assertion.contains("equals"), "assertion requires equals");
				if (assertion.contains("tolerance"))
				{
					Require(assertion["tolerance"].is_number(), "assertion tolerance must be numeric");
					const double tolerance = assertion["tolerance"].get<double>();
					Require(std::isfinite(tolerance) && tolerance >= 0, "assertion tolerance must be finite and nonnegative");
				}
			}
		}

		// ApplyScene's data-only paths work without a GraphicsDevice. Reject everything
		// requiring omitted systems before applying so no authored gameplay is silently lost.
		void ValidateScene(const scene::SceneDescription& description)
		{
			Require(description.prefabInstances.empty() && description.includes.empty(), "Headless run does not support linked prefabs or scene includes");
			Require(description.lights.empty() && !description.environment, "Headless run does not support 3D lighting/environment");
			const std::set<std::string> supported{"Name", "Transform", "Rigid Body 2D", "Collider 2D", "Joint 2D", "UI Canvas", "UI Rect", "UI Text", "UI Image", "UI Progress Bar", "UI Mask"};
			for (const auto& record: description.entities)
			{
				Require(!record.meshRenderer && !record.mesh && !record.material && !record.skinned && !record.physics && !record.effect && !record.joint && record.scriptJoints.empty() && !record.spriteAnimator,
				        "Headless run requires unavailable model/3D physics/effect/animation systems: " + record.name);
				for (const auto& component: record.reflected)
				{
					Require(supported.contains(component.type), "Headless run does not support component: " + component.type);
				}
			}
		}

		Json CopyProperty(const ScriptPropertyValue& property, World& world)
		{
			switch (property.type)
			{
				case ScriptPropertyValue::Type::Float:
					Require(std::isfinite(property.f4[0]), "Script returned a nonfinite property");
					return property.f4[0];
				case ScriptPropertyValue::Type::Vector3:
					Require(std::isfinite(property.f4[0]) && std::isfinite(property.f4[1]) && std::isfinite(property.f4[2]), "Script returned a nonfinite vector");
					return Json::array({property.f4[0], property.f4[1], property.f4[2]});
				case ScriptPropertyValue::Type::Bool:
					return property.i64 != 0;
				case ScriptPropertyValue::Type::String:
					return property.str;
				case ScriptPropertyValue::Type::Entity:
				case ScriptPropertyValue::Type::Component:
				{
					const Entity target{static_cast<std::uint32_t>(property.i64)};
					if (world.GetRegistry().valid(World::ToEntt(target)))
					{
						if (const auto* name = world.TryGet<NameComponent>(target))
						{
							return name->name;
						}
					}
					return nullptr;
				}
				default:
					return property.i64;
			}
		}

		bool Equal(const Json& actual, const Json& expected, double tolerance)
		{
			if (tolerance == 0 && actual.is_number_integer() && expected.is_number_integer())
			{
				if (actual.is_number_unsigned() == expected.is_number_unsigned())
				{
					return actual == expected;
				}
				const auto signedValue = (actual.is_number_unsigned() ? expected : actual).get<std::int64_t>();
				const auto unsignedValue = (actual.is_number_unsigned() ? actual : expected).get<std::uint64_t>();
				return signedValue >= 0 && static_cast<std::uint64_t>(signedValue) == unsignedValue;
			}
			if (actual.is_number() && expected.is_number())
			{
				return std::abs(actual.get<double>() - expected.get<double>()) <= tolerance;
			}
			if (actual.is_array() && expected.is_array() && actual.size() == expected.size())
			{
				for (std::size_t i = 0; i < actual.size(); ++i)
				{
					if (!Equal(actual[i], expected[i], tolerance))
					{
						return false;
					}
				}
				return true;
			}
			return actual == expected;
		}
	} // namespace

	void ValidateGameplayRunConfig(const GameplayRunRequest& request)
	{
		ValidateConfig(request);
	}

	std::vector<std::string> AssignScheduledGraphics(EngineSettings& settings, const Json& graphics)
	{
		Require(graphics.is_object(), "changes.graphics must be an object");
		std::vector<std::string> fields;
		for (const auto& [key, value]: graphics.items())
		{
			Require(value.is_string() || value.is_number() || value.is_boolean(), "changes.graphics." + key + " must be a string, number or boolean");
			const std::string field = "graphics." + key;
			const std::string text = value.is_string() ? value.get<std::string>() : value.dump();
			Require(SetSettingValueFromString(settings, field, text), "changes.graphics." + key + " is not a setting or cannot hold '" + text + "'");
			fields.push_back(field);
		}
		return fields;
	}

	void WriteGameplayState(const GameplayRunRequest& request, World& world, GameplayHost& hostRef, ScriptComponentSystem& scripts, bool allowSaveReads, const Json& observations, aether::scripting::AgentReport* report)
	{
		auto* host = &hostRef;
		Json state = {{"schemaVersion", 1}, {"initialFrame", 0}, {"ticks", request.ticks}, {"step", request.step}, {"readSaves", allowSaveReads}, {"entities", Json::array()}, {"assertions", Json::array()}};
		for (auto&& [handle, component]: world.View<ScriptComponent>().each())
		{
			const Entity entity = World::FromEntt(handle);
			const auto* name = world.TryGet<NameComponent>(entity);
			for (std::uint32_t index = 0; index < component.scripts.size(); ++index)
			{
				const auto& entry = component.scripts[index];
				const auto instance = scripts.GetInstanceHandle(entity.id, index);
				Require(entry.path.empty() || instance != 0 || !entry.attached, "Script instance missing: " + entry.path);
				// Disabled authored scripts are intentionally not executed.
				if (!entry.attached)
				{
					continue;
				}
				Json values = Json::object();
				const auto properties = host->GetScriptProperties(entry.path);
				for (std::size_t p = 0; p < properties.size(); ++p)
				{
					ScriptPropertyValue value;
					Require(host->GetPropertyValue(instance, static_cast<int>(p), value), "Cannot copy property: " + properties[p].name);
					values[properties[p].name] = CopyProperty(value, world);
				}
				state["entities"].push_back({{"entity", name ? name->name : std::to_string(entity.id)}, {"type", entry.path}, {"properties", values}});
			}
		}
		for (auto&& [handle, name]: world.View<NameComponent>().each())
		{
			const Entity entity = World::FromEntt(handle);
			Json values = {{"exists", true}};
			if (const auto* transform = world.TryGet<TransformComponent>(entity))
			{
				values["position"] = Json::array({transform->localToWorld[3].x, transform->localToWorld[3].y, transform->localToWorld[3].z});
				Json matrix = Json::array();
				for (int column = 0; column < 4; ++column)
				{
					for (int row = 0; row < 4; ++row)
					{
						matrix.push_back(transform->localToWorld[column][row]);
					}
				}
				values["localToWorld"] = std::move(matrix);
			}
			if (const auto* text = world.TryGet<ui::UIText>(entity))
			{
				values["uiText"] = text->text;
			}
			state["entities"].push_back({{"entity", name.name}, {"type", "$engine"}, {"properties", values}});
		}
		for (const auto& observation: observations)
		{
			state["entities"].push_back(observation);
		}
		std::set<std::string> observedEntities;
		for (const auto& entity: state["entities"])
		{
			observedEntities.insert(entity["entity"].get<std::string>());
		}
		for (const auto& assertion: request.launchConfig.value("assertions", Json::array()))
		{
			if (assertion["type"] != "$engine" || assertion["property"] != "exists")
			{
				continue;
			}
			const bool found = std::any_of(state["entities"].begin(), state["entities"].end(), [&](const Json& entry) { return entry["entity"] == assertion["entity"] && entry["type"] == "$engine"; });
			if (!found)
			{
				state["entities"].push_back({{"entity", assertion["entity"]}, {"type", "$engine"}, {"properties", {{"exists", false}}}});
			}
		}

		std::vector<aether::scripting::AgentDiagnostic> failures;
		std::size_t assertionIndex = 0;
		for (const auto& assertion: request.launchConfig.value("assertions", Json::array()))
		{
			const Json* actual = nullptr;
			std::size_t matches = 0;
			const bool entityFound = observedEntities.contains(assertion["entity"].get<std::string>());
			for (const auto& entity: state["entities"])
			{
				if (entity["entity"] == assertion["entity"] && entity["type"] == assertion["type"])
				{
					++matches;
					const auto property = assertion["property"].get<std::string>();
					if (entity["properties"].contains(property))
					{
						actual = &entity["properties"][property];
					}
				}
			}
			const bool passed = matches == 1 && actual != nullptr && Equal(*actual, assertion["equals"], assertion.value("tolerance", 0.0));
			if (!passed)
			{
				const std::string code = matches > 1         ? "GAMEPLAY_ASSERTION_AMBIGUOUS_TARGET"
				                         : matches == 0      ? (entityFound ? "GAMEPLAY_ASSERTION_MISSING_TYPE" : "GAMEPLAY_ASSERTION_MISSING_ENTITY")
				                         : actual == nullptr ? "GAMEPLAY_ASSERTION_MISSING_PROPERTY"
				                                             : "GAMEPLAY_ASSERTION_VALUE_MISMATCH";
				const Json detail = {{"assertionIndex", assertionIndex},
				        {"tick", request.ticks},
				        {"entity", assertion["entity"]},
				        {"type", assertion["type"]},
				        {"property", assertion["property"]},
				        {"expected", assertion["equals"]},
				        {"actual", matches == 1 && actual ? *actual : Json(nullptr)},
				        {"actualPresent", matches == 1 && actual != nullptr},
				        {"matchCount", matches},
				        {"tolerance", assertion.value("tolerance", 0.0)}};
				failures.push_back({.code = code, .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = detail.dump(), .source = aether::scripting::AgentSourceLocation{.file = io::PlatformPaths::PathToUtf8(request.output)}});
			}
			state["assertions"].push_back({{"entity", assertion["entity"]},
			        {"type", assertion["type"]},
			        {"property", assertion["property"]},
			        {"expected", assertion["equals"]},
			        {"actual", actual ? *actual : Json(nullptr)},
			        {"tolerance", assertion.value("tolerance", 0.0)},
			        {"passed", passed}});
			++assertionIndex;
		}
		Require(request.output.parent_path().empty() || io::file_util::CreateDirectories(request.output.parent_path()).has_value(), "Cannot create artifact directory");
		const auto written = io::file_util::WriteText(request.output, state.dump(2));
		Require(written.has_value(), "Cannot write copied script state artifact");
		if (report)
		{
			report->artifacts.push_back({io::PlatformPaths::PathToUtf8(request.output), std::filesystem::file_size(request.output)});
			report->diagnostics.insert(report->diagnostics.end(), failures.begin(), failures.end());
		}
		Require(failures.empty(), failures.empty() ? "" : failures.front().code + ": " + failures.front().message);
	}

	aether::scripting::AgentReport RunGameplay(const GameplayRunRequest& request)
	{
		const auto started = std::chrono::steady_clock::now();
		aether::scripting::AgentReport report;
		report.jobId = "gameplay-run";
		report.status = Status::Failed;
		report.phase = "run";
		report.target = "desktop";
		report.language = "rust";
		report.profile = "native-headless-2d-v1";
		report.requiredChecks = {{"scene-load", Status::Failed}, {"gameplay-ticks", Status::Failed}, {"state-assertions", Status::Failed}};
		try
		{
			ValidateConfig(request);
			Require(request.launchConfig.value("profile", "native-headless-2d-v1") == "native-headless-2d-v1", "Headless runner requires native-headless-2d-v1");
			Require(!request.output.empty(), "State artifact output path is required");
			Require(scene::IsValidAssetName(request.scene), "A valid named scene is required");
			const auto settings = project::ReadProjectDescriptor(request.project);
			Require(settings.has_value(), settings ? "" : settings.error().message);
			report.language = std::string(GameplayLanguageName(settings->language));
			const auto scenePath = settings->scenesDir / (request.scene + ".scene.toml");
			const auto text = io::file_util::ReadText(scenePath);
			Require(text.has_value(), "Cannot read named scene: " + scenePath.string());
			const auto description = scene::ParseToml(*text);
			Require(description.has_value(), "Scene TOML is invalid");
			ValidateScene(*description);

			ServiceContainer services;
			World world;
			Input input;
			SceneContext context;
			context.world = &world;
			context.input = &input;
			context.services = &services;
			context.allowSaveReads = request.launchConfig.value("readSaves", false);
			context.beforeSceneLoad = [&](const std::string& name)
			{
				Require(scene::IsValidAssetName(name), "Invalid requested scene destination");
				const auto destination = scene::ReadSceneFile(name);
				Require(destination.has_value(), "Requested scene destination is missing: " + name);
				ValidateScene(*destination);
			};
			services.Register(context);
			services.Register(input);
			services.Register(world);
			auto physics = std::make_unique<Physics2DSystem>();
			context.physics2D = physics.get();
			services.Register(*physics);
			world.RegisterSystem(std::move(physics));
			RustGameplayHost rust;
			std::unique_ptr<CSharpScriptingSubsystem> managed;
			GameplayHost* host = nullptr;
			if (settings->language == GameplayLanguage::Rust)
			{
				const auto loaded = rust.Load(request.module, world, &context);
				Require(loaded.has_value(), loaded ? "" : loaded.error().message);
				host = &rust.Gameplay();
			}
			else
			{
				Require(std::filesystem::is_regular_file(request.managedDir / "AetherGame.dll"), "Isolated managed directory must contain AetherGame.dll");
				managed = std::make_unique<CSharpScriptingSubsystem>(request.managedDir);
				Require(managed->IsAvailable(), "C# host is unavailable");
				if (!managed->Gameplay().IsAvailable())
				{
					managed->LoadScripts();
				}
				Require(managed->Gameplay().IsAvailable(), "C# gameplay assembly is unavailable");
				host = &managed->Gameplay();
			}
			services.Register(*host);
			std::vector<std::string> callbackErrors;
			host->SetErrorHandler([&](const std::string& message) { callbackErrors.push_back(message); });
			ScriptComponentSystem scripts(services);

			struct Cleanup
			{
				ScriptComponentSystem& scripts;
				World& world;
				GameplayHost& host;
				bool closed = false;

				void Close()
				{
					if (!closed)
					{
						closed = true;
						scripts.Invalidate(world);
						host.SetErrorHandler({});
					}
				}

				~Cleanup()
				{
					Close();
				}
			} cleanup{scripts, world, *host};

			scene::SetProjectSceneDirectories(settings->scenesDir, settings->prefabsDir, request.project);

			struct DirectoryCleanup
			{
				~DirectoryCleanup()
				{
					scene::ClearProjectSceneDirectories();
				}
			} directoryCleanup;

			scene::ApplySceneDeps deps;
			deps.sceneContext = &context;
			scene::ApplyScene(*description, world, deps);
			report.requiredChecks[0].status = Status::Passed;
			const auto events = request.launchConfig.value("input", Json::array());
			for (std::uint32_t tick = 0; tick < request.ticks; ++tick)
			{
				for (const auto& event: events)
				{
					if (event["tick"].get<std::uint32_t>() == tick)
					{
						input.SetSyntheticKey(Input::KeyCodeFromName(event["key"].get<std::string>()), event["down"].get<bool>(), false);
					}
				}
				input.Update();
				context.physics2D->Update(world, request.step);
				scripts.Update(world, request.step);
				for (auto&& [handle, component]: world.View<ScriptComponent>().each())
				{
					if (ecs::HasDisabledAncestor(world, World::FromEntt(handle)))
					{
						continue;
					}
					for (const auto& entry: component.scripts)
					{
						Require(entry.path.empty() || entry.attached, "Script type or instance failed: " + entry.path);
					}
				}
				Require(callbackErrors.empty(), callbackErrors.empty() ? "" : callbackErrors.front());
				if (managed)
				{
					Require(managed->ScriptErrors().empty(), managed->ScriptErrors().empty() ? "" : managed->ScriptErrors().front());
				}
				for (const auto& diagnostic: rust.Diagnostics())
				{
					Require(diagnostic.severity != aether::scripting::AgentDiagnosticSeverity::Error, diagnostic.message);
				}
			}
			report.requiredChecks[1].status = Status::Passed;
			WriteGameplayState(request, world, *host, scripts, context.allowSaveReads, Json::array(), &report);
			cleanup.Close();
			rust.Unload();
			Require(callbackErrors.empty(), callbackErrors.empty() ? "" : callbackErrors.front());
			if (managed)
			{
				Require(managed->ScriptErrors().empty(), managed->ScriptErrors().empty() ? "" : managed->ScriptErrors().front());
			}
			for (const auto& diagnostic: rust.Diagnostics())
			{
				Require(diagnostic.severity != aether::scripting::AgentDiagnosticSeverity::Error, diagnostic.message);
			}
			report.requiredChecks[2].status = Status::Passed;
			report.status = Status::Passed;
		}
		catch (const std::exception& error)
		{
			report.diagnostics.push_back({"GAMEPLAY_RUN", aether::scripting::AgentDiagnosticSeverity::Error, error.what(), std::nullopt});
		}
		report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		return report;
	}
} // namespace aether::app::scripting
