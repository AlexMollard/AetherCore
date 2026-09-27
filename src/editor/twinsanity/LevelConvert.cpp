#include "twinsanity/LevelConvert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/AssetManager.hpp"
#include "editor/UndoStack.hpp"
#include "io/FileSystem.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::editor::twinsanity
{
	namespace
	{
		using nlohmann::json;
		namespace sc = app::scene;

		// Written by the C# half (TwinsanityConvert.ManifestPath); drift reports land beside it.
		constexpr const char* kManifestPath = "project://.aether/convert/manifest.json";
		constexpr const char* kReportDir = "project://.aether/convert/";

		// Report thresholds (DESIGN.md §2.8).
		constexpr float kPositionTolerance = 0.01f; // 1 cm
		constexpr float kAngleTolerance = 0.5f;     // degrees

		// Derived, not random, node ids: the same disc instance gets the same id on every run, so
		// a re-run writes byte-identical scenes and report mode finds a saved instance by its key.
		std::uint64_t NodeIdOf(const std::string& key)
		{
			std::uint64_t h = 14695981039346656037ull;
			for (const unsigned char c: key)
			{
				h ^= c;
				h *= 1099511628211ull;
			}
			return h != 0 ? h : 1;
		}

		glm::vec3 Vec3(const json& a)
		{
			return {a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()};
		}

		bool Listed(const std::vector<std::string>& names, const std::string& name)
		{
			return std::ranges::find(names, "*") != names.end() || std::ranges::find(names, name) != names.end();
		}

		// A manifest property ({t, v}) as the script property the scene stores. Entity refs are
		// resolved separately, once every instance root of the area exists.
		std::optional<ScriptPropertyValue> ValueOf(const json& prop)
		{
			const std::string t = prop.at("t").get<std::string>();
			ScriptPropertyValue v;
			if (t == "string")
			{
				v.type = ScriptPropertyValue::Type::String;
				v.str = prop.at("v").get<std::string>();
			}
			else if (t == "int" || t == "enum")
			{
				v.type = t == "int" ? ScriptPropertyValue::Type::Int : ScriptPropertyValue::Type::Enum;
				v.i64 = prop.at("v").get<std::int64_t>();
			}
			else if (t == "bool")
			{
				v.type = ScriptPropertyValue::Type::Bool;
				v.i64 = prop.at("v").get<bool>() ? 1 : 0;
			}
			else
			{
				return std::nullopt;
			}
			return v;
		}

		ScriptEntry* ScriptOf(World& world, Entity e, const std::string& type)
		{
			auto* scripts = world.TryGet<ScriptComponent>(e);
			if (scripts == nullptr)
			{
				return nullptr;
			}
			for (ScriptEntry& s: scripts->scripts)
			{
				if (s.path == type)
				{
					return &s;
				}
			}
			return nullptr;
		}

		// Each entity's name path from the root ("root/child#n"), n counting same-named siblings.
		std::vector<std::string> NamePaths(const sc::SceneDescription& desc)
		{
			std::vector<std::string> paths(desc.entities.size());
			std::map<std::pair<int, std::string>, int> seen;
			for (std::size_t i = 0; i < desc.entities.size(); ++i)
			{
				const sc::EntityRecord& r = desc.entities[i];
				const int n = seen[{r.parentIndex, r.name}]++;
				const std::string self = r.name + "#" + std::to_string(n);
				paths[i] = r.parentIndex >= 0 && static_cast<std::size_t>(r.parentIndex) < i ? paths[static_cast<std::size_t>(r.parentIndex)] + "/" + self : self;
			}
			return paths;
		}

		// DESIGN.md §2.8 step 2: a re-captured template keeps the existing prefab's guids, matched
		// by entity name path, so scene overrides keep routing to the same entities; new entities
		// get guids above every old one (a removed entity's guid is never reused).
		void RestampGuids(sc::SceneDescription& fresh, const std::optional<sc::SceneDescription>& old)
		{
			std::uint64_t next = 1;
			std::unordered_map<std::string, std::uint64_t> oldGuids;
			if (old)
			{
				const std::vector<std::string> paths = NamePaths(*old);
				for (std::size_t i = 0; i < old->entities.size(); ++i)
				{
					const std::uint64_t guid = sc::EffectiveGuid(old->entities[i], i);
					oldGuids.emplace(paths[i], guid);
					next = std::max(next, guid + 1);
				}
			}
			const std::vector<std::string> paths = NamePaths(fresh);
			for (std::size_t i = 0; i < fresh.entities.size(); ++i)
			{
				const auto it = oldGuids.find(paths[i]);
				fresh.entities[i].guid = it != oldGuids.end() ? it->second : next++;
			}
		}

		// A parentless entity by name (templates, area roots, the sky).
		Entity TopLevel(World& world, const std::string& name)
		{
			for (const auto handle: world.View<NameComponent>())
			{
				const Entity e = World::FromEntt(handle);
				const auto* nc = world.TryGet<NameComponent>(e);
				const auto* h = world.TryGet<HierarchyComponent>(e);
				if (nc != nullptr && nc->name == name && (h == nullptr || !h->parent.IsValid()))
				{
					return e;
				}
			}
			return Entity{};
		}

		// Static content (an area's scenery and collision, the sky) captured exactly as the editor
		// saves a scene - CaptureScene's entity order and asset manifest - so opening and re-saving
		// the scene does not reshuffle it. Every other top-level entity is masked SceneTransient for
		// the capture, and each static entity gets a derived node id first, so a re-run writes the
		// same file.
		sc::SceneDescription CaptureStatics(World& world, const std::vector<Entity>& roots, const std::string& prefix, const MaterialRegistry& materials, const TextureRegistry& textures)
		{
			std::vector<Entity> masked;
			for (const auto handle: world.View<NameComponent>())
			{
				const Entity e = World::FromEntt(handle);
				const auto* h = world.TryGet<HierarchyComponent>(e);
				if ((h == nullptr || !h->parent.IsValid()) && std::ranges::find(roots, e) == roots.end() && !world.Has<SceneTransientComponent>(e))
				{
					masked.push_back(e);
				}
			}
			for (const Entity e: masked)
			{
				world.Emplace<SceneTransientComponent>(e);
			}
			std::uint64_t n = 0;
			std::vector<Entity> stack(roots.rbegin(), roots.rend());
			while (!stack.empty())
			{
				const Entity e = stack.back();
				stack.pop_back();
				world.EmplaceOrReplace<SceneNodeComponent>(e, SceneNodeComponent{NodeIdOf(prefix + "#" + std::to_string(n++))});
				if (const auto* h = world.TryGet<HierarchyComponent>(e))
				{
					stack.insert(stack.end(), h->children.rbegin(), h->children.rend());
				}
			}
			sc::SceneDescription desc = sc::CaptureScene(world, materials, textures, nullptr);
			for (const Entity e: masked)
			{
				world.Remove<SceneTransientComponent>(e);
			}
			return desc;
		}

		// Drop every top-level entity named `name` (and its subtree) from a description, remapping
		// parent indices of what stays.
		void RemoveTopLevel(sc::SceneDescription& desc, const std::string& name)
		{
			std::vector<bool> drop(desc.entities.size(), false);
			for (std::size_t i = 0; i < desc.entities.size(); ++i)
			{
				const int parent = desc.entities[i].parentIndex;
				drop[i] = parent < 0 ? desc.entities[i].name == name : drop[static_cast<std::size_t>(parent)];
			}
			std::vector<int> remap(desc.entities.size(), -1);
			std::vector<sc::EntityRecord> kept;
			for (std::size_t i = 0; i < desc.entities.size(); ++i)
			{
				if (!drop[i])
				{
					remap[i] = static_cast<int>(kept.size());
					kept.push_back(std::move(desc.entities[i]));
				}
			}
			for (sc::EntityRecord& r: kept)
			{
				r.parentIndex = r.parentIndex >= 0 ? remap[static_cast<std::size_t>(r.parentIndex)] : -1;
			}
			desc.entities = std::move(kept);
		}

		// CaptureScene's instance order (ascending root node id, unsigned), so the editor's first
		// save of a converted scene does not reorder it.
		void SortByNode(std::vector<sc::PrefabInstanceRecord>& instances)
		{
			std::ranges::stable_sort(instances, {}, &sc::PrefabInstanceRecord::node);
		}

		void Append(sc::SceneDescription& into, const sc::SceneDescription& from)
		{
			const int base = static_cast<int>(into.entities.size());
			for (sc::EntityRecord r: from.entities)
			{
				r.parentIndex = r.parentIndex >= 0 ? r.parentIndex + base : -1;
				into.entities.push_back(std::move(r));
			}
			for (const sc::AssetManifestEntry& a: from.assetManifest)
			{
				if (std::ranges::none_of(into.assetManifest, [&](const sc::AssetManifestEntry& b) { return b.id == a.id; }))
				{
					into.assetManifest.push_back(a);
				}
			}
		}

		// One manifest instance made live: the linked prefab instance at its world transform with
		// its derived node id, its name, the descriptor's properties on the root, and its Point/Path
		// children. Refs are set once every root of the area exists (ResolveRefs).
		Entity PlaceInstance(World& world, const sc::ApplySceneDeps& deps, const json& inst, const sc::SceneDescription& prefab)
		{
			const std::string prefabName = inst.at("prefab").get<std::string>();
			const glm::mat4 xform = ComposeTransform(Vec3(inst.at("position")), Vec3(inst.at("euler")), Vec3(inst.at("scale")));
			const Entity root = sc::InstantiatePrefabInstance(prefabName, prefab, world, deps, xform, NodeIdOf(inst.at("key").get<std::string>()));
			if (!root.IsValid())
			{
				return root;
			}
			world.EmplaceOrReplace<NameComponent>(root, NameComponent{inst.at("name").get<std::string>()});
			if (ScriptEntry* script = ScriptOf(world, root, inst.at("script").get<std::string>()))
			{
				for (const auto& [name, prop]: inst.at("properties").items())
				{
					if (const auto value = ValueOf(prop))
					{
						script->properties[name] = *value;
					}
				}
			}
			for (const char* list: {"points", "path"})
			{
				if (!inst.contains(list))
				{
					continue;
				}
				const std::string prefix = std::string(list) == "points" ? "Point " : "Path ";
				int n = 0;
				for (const json& p: inst.at(list))
				{
					const Entity child = world.Create();
					world.Emplace<NameComponent>(child, NameComponent{prefix + std::to_string(n++)});
					world.Emplace<TransformComponent>(child, TransformComponent{ComposeTransform(Vec3(p), glm::vec3(0.0f), glm::vec3(1.0f))});
					ecs::SetParent(world, child, root);
				}
			}
			return root;
		}

		void ResolveRefs(World& world, const json& inst, Entity root, const std::unordered_map<std::string, Entity>& roots)
		{
			ScriptEntry* script = ScriptOf(world, root, inst.at("script").get<std::string>());
			if (script == nullptr)
			{
				return;
			}
			for (const auto& [name, prop]: inst.at("properties").items())
			{
				if (!prop.contains("ref"))
				{
					continue;
				}
				const auto it = roots.find(prop.at("ref").get<std::string>());
				if (it != roots.end())
				{
					ScriptPropertyValue v;
					v.type = ScriptPropertyValue::Type::Entity;
					v.i64 = static_cast<std::int64_t>(it->second.id);
					script->properties[name] = v;
				}
			}
		}

		float AngleBetween(glm::vec3 eulerA, glm::vec3 eulerB)
		{
			const glm::quat a = glm::quat_cast(ComposeTransform(glm::vec3(0.0f), eulerA, glm::vec3(1.0f)));
			const glm::quat b = glm::quat_cast(ComposeTransform(glm::vec3(0.0f), eulerB, glm::vec3(1.0f)));
			return glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b)))));
		}

		std::string PropText(const ScriptPropertyValue& v)
		{
			switch (v.type)
			{
				case ScriptPropertyValue::Type::String: return "'" + v.str + "'";
				case ScriptPropertyValue::Type::Bool: return v.i64 != 0 ? "true" : "false";
				default: return std::to_string(v.i64);
			}
		}

		// The saved instance's root script as the scene stores it: the prefab root's script with the
		// instance's root override (if any) merged over it.
		std::optional<sc::ScriptRecord> SavedScript(const sc::PrefabInstanceRecord& rec, const sc::SceneDescription& prefab, const std::string& type)
		{
			if (prefab.entities.empty())
			{
				return std::nullopt;
			}
			sc::EntityRecord root = prefab.entities[0];
			const std::uint64_t rootGuid = sc::EffectiveGuid(prefab.entities[0], 0);
			for (const sc::PrefabEntityOverride& o: rec.overrides)
			{
				if (o.guid == rootGuid)
				{
					root = sc::MergePrefabOverride(prefab.entities[0], o.partialToml);
				}
			}
			for (const sc::ScriptRecord& s: root.scripts)
			{
				if (s.type == type)
				{
					return s;
				}
			}
			return std::nullopt;
		}

		std::vector<glm::vec3> SavedPoints(const sc::PrefabInstanceRecord& rec, const std::string& prefix)
		{
			std::map<int, glm::vec3> found;
			for (const sc::EntityRecord& r: rec.addedEntities)
			{
				if (r.parentIndex < 0 && r.name.starts_with(prefix))
				{
					found[std::atoi(r.name.c_str() + prefix.size())] = r.position;
				}
			}
			std::vector<glm::vec3> out;
			for (const auto& [i, p]: found)
			{
				out.push_back(p);
			}
			return out;
		}

		// Report mode: every difference between what the current extract would write for one scene
		// and what that scene holds on disk, one line each.
		std::vector<std::string> DiffScene(const json& instances, const sc::SceneDescription& saved, std::map<std::string, std::optional<sc::SceneDescription>>& prefabs)
		{
			std::vector<std::string> lines;
			std::unordered_map<std::uint64_t, const sc::PrefabInstanceRecord*> byNode;
			for (const sc::PrefabInstanceRecord& r: saved.prefabInstances)
			{
				byNode[r.node] = &r;
			}
			std::set<std::uint64_t> expected;
			for (const json& inst: instances)
			{
				const std::string key = inst.at("key").get<std::string>();
				const std::uint64_t node = NodeIdOf(key);
				expected.insert(node);
				const auto it = byNode.find(node);
				if (it == byNode.end())
				{
					lines.push_back(std::format("- `{}` {} ({}): missing from the scene", key, inst.at("name").get<std::string>(), inst.at("prefab").get<std::string>()));
					continue;
				}
				const sc::PrefabInstanceRecord& rec = *it->second;
				const std::string prefabName = inst.at("prefab").get<std::string>();
				if (rec.prefabPath != prefabName)
				{
					lines.push_back(std::format("- `{}`: prefab {} in the scene, {} in the extract", key, rec.prefabPath, prefabName));
				}
				const glm::vec3 pos = Vec3(inst.at("position"));
				if (glm::length(rec.position - pos) > kPositionTolerance)
				{
					lines.push_back(std::format("- `{}`: position ({:.3f}, {:.3f}, {:.3f}) in the scene, ({:.3f}, {:.3f}, {:.3f}) in the extract", key,
					        rec.position.x, rec.position.y, rec.position.z, pos.x, pos.y, pos.z));
				}
				if (const float angle = AngleBetween(rec.eulerDeg, Vec3(inst.at("euler"))); angle > kAngleTolerance)
				{
					lines.push_back(std::format("- `{}`: rotated {:.2f} degrees from the extract", key, angle));
				}
				const glm::vec3 scale = Vec3(inst.at("scale"));
				if (glm::length(rec.scale - scale) > kPositionTolerance)
				{
					lines.push_back(std::format("- `{}`: scale ({:.3f}, {:.3f}, {:.3f}) in the scene, ({:.3f}, {:.3f}, {:.3f}) in the extract", key,
					        rec.scale.x, rec.scale.y, rec.scale.z, scale.x, scale.y, scale.z));
				}
				auto& prefab = prefabs[rec.prefabPath];
				if (!prefab)
				{
					prefab = sc::ReadPrefabFile(rec.prefabPath);
				}
				const std::string scriptType = inst.at("script").get<std::string>();
				const std::optional<sc::ScriptRecord> script = prefab ? SavedScript(rec, *prefab, scriptType) : std::nullopt;
				if (!script)
				{
					lines.push_back(std::format("- `{}`: no {} script on the saved root", key, scriptType));
					continue;
				}
				for (const auto& [name, prop]: inst.at("properties").items())
				{
					if (prop.contains("ref"))
					{
						const auto ref = script->nodeRefs.find(name);
						const std::uint64_t want = NodeIdOf(prop.at("ref").get<std::string>());
						if (ref == script->nodeRefs.end() || ref->second != want)
						{
							lines.push_back(std::format("- `{}`: {} does not reference `{}`", key, name, prop.at("ref").get<std::string>()));
						}
						continue;
					}
					const std::optional<ScriptPropertyValue> want = ValueOf(prop);
					const auto have = script->properties.find(name);
					if (!want)
					{
						continue;
					}
					if (have == script->properties.end() || have->second.type != want->type || have->second.i64 != want->i64 || have->second.str != want->str)
					{
						lines.push_back(std::format("- `{}`: {} = {} in the scene, {} in the extract", key, name,
						        have == script->properties.end() ? std::string("(unset)") : PropText(have->second), PropText(*want)));
					}
				}
				for (const auto& [name, node]: script->nodeRefs)
				{
					if (!inst.at("properties").contains(name))
					{
						lines.push_back(std::format("- `{}`: {} references an instance the extract does not link", key, name));
					}
				}
				for (const char* list: {"points", "path"})
				{
					const std::vector<glm::vec3> have = SavedPoints(rec, std::string(list) == "points" ? "Point " : "Path ");
					const json& want = inst.contains(list) ? inst.at(list) : json::array();
					bool same = have.size() == want.size();
					for (std::size_t i = 0; same && i < have.size(); ++i)
					{
						same = glm::length(have[i] - Vec3(want[i])) <= kPositionTolerance;
					}
					if (!same)
					{
						lines.push_back(std::format("- `{}`: {} differ ({} in the scene, {} in the extract)", key, list, have.size(), want.size()));
					}
				}
			}
			for (const sc::PrefabInstanceRecord& r: saved.prefabInstances)
			{
				if (!expected.contains(r.node))
				{
					lines.push_back(std::format("- extra instance {} ({}), not in the extract", r.name, r.prefabPath));
				}
			}
			return lines;
		}

		std::vector<std::string> StaticNames(const sc::SceneDescription& desc)
		{
			std::vector<std::string> paths = NamePaths(desc);
			std::ranges::sort(paths);
			return paths;
		}

		void DiffStatics(const sc::SceneDescription& fresh, const sc::SceneDescription& saved, std::vector<std::string>& lines)
		{
			// Scenery and collision are the extract's own models and refresh in place; only a
			// piece appearing or disappearing is drift.
			std::vector<std::string> want = StaticNames(fresh);
			sc::SceneDescription savedStatic;
			savedStatic.entities = saved.entities;
			std::vector<std::string> have = StaticNames(savedStatic);
			std::vector<std::string> missing;
			std::vector<std::string> extra;
			std::ranges::set_difference(want, have, std::back_inserter(missing));
			std::ranges::set_difference(have, want, std::back_inserter(extra));
			for (const std::string& m: missing)
			{
				lines.push_back("- static `" + m + "`: missing from the scene");
			}
			for (const std::string& e: extra)
			{
				lines.push_back("- static `" + e + "`: in the scene, not in the extract");
			}
		}
	} // namespace

	json ConvertLevel(ServiceContainer& services, const LevelConvertRequest& request)
	{
		auto* scenes = services.TryGet<SceneSubsystem>();
		auto* assets = services.TryGet<AssetManager>();
		auto* scripting = services.TryGet<app::scripting::CSharpScriptingSubsystem>();
		auto* sceneCtx = services.TryGet<app::scripting::SceneContext>();
		if (scenes == nullptr || assets == nullptr || scripting == nullptr || sceneCtx == nullptr)
		{
			return json{{"ok", false}, {"error", "editor subsystems unavailable (no scene, assets, C# host or scene context)"}};
		}
		auto* undo = services.TryGet<UndoStack>();
		if (undo != nullptr && undo->HasUnsavedChanges())
		{
			return json{{"ok", false}, {"error", "the open scene has unsaved edits - save or discard them first (the conversion replaces the live world)"}};
		}
		const auto& materials = assets->GetMaterialRegistry();
		const auto& textures = assets->GetTextureRegistry();
		World& world = scenes->GetWorld();
		const sc::ApplySceneDeps deps = sc::MakeApplySceneDeps(services);
		const std::string previousScene = scenes->GetCurrentScene();
		const bool writing = !request.report;

		// Whatever happens next, the editor ends on a scene loaded from disk, not on the
		// conversion's scratch world.
		struct Restore
		{
			SceneSubsystem& scenes;
			World& world;
			const sc::ApplySceneDeps& deps;
			UndoStack* undo;
			std::string scene;
			~Restore()
			{
				if (!scene.empty() && sc::SwitchScene(scene, world, deps, sc::OnLoadFailure::ClearWorld))
				{
					scenes.SetCurrentScene(scene);
				}
				if (undo != nullptr)
				{
					undo->Clear(); // its commands name the scratch world's entities
				}
			}
		};

		// Run the C# half in an emptied world. Its Scene.*/Entity.*/Assets.* exports dereference the
		// thread's active scene context, which nothing else publishes on the control thread.
		const std::string manifestText = [&]() -> std::string
		{
			sc::SceneDescription blank;
			sc::ReplaceScene(blank, world, deps);
			const app::scripting::ActiveContextScope scope(*sceneCtx);
			std::string error;
			if (!scripting->InvokeScriptCommand("TwinsanityConvert", error))
			{
				AE_ERROR(LogCategory::App, "twinsanity.convert: {}", error);
				return {};
			}
			auto text = io::FileSystem::ReadFileText(kManifestPath);
			return text ? *text : std::string{};
		}();
		const std::string worldName = "Beach";
		Restore restore{*scenes, world, deps, undo, previousScene.empty() ? worldName : previousScene};
		if (manifestText.empty())
		{
			return json{{"ok", false}, {"error", "the TwinsanityConvert command failed or wrote no manifest - see the console"}};
		}
		const json manifest = json::parse(manifestText, nullptr, false);
		if (manifest.is_discarded() || manifest.value("version", 0) != 1)
		{
			return json{{"ok", false}, {"error", std::string("unreadable manifest ") + kManifestPath}};
		}
		const json& worldSpec = manifest.at("world");
		const std::string worldScene = worldSpec.at("scene").get<std::string>();
		const auto worldFile = sc::ReadSceneFile(worldScene);
		if (!worldFile)
		{
			return json{{"ok", false}, {"error", "world scene '" + worldScene + "' not found - it supplies the environment, Level and Crash"}};
		}
		const auto wanted = [&](const std::string& scene) { return request.areas.empty() || std::ranges::find(request.areas, scene) != request.areas.end(); };

		json result{{"ok", true}, {"mode", writing ? "write" : "report"}, {"counts", manifest.at("counts")}};
		json prefabsWritten = json::array(), prefabsKept = json::array(), scenesWritten = json::array(), scenesKept = json::array();
		json drift = json::object();
		std::map<std::string, std::vector<std::string>> driftLines;

		// Prefabs: capture every template, keep the old guids, write (or diff).
		std::map<std::string, std::optional<sc::SceneDescription>> prefabs;
		for (const json& p: manifest.at("prefabs"))
		{
			const std::string name = p.at("name").get<std::string>();
			const Entity tmpl = TopLevel(world, p.at("template").get<std::string>());
			if (!tmpl.IsValid())
			{
				return json{{"ok", false}, {"error", "template '" + name + "' was not built - see the console"}};
			}
			if (ScriptEntry* script = ScriptOf(world, tmpl, p.at("script").get<std::string>()))
			{
				for (const auto& [prop, value]: p.at("properties").items())
				{
					if (const auto v = ValueOf(value))
					{
						script->properties[prop] = *v;
					}
				}
			}
			sc::SceneDescription fresh = sc::CapturePrefab(world, tmpl, materials, textures);
			ecs::DestroyHierarchy(world, tmpl);
			const std::optional<sc::SceneDescription> old = sc::ReadPrefabFile(name);
			RestampGuids(fresh, old);
			if (!writing)
			{
				if (!old)
				{
					driftLines["prefabs"].push_back("- prefab `" + name + "`: not written yet");
				}
				else if (sc::WriteToml(fresh, false) != sc::WriteToml(*old, false))
				{
					driftLines["prefabs"].push_back("- prefab `" + name + "`: differs from a fresh template");
				}
				prefabs[name] = old;
				continue;
			}
			if (old && !Listed(request.overwrite, name))
			{
				prefabsKept.push_back(name);
				prefabs[name] = old;
				continue;
			}
			if (!sc::SavePrefabFile(name, fresh))
			{
				return json{{"ok", false}, {"error", "could not write prefab " + name}};
			}
			prefabsWritten.push_back(name);
			prefabs[name] = sc::ReadPrefabFile(name); // exactly what instances diff against
		}

		// World-scene statics (the sky) and every area's statics, captured before any instance
		// exists so each area scene holds only its own.
		const auto captureStatics = [&](const json& names, const std::string& prefix)
		{
			std::vector<Entity> roots;
			for (const json& name: names)
			{
				if (const Entity e = TopLevel(world, name.get<std::string>()); e.IsValid())
				{
					roots.push_back(e);
				}
			}
			sc::SceneDescription statics = CaptureStatics(world, roots, prefix, materials, textures);
			for (const Entity e: roots)
			{
				ecs::DestroyHierarchy(world, e);
			}
			return statics;
		};
		const sc::SceneDescription worldStatics = captureStatics(worldSpec.at("entities"), worldScene + "#static");
		std::map<std::string, sc::SceneDescription> areaStatics;
		for (const json& area: manifest.at("areas"))
		{
			areaStatics[area.at("scene").get<std::string>()] = captureStatics(area.at("static"), area.at("chunk").get<std::string>() + "#static");
		}

		// One scene's instances made live, then captured as reference records in manifest order.
		const auto buildInstances = [&](const json& instances, std::vector<sc::PrefabInstanceRecord>& out) -> std::string
		{
			std::unordered_map<std::string, Entity> roots;
			std::vector<Entity> order;
			for (const json& inst: instances)
			{
				const std::string prefabName = inst.at("prefab").get<std::string>();
				const auto& prefab = prefabs[prefabName];
				if (!prefab)
				{
					return "prefab " + prefabName + " is missing";
				}
				const Entity root = PlaceInstance(world, deps, inst, *prefab);
				if (!root.IsValid())
				{
					return "could not instantiate " + prefabName;
				}
				roots[inst.at("key").get<std::string>()] = root;
				order.push_back(root);
			}
			for (std::size_t i = 0; i < order.size(); ++i)
			{
				ResolveRefs(world, instances[i], order[i], roots);
			}
			for (const Entity root: order)
			{
				out.push_back(sc::CapturePrefabInstance(world, root, materials, textures));
			}
			for (const Entity root: order)
			{
				ecs::DestroyHierarchy(world, root);
			}
			return {};
		};

		// An area scene shaped as the editor saves one (no [scene] name, CaptureScene order), with
		// the world's environment so an area opened alone looks as it does in the world.
		const auto sceneFrom = [&](const sc::SceneDescription& statics) -> sc::SceneDescription
		{
			sc::SceneDescription desc = statics;
			desc.kind = worldFile->kind;
			desc.features = worldFile->features;
			desc.environment = worldFile->environment;
			return desc;
		};

		for (const json& area: manifest.at("areas"))
		{
			const std::string sceneName = area.at("scene").get<std::string>();
			if (!wanted(sceneName))
			{
				continue;
			}
			const auto saved = sc::ReadSceneFile(sceneName);
			if (!writing)
			{
				std::vector<std::string>& lines = driftLines[sceneName];
				if (!saved)
				{
					lines.push_back("- scene not written yet");
					continue;
				}
				DiffStatics(areaStatics[sceneName], *saved, lines);
				const std::vector<std::string> diff = DiffScene(area.at("instances"), *saved, prefabs);
				lines.insert(lines.end(), diff.begin(), diff.end());
				continue;
			}
			if (saved && !Listed(request.overwrite, sceneName))
			{
				scenesKept.push_back(sceneName);
				continue;
			}
			sc::SceneDescription desc = sceneFrom(areaStatics[sceneName]);
			if (const std::string error = buildInstances(area.at("instances"), desc.prefabInstances); !error.empty())
			{
				return json{{"ok", false}, {"error", sceneName + ": " + error}};
			}
			SortByNode(desc.prefabInstances);
			if (!sc::SaveSceneFile(sceneName, desc))
			{
				return json{{"ok", false}, {"error", "could not write scene " + sceneName}};
			}
			scenesWritten.push_back(sceneName);
		}

		// The world scene: its own entities (Level, Crash, ...) and environment kept, the old bake
		// instance and any previous conversion output replaced, then the sky, the primary spawn and
		// the area includes.
		if (wanted(worldScene))
		{
			if (!writing)
			{
				std::vector<std::string>& lines = driftLines[worldScene];
				const std::vector<std::string> diff = DiffScene(worldSpec.at("instances"), *worldFile, prefabs);
				for (const std::string& line: diff)
				{
					if (!line.starts_with("- extra instance")) // the world scene's own instances are not the extract's
					{
						lines.push_back(line);
					}
				}
				std::vector<std::string> includes = worldSpec.at("includes").get<std::vector<std::string>>();
				if (worldFile->includes != includes)
				{
					lines.push_back("- includes differ from the extract's areas");
				}
			}
			else if (!Listed(request.overwrite, worldScene))
			{
				scenesKept.push_back(worldScene);
			}
			else
			{
				sc::SceneDescription desc = *worldFile;
				desc.name.clear(); // the editor's own save writes no [scene] name
				for (sc::EntityRecord& r: desc.entities)
				{
					if (r.hasTransform) // the editor saves transforms decomposed from the live matrix (224.82 -> -135.18)
					{
						DecomposeTRS(ComposeTransform(r.position, r.eulerDeg, r.scale), r.position, r.eulerDeg, r.scale);
					}
				}
				std::erase_if(desc.prefabInstances, [&](const sc::PrefabInstanceRecord& r)
				        {
					        if (r.prefabPath == "beach") // the retired level bake
					        {
						        return true;
					        }
					        for (const json& inst: worldSpec.at("instances"))
					        {
						        if (r.node == NodeIdOf(inst.at("key").get<std::string>()))
						        {
							        return true;
						        }
					        }
					        return false;
				        });
				for (const json& name: worldSpec.at("entities"))
				{
					RemoveTopLevel(desc, name.get<std::string>());
				}
				Append(desc, worldStatics);
				for (std::size_t i = 0; i < desc.entities.size(); ++i)
				{
					if (desc.entities[i].nodeId == 0) // hand-written entities (Level, Crash): pin the id the editor would mint
					{
						desc.entities[i].nodeId = NodeIdOf(worldScene + "#entity#" + desc.entities[i].name + "#" + std::to_string(i));
					}
				}
				std::vector<sc::PrefabInstanceRecord> placed;
				if (const std::string error = buildInstances(worldSpec.at("instances"), placed); !error.empty())
				{
					return json{{"ok", false}, {"error", worldScene + ": " + error}};
				}
				desc.prefabInstances.insert(desc.prefabInstances.end(), placed.begin(), placed.end());
				desc.includes = worldSpec.at("includes").get<std::vector<std::string>>();
				SortByNode(desc.prefabInstances);
				if (!sc::SaveSceneFile(worldScene, desc))
				{
					return json{{"ok", false}, {"error", "could not write scene " + worldScene}};
				}
				scenesWritten.push_back(worldScene);
			}
		}

		if (!writing)
		{
			int total = 0;
			for (const auto& [scene, lines]: driftLines)
			{
				std::string text = "# Twinsanity convert drift: " + scene + "\n\n";
				text += lines.empty() ? "No drift: the saved content matches the current extract.\n" : std::format("{} differences between the saved content and the current extract.\n\n", lines.size());
				for (const std::string& line: lines)
				{
					text += line + "\n";
				}
				const std::string path = std::string(kReportDir) + "drift-" + scene + ".md";
				(void)io::FileSystem::WriteFileText(path, text);
				drift[scene] = static_cast<int>(lines.size());
				total += static_cast<int>(lines.size());
			}
			result["drift"] = drift;
			result["driftTotal"] = total;
			result["reportDir"] = kReportDir;
			return result;
		}
		result["prefabsWritten"] = prefabsWritten.size();
		result["prefabsKept"] = prefabsKept;
		result["scenesWritten"] = scenesWritten;
		result["scenesKept"] = scenesKept;
		return result;
	}
} // namespace aether::editor::twinsanity
