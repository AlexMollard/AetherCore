using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Editor command, run once as a MIGRATION (docs/twinsanity-editor.md, "Level convert"): the C#
/// half of twinsanity.convert. It reads tw-extract's level JSON and builds, in the (empty) live
/// world:
///   - one TEMPLATE per catalogue family at the origin, named after its prefab (model, body
///     child, tags, descriptor script) - the C++ side captures each as assets/prefabs/tw_*.prefab;
///   - each area's static content (scenery, collision) under an "Area &lt;chunk&gt;" root;
///   - the one Sky (tag tw_sky) for the world scene;
/// and writes the manifest (ManifestPath) the C++ side turns into prefab instances, area scenes and
/// the world scene. Nothing here runs at play, and nothing reads the manifest but the command.
///
/// Classification is today's bake rule, instance by instance: a scripted object (its id is in the
/// chunk's scripts.json) always gets a descriptor; object 1 is wumpa (tag tw_wumpa + TwAgent,
/// DESIGN.md amendment A1); a crate kind is a TwCrate; a spawner a TwSpawner; a skipped instance only
/// survives as an empty TwAgent when scripted; anything else with a model is a TwActor.
/// </summary>
public static class TwinsanityConvert
{
	public const string ManifestPath = "project://.aether/convert/manifest.json";
	public const string WorldScene = "Beach";
	public const string SkyName = "Sky";
	private const string ModelFolder = "project://assets/models/objects/";

	// Chunk stem -> area scene, in the world scene's include order. Every chunk the hub walk reaches
	// must be listed; an unlisted one fails the conversion.
	private static readonly (string Chunk, string Scene)[] Areas =
	{
		("beach", "HubBeach"), ("huba", "HubA"), ("hubb", "HubB"), ("hubc", "HubC"), ("hubd", "HubD"),
		("pier", "Pier"), ("highpath", "HighPath"), ("bossarea", "BossArea"), ("alwayson", "AlwaysOn"),
	};

	private enum Recipe { Crate, Wumpa, Actor, Spawner, Agent }

	// The checked-in family catalogue: (recipe, family key) -> prefab name. Crates, wumpa and actors
	// are keyed by their model (relative to ModelFolder), because a model variant is its own prefab
	// (DESIGN.md R6); spawners and agents by NameKey, because their roots carry no model. A family
	// missing here fails the conversion loudly - add a row, never a fallback.
	private static readonly Dictionary<(Recipe, string), string> Catalogue = new()
	{
		{ (Recipe.Agent, "act_activatedcollision64x"), "tw_agent_activatedcollision64x" },
		{ (Recipe.Agent, "act_ambience_manager"), "tw_agent_ambience_manager" },
		{ (Recipe.Agent, "act_angry_skunk_cutscene_director"), "tw_agent_angry_skunk_cutscene_director" },
		{ (Recipe.Agent, "act_beach_aku_cutscene_director"), "tw_agent_beach_aku_cutscene_director" },
		{ (Recipe.Agent, "act_beach_training_cutscene_director"), "tw_agent_beach_training_cutscene_director" },
		{ (Recipe.Agent, "act_bosstrigger"), "tw_agent_bosstrigger" },
		{ (Recipe.Agent, "act_coco_creature"), "tw_agent_coco_creature" },
		{ (Recipe.Agent, "act_coop_roller_proxy_a"), "tw_agent_coop_roller_proxy_a" },
		{ (Recipe.Agent, "act_cortex"), "tw_agent_cortex" },
		{ (Recipe.Agent, "act_cortex_constraint"), "tw_agent_cortex_constraint" },
		{ (Recipe.Agent, "act_cortex_creature"), "tw_agent_cortex_creature" },
		{ (Recipe.Agent, "act_cortex_follow_manager"), "tw_agent_cortex_follow_manager" },
		{ (Recipe.Agent, "act_cortex_hoverboard"), "tw_agent_cortex_hoverboard" },
		{ (Recipe.Agent, "act_cortex_training_cutscene_assistant"), "tw_agent_cortex_training_cutscene_assistant" },
		{ (Recipe.Agent, "act_cortex_training_fake_crunch"), "tw_agent_cortex_training_fake_crunch" },
		{ (Recipe.Agent, "act_cortex_training_fake_dingodile"), "tw_agent_cortex_training_fake_dingodile" },
		{ (Recipe.Agent, "act_cortex_training_fake_kong"), "tw_agent_cortex_training_fake_kong" },
		{ (Recipe.Agent, "act_cortex_training_fake_noxide"), "tw_agent_cortex_training_fake_noxide" },
		{ (Recipe.Agent, "act_cortex_training_fake_pinstrip"), "tw_agent_cortex_training_fake_pinstrip" },
		{ (Recipe.Agent, "act_cortex_training_fake_pola"), "tw_agent_cortex_training_fake_pola" },
		{ (Recipe.Agent, "act_cortex_training_fake_ripperroo"), "tw_agent_cortex_training_fake_ripperroo" },
		{ (Recipe.Agent, "act_cortex_training_fake_tiny"), "tw_agent_cortex_training_fake_tiny" },
		{ (Recipe.Agent, "act_cortex_training_miniboss"), "tw_agent_cortex_training_miniboss" },
		{ (Recipe.Agent, "act_counter_relay"), "tw_agent_counter_relay" },
		{ (Recipe.Agent, "act_crash"), "tw_agent_crash" },
		{ (Recipe.Agent, "act_cutscene_extra"), "tw_agent_cutscene_extra" },
		{ (Recipe.Agent, "act_dj"), "tw_agent_dj" },
		{ (Recipe.Agent, "act_dummy_frontend_character"), "tw_agent_dummy_frontend_character" },
		{ (Recipe.Agent, "act_earth_emu_farmer"), "tw_agent_earth_emu_farmer" },
		{ (Recipe.Agent, "act_earth_hub_cutscene_director"), "tw_agent_earth_hub_cutscene_director" },
		{ (Recipe.Agent, "act_forcevolume_controller"), "tw_agent_forcevolume_controller" },
		{ (Recipe.Agent, "act_frontend_flyby_cutscene_director"), "tw_agent_frontend_flyby_cutscene_director" },
		{ (Recipe.Agent, "act_generic_fmv_player"), "tw_agent_generic_fmv_player" },
		{ (Recipe.Agent, "act_global_ambient_sound"), "tw_agent_global_ambient_sound" },
		{ (Recipe.Agent, "act_global_ambient_sound_beach"), "tw_agent_global_ambient_sound_beach" },
		{ (Recipe.Agent, "act_global_ambient_sound_bodssarea"), "tw_agent_global_ambient_sound_bodssarea" },
		{ (Recipe.Agent, "act_global_ambient_sound_highpath"), "tw_agent_global_ambient_sound_highpath" },
		{ (Recipe.Agent, "act_global_ambient_sound_hubc"), "tw_agent_global_ambient_sound_hubc" },
		{ (Recipe.Agent, "act_global_bat_darkpurple"), "tw_agent_global_bat_darkpurple" },
		{ (Recipe.Agent, "act_hub2_to_hub3_cutscene_director"), "tw_agent_hub2_to_hub3_cutscene_director" },
		{ (Recipe.Agent, "act_invisible_checkpoint_crate"), "tw_agent_invisible_checkpoint_crate" },
		{ (Recipe.Agent, "act_level_controller"), "tw_agent_level_controller" },
		{ (Recipe.Agent, "act_party_arena_director"), "tw_agent_party_arena_director" },
		{ (Recipe.Agent, "act_set_playermode_to_single"), "tw_agent_set_playermode_to_single" },
		{ (Recipe.Agent, "act_sound_spot_waterfall"), "tw_agent_sound_spot_waterfall" },
		{ (Recipe.Agent, "act_training_cutscene_director_highpath"), "tw_agent_training_cutscene_director_highpath" },
		{ (Recipe.Agent, "act_training_cutscene_director_huba_spin_cutscene"), "tw_agent_training_cutscene_director_huba_spin_cutscene" },
		{ (Recipe.Agent, "act_training_cutscene_director_hubb"), "tw_agent_training_cutscene_director_hubb" },
		{ (Recipe.Agent, "act_training_cutscene_skunk"), "tw_agent_training_cutscene_skunk" },
		{ (Recipe.Agent, "act_training_miniboss_arena_floor"), "tw_agent_training_miniboss_arena_floor" },
		{ (Recipe.Agent, "act_training_miniboss_central_altar"), "tw_agent_training_miniboss_central_altar" },
		{ (Recipe.Agent, "act_training_miniboss_stair_chunk"), "tw_agent_training_miniboss_stair_chunk" },
		{ (Recipe.Agent, "act_training_miniboss_stonedoor"), "tw_agent_training_miniboss_stonedoor" },
		{ (Recipe.Agent, "act_util_textmaster"), "tw_agent_util_textmaster" },
		{ (Recipe.Agent, "act_whackaworm_controller"), "tw_agent_whackaworm_controller" },
		{ (Recipe.Crate, "act_AKUAKUCRATE/act_AKUAKUCRATE_0.gltf"), "tw_crate_akuaku" },
		{ (Recipe.Crate, "AKUAKUCRATE/AKUAKUCRATE_0.gltf"), "tw_crate_akuaku_plain" },
		{ (Recipe.Crate, "BASICCRATE/BASICCRATE_0.gltf"), "tw_crate_basic" },
		{ (Recipe.Crate, "act_BASICCRATE24/act_BASICCRATE24_0.gltf"), "tw_crate_basic_act24" },
		{ (Recipe.Crate, "act_BASICCRATE45/act_BASICCRATE45_0.gltf"), "tw_crate_basic_act45" },
		{ (Recipe.Crate, "act_CHECKPOINTCRATE/act_CHECKPOINTCRATE_0.gltf"), "tw_crate_checkpoint" },
		{ (Recipe.Crate, "act_CHECKPOINTCRATE4/act_CHECKPOINTCRATE4_0.gltf"), "tw_crate_checkpoint_4" },
		{ (Recipe.Crate, "act_CHECKPOINTCRATE6/act_CHECKPOINTCRATE6_0.gltf"), "tw_crate_checkpoint_6" },
		{ (Recipe.Crate, "DETONATOR_CRATE/DETONATOR_CRATE_0.gltf"), "tw_crate_detonator" },
		{ (Recipe.Crate, "EXTRALIFECRATE/EXTRALIFECRATE_0.gltf"), "tw_crate_extralife" },
		{ (Recipe.Crate, "act_EXTRALIFECRATE/act_EXTRALIFECRATE_0.gltf"), "tw_crate_extralife_act" },
		{ (Recipe.Crate, "act_EXTRALIFECRATE1/act_EXTRALIFECRATE1_0.gltf"), "tw_crate_extralife_act1" },
		{ (Recipe.Crate, "IRONCRATE/IRONCRATE_0.gltf"), "tw_crate_iron" },
		{ (Recipe.Crate, "act_IRONCRATE22/act_IRONCRATE22_0.gltf"), "tw_crate_iron_act22" },
		{ (Recipe.Crate, "IRONSPRINGCRATE/IRONSPRINGCRATE_0.gltf"), "tw_crate_ironspring" },
		{ (Recipe.Crate, "act_LEVELCRATE/act_LEVELCRATE_0.gltf"), "tw_crate_level" },
		{ (Recipe.Crate, "act_LEVELCRATE1/act_LEVELCRATE1_0.gltf"), "tw_crate_level_1" },
		{ (Recipe.Crate, "act_MULTIPLEHITCRATE1/act_MULTIPLEHITCRATE1_0.gltf"), "tw_crate_multihit" },
		{ (Recipe.Crate, "NITROCRATE/NITROCRATE_0.gltf"), "tw_crate_nitro" },
		{ (Recipe.Crate, "act_NITROCRATE49/act_NITROCRATE49_0.gltf"), "tw_crate_nitro_act49" },
		{ (Recipe.Crate, "REINFORCEDWOODENCRATE/REINFORCEDWOODENCRATE_0.gltf"), "tw_crate_reinforced" },
		{ (Recipe.Crate, "SURPRISECRATE/SURPRISECRATE_0.gltf"), "tw_crate_surprise" },
		{ (Recipe.Crate, "TNTCRATE/TNTCRATE_0.gltf"), "tw_crate_tnt" },
		{ (Recipe.Crate, "act_TNTCRATE/act_TNTCRATE_0.gltf"), "tw_crate_tnt_act" },
		{ (Recipe.Crate, "act_TNTCRATE3/act_TNTCRATE3_0.gltf"), "tw_crate_tnt_act3" },
		{ (Recipe.Crate, "WOODENSPRINGCRATE/WOODENSPRINGCRATE_0.gltf"), "tw_crate_woodenspring" },
		{ (Recipe.Actor, "act_GLOBAL_BAT_DARKPURPLE10/act_GLOBAL_BAT_DARKPURPLE10.gltf"), "tw_critter_bat" },
		{ (Recipe.Actor, "act_BIRDCLUMP8/act_BIRDCLUMP8_0.gltf"), "tw_critter_birdclump" },
		{ (Recipe.Actor, "act_BIRDCLUMP6/act_BIRDCLUMP6_0.gltf"), "tw_critter_birdclump_6" },
		{ (Recipe.Actor, "act_BIRDCLUMP/act_BIRDCLUMP_0.gltf"), "tw_critter_birdclump_plain" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_BLUE/act_GLOBAL_BUTTERFLY_BLUE.gltf"), "tw_critter_butterfly_blue" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_BLUE1/act_GLOBAL_BUTTERFLY_BLUE1.gltf"), "tw_critter_butterfly_blue_1" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_BLUE2/act_GLOBAL_BUTTERFLY_BLUE2.gltf"), "tw_critter_butterfly_blue_2" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_BLUE3/act_GLOBAL_BUTTERFLY_BLUE3.gltf"), "tw_critter_butterfly_blue_3" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_BLUE4/act_GLOBAL_BUTTERFLY_BLUE4.gltf"), "tw_critter_butterfly_blue_4" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_GREEN/act_GLOBAL_BUTTERFLY_GREEN.gltf"), "tw_critter_butterfly_green" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_GREEN1/act_GLOBAL_BUTTERFLY_GREEN1.gltf"), "tw_critter_butterfly_green_1" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_GREEN2/act_GLOBAL_BUTTERFLY_GREEN2.gltf"), "tw_critter_butterfly_green_2" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_GREEN3/act_GLOBAL_BUTTERFLY_GREEN3.gltf"), "tw_critter_butterfly_green_3" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_GREEN4/act_GLOBAL_BUTTERFLY_GREEN4.gltf"), "tw_critter_butterfly_green_4" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_ORANGE/act_GLOBAL_BUTTERFLY_ORANGE.gltf"), "tw_critter_butterfly_orange" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_ORANGE1/act_GLOBAL_BUTTERFLY_ORANGE1.gltf"), "tw_critter_butterfly_orange_1" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_ORANGE2/act_GLOBAL_BUTTERFLY_ORANGE2.gltf"), "tw_critter_butterfly_orange_2" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_ORANGE3/act_GLOBAL_BUTTERFLY_ORANGE3.gltf"), "tw_critter_butterfly_orange_3" },
		{ (Recipe.Actor, "act_GLOBAL_BUTTERFLY_ORANGE4/act_GLOBAL_BUTTERFLY_ORANGE4.gltf"), "tw_critter_butterfly_orange_4" },
		{ (Recipe.Actor, "act_GLOBAL_CHICKEN1/act_GLOBAL_CHICKEN1.gltf"), "tw_critter_chicken" },
		{ (Recipe.Actor, "act_GLOBAL_CHICKEN17/act_GLOBAL_CHICKEN17.gltf"), "tw_critter_chicken_17" },
		{ (Recipe.Actor, "old_act_GLOBAL_CRAB4/old_act_GLOBAL_CRAB4.gltf"), "tw_critter_crab" },
		{ (Recipe.Actor, "act_GLOBAL_CRAB/act_GLOBAL_CRAB.gltf"), "tw_critter_crab_act" },
		{ (Recipe.Actor, "act_GLOBAL_MONKEY2/act_GLOBAL_MONKEY2.gltf"), "tw_critter_monkey" },
		{ (Recipe.Actor, "act_PIRANHAPLANT4/act_PIRANHAPLANT4.gltf"), "tw_critter_piranha" },
		{ (Recipe.Actor, "act_PIRANHAPLANT5/act_PIRANHAPLANT5.gltf"), "tw_critter_piranha_5" },
		{ (Recipe.Actor, "act_GLOBAL_SEAGULL12/act_GLOBAL_SEAGULL12.gltf"), "tw_critter_seagull" },
		{ (Recipe.Actor, "act_GLOBAL_SEAGULL3/act_GLOBAL_SEAGULL3.gltf"), "tw_critter_seagull_3" },
		{ (Recipe.Actor, "act_EARTH_TRIBESMAN_SHIELDBEARER1/act_EARTH_TRIBESMAN_SHIELDBEARER1.gltf"), "tw_critter_shieldbearer" },
		{ (Recipe.Actor, "act_EARTH_TRIBESMAN_SHIELDBEARER2/act_EARTH_TRIBESMAN_SHIELDBEARER2.gltf"), "tw_critter_shieldbearer_2" },
		{ (Recipe.Actor, "act_GLOBAL_SKUNK6/act_GLOBAL_SKUNK6.gltf"), "tw_critter_skunk" },
		{ (Recipe.Actor, "act_GLOBAL_SKUNK3/act_GLOBAL_SKUNK3.gltf"), "tw_critter_skunk_3" },
		{ (Recipe.Actor, "act_GLOBAL_SKUNK/act_GLOBAL_SKUNK.gltf"), "tw_critter_skunk_plain" },
		{ (Recipe.Actor, "act_EARTH_TRIBESMAN3/act_EARTH_TRIBESMAN3.gltf"), "tw_critter_tribesman" },
		{ (Recipe.Actor, "act_WHACKAWORM_WORM/act_WHACKAWORM_WORM.gltf"), "tw_critter_whackaworm" },
		{ (Recipe.Actor, "act_EARTH_WORM9/act_EARTH_WORM9.gltf"), "tw_critter_worm" },
		{ (Recipe.Actor, "act_EARTH_WORM/act_EARTH_WORM.gltf"), "tw_critter_worm_plain" },
		{ (Recipe.Actor, "GEM_BLUE/GEM_BLUE.gltf"), "tw_gem_blue" },
		{ (Recipe.Actor, "act_GEM_BLUE/act_GEM_BLUE.gltf"), "tw_gem_blue_act" },
		{ (Recipe.Actor, "act_GEM_CLEAR/act_GEM_CLEAR.gltf"), "tw_gem_clear" },
		{ (Recipe.Actor, "GEM_GREEN/GEM_GREEN.gltf"), "tw_gem_green" },
		{ (Recipe.Actor, "GEM_PURPLE/GEM_PURPLE.gltf"), "tw_gem_purple" },
		{ (Recipe.Actor, "GEM_RED/GEM_RED.gltf"), "tw_gem_red" },
		{ (Recipe.Actor, "act_GEM_RED/act_GEM_RED.gltf"), "tw_gem_red_act" },
		{ (Recipe.Actor, "GEM_YELLOW/GEM_YELLOW.gltf"), "tw_gem_yellow" },
		{ (Recipe.Actor, "act_POWER_CRYSTAL/act_POWER_CRYSTAL.gltf"), "tw_pickup_powercrystal" },
		{ (Recipe.Actor, "act_NATIVE_TRANSPORT_BOAT/act_NATIVE_TRANSPORT_BOAT.gltf"), "tw_prop_boat" },
		{ (Recipe.Actor, "act_NATIVE_TRANSPORT_BOAT3/act_NATIVE_TRANSPORT_BOAT3.gltf"), "tw_prop_boat_3" },
		{ (Recipe.Actor, "act_TRAINING_CAVE_BLOCKER/act_TRAINING_CAVE_BLOCKER.gltf"), "tw_prop_caveblocker" },
		{ (Recipe.Actor, "CEILINGCHICHIGRASS/CEILINGCHICHIGRASS.gltf"), "tw_prop_ceilinggrass" },
		{ (Recipe.Actor, "act_TRAINING_FALLING_LOG1/act_TRAINING_FALLING_LOG1.gltf"), "tw_prop_fallinglog" },
		{ (Recipe.Actor, "act_TRAINING_FALLING_LOG4/act_TRAINING_FALLING_LOG4.gltf"), "tw_prop_fallinglog_4" },
		{ (Recipe.Actor, "act_EARTH_FISHFOUNTAIN_BLUE/act_EARTH_FISHFOUNTAIN_BLUE.gltf"), "tw_prop_fishfountain_blue" },
		{ (Recipe.Actor, "act_EARTH_FISHFOUNTAIN_PINK/act_EARTH_FISHFOUNTAIN_PINK.gltf"), "tw_prop_fishfountain_pink" },
		{ (Recipe.Actor, "act_TRAINING_EXPLODING_IDOL_HEAD2/act_TRAINING_EXPLODING_IDOL_HEAD2.gltf"), "tw_prop_idolhead" },
		{ (Recipe.Actor, "act_TRAINING_EXPLODING_IDOL_HEAD7/act_TRAINING_EXPLODING_IDOL_HEAD7.gltf"), "tw_prop_idolhead_7" },
		{ (Recipe.Actor, "act_TRAINING_PATH_PLATFORM2/act_TRAINING_PATH_PLATFORM2.gltf"), "tw_prop_pathplatform" },
		{ (Recipe.Actor, "act_SEAPILLAR1/act_SEAPILLAR1.gltf"), "tw_prop_seapillar" },
		{ (Recipe.Actor, "old_act_TRAINING_SWINGING_LOG/old_act_TRAINING_SWINGING_LOG.gltf"), "tw_prop_swinginglog" },
		{ (Recipe.Actor, "act_TRAINING_SWINGING_LOG11/act_TRAINING_SWINGING_LOG11.gltf"), "tw_prop_swinginglog_act11" },
		{ (Recipe.Actor, "act_TIKI_MON/act_TIKI_MON_0.gltf"), "tw_prop_tikimon" },
		{ (Recipe.Actor, "old_act_WUMPA_TREE7/old_act_WUMPA_TREE7.gltf"), "tw_prop_wumpatree" },
		{ (Recipe.Actor, "act_WUMPA_TREE/act_WUMPA_TREE.gltf"), "tw_prop_wumpatree_act" },
		{ (Recipe.Actor, "act_GLOBAL_BARREL/act_GLOBAL_BARREL.gltf"), "tw_push_barrel" },
		{ (Recipe.Actor, "act_BEACH_BALL1/act_BEACH_BALL1_0.gltf"), "tw_push_beachball" },
		{ (Recipe.Actor, "act_GLOBAL_BOMB/act_GLOBAL_BOMB_0.gltf"), "tw_push_bomb" },
		{ (Recipe.Actor, "act_RIGID_CANNON/act_RIGID_CANNON.gltf"), "tw_push_cannon" },
		{ (Recipe.Actor, "act_RIGID_CANNON1/act_RIGID_CANNON1.gltf"), "tw_push_cannon_1" },
		{ (Recipe.Actor, "act_GLOBAL_HAYBALE6/act_GLOBAL_HAYBALE6.gltf"), "tw_push_haybale" },
		{ (Recipe.Actor, "act_GLOBAL_HAYBALE4/act_GLOBAL_HAYBALE4.gltf"), "tw_push_haybale_4" },
		{ (Recipe.Actor, "act_MONKEY_ROCK/act_MONKEY_ROCK.gltf"), "tw_push_monkeyrock" },
		{ (Recipe.Actor, "act_WUMPA_NUT/act_WUMPA_NUT.gltf"), "tw_push_nut" },
		{ (Recipe.Actor, "act_EARTH_NATIVE_SLEDGE/act_EARTH_NATIVE_SLEDGE.gltf"), "tw_sled" },
		{ (Recipe.Actor, "act_EARTH_NATIVE_SLEDGE1/act_EARTH_NATIVE_SLEDGE1.gltf"), "tw_sled_1" },
		{ (Recipe.Spawner, "act_creature_spawner"), "tw_spawner_creature" },
		{ (Recipe.Spawner, "act_util_ecology_manager"), "tw_spawner_ecology" },
		{ (Recipe.Spawner, "act_parrot_spawner"), "tw_spawner_parrot" },
		{ (Recipe.Wumpa, "REDWUMPA/REDWUMPA.gltf"), "tw_wumpa" },
		{ (Recipe.Wumpa, "act_REDWUMPA102/act_REDWUMPA102.gltf"), "tw_wumpa_act102" },
		{ (Recipe.Wumpa, "act_REDWUMPA106/act_REDWUMPA106.gltf"), "tw_wumpa_act106" },
		{ (Recipe.Wumpa, "act_REDWUMPA134/act_REDWUMPA134.gltf"), "tw_wumpa_act134" },
	};

	public const string TriggerPrefab = "tw_trigger";
	public const string SpawnPrefab = "tw_spawn";

	// One disc instance the manifest places.
	private sealed class Placed
	{
		public string Key = "", Prefab = "", Name = "", Script = "";
		public Vector3 Position, Euler, Scale = Vector3.One;
		public readonly List<(string Name, string Type, object Value)> Properties = new();
		public readonly List<(string Name, string Key)> Refs = new();
		public Vector3[] Points = Array.Empty<Vector3>(), Path = Array.Empty<Vector3>();
		public CrateKind? Crate; // checkpoint pick
	}

	// One prefab template: its root entity (named after the prefab) plus the family default properties.
	private sealed class Template
	{
		public string Name = "", Script = "";
		public readonly List<(string Name, string Type, object Value)> Properties = new();
	}

	public static void Run()
	{
		var cfg = new TwinsanityLevel(); // the script's property defaults are the conversion's input paths
		string levelPath = cfg.LevelPath;
		string levelFolder = levelPath[..(levelPath.LastIndexOf('/') + 1)];

		var objectModels = new Dictionary<int, string>();
		string? objects = Assets.ReadText(cfg.ObjectsPath) ?? throw new InvalidOperationException($"{cfg.ObjectsPath} not extracted");
		using (JsonDocument table = JsonDocument.Parse(objects))
		{
			foreach (JsonProperty row in table.RootElement.EnumerateObject())
			{
				if (row.Value.TryGetProperty("model", out JsonElement model))
				{
					objectModels[int.Parse(row.Name, CultureInfo.InvariantCulture)] = model.GetString()!;
				}
			}
		}

		var cutscenes = new TwinsanityCutscenes();
		var templates = new Dictionary<string, Template>(StringComparer.Ordinal);
		var areas = new List<(string Chunk, string Scene, List<Placed> Instances)>();
		var crates = new List<Placed>();
		Placed? spawn = null;
		int nCrates = 0, nWumpa = 0, nActors = 0, nSpawners = 0, nAgents = 0, nTriggers = 0, unresolved = 0;

		// The hub walk: every seamless-neighbour chunk inside the start chunk's folder, breadth-first,
		// exactly as TwinsanityLevel.BuildFromJson and the old bake walked it.
		var queue = new Queue<(string Path, Matrix4x4 Transform)>();
		var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		queue.Enqueue((levelPath, Matrix4x4.Identity));
		while (queue.Count > 0)
		{
			(string path, Matrix4x4 transform) = queue.Dequeue();
			if (!visited.Add(path))
			{
				continue;
			}
			string text = Assets.ReadText(path) ?? throw new InvalidOperationException($"{path} not extracted");
			using JsonDocument doc = JsonDocument.Parse(text);
			JsonElement level = doc.RootElement;
			string chunk = Path.GetFileNameWithoutExtension(Path.GetFileNameWithoutExtension(path)).ToLowerInvariant();
			string scene = Array.Find(Areas, a => a.Chunk == chunk).Scene
				?? throw new InvalidOperationException($"chunk '{chunk}' ({path}) has no area scene in TwinsanityConvert.Areas");
			bool start = path == levelPath;
			HashSet<int> scripted = cutscenes.CutsceneObjectIds(path);
			var placed = new List<Placed>();
			areas.Add((chunk, scene, placed));

			SpawnStatics(level, chunk, transform);
			if (start)
			{
				Entity sky = SpawnModel(SkyName, level.GetProperty("sky").GetString()!, Vector3.Zero, Vector3.Zero);
				sky.Scale = new Vector3(cfg.SkyScale);
				for (int i = 0; i < sky.ChildCount; i++)
				{
					MeshRenderer.SetCastShadows(sky.GetChild(i), false);
					// The dome is camera-centred and swallows every viewport pick otherwise.
					sky.GetChild(i).Component("Not Pickable").Add();
				}
				sky.Component("Not Pickable").Add();
				Tag(sky, "tw_sky");
				if (level.TryGetProperty("spawn", out JsonElement sp))
				{
					spawn = SpawnOf(sp, chunk, transform, templates);
				}
			}

			foreach (JsonElement instance in level.GetProperty("instances").EnumerateArray())
			{
				int objectId = instance.GetProperty("object").GetInt32();
				string? model = instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : objectModels.GetValueOrDefault(objectId);
				string objectName = TwinsanityActors.InstanceName(instance, model);
				string nameKey = TwinsanityActors.NameKeyOf(objectName);
				bool isScripted = scripted.Contains(objectId);
				CrateKind? kind = objectId == 1 ? null : TwinsanityLevel.KindFor(objectId, model);
				int spawnerRole = TwinsanityActors.SpawnerRoleFor(objectName);

				Recipe? recipe = null;
				if (objectId == 1)
				{
					recipe = model != null ? Recipe.Wumpa : null;
				}
				else if (kind != null)
				{
					recipe = model != null ? Recipe.Crate : null;
				}
				else if (spawnerRole == TwinsanityActors.SpawnerParrot
					|| (spawnerRole == TwinsanityActors.SpawnerCreature && instance.TryGetProperty("links", out _)))
				{
					recipe = Recipe.Spawner;
				}
				else if (!TwinsanityActors.Skipped(objectName) && model != null)
				{
					recipe = Recipe.Actor;
				}
				if (recipe == null && isScripted)
				{
					recipe = Recipe.Agent; // a cutscene agent and nothing else: an empty root
				}
				if (recipe == null)
				{
					continue; // not built today either
				}

				string familyKey = recipe is Recipe.Agent or Recipe.Spawner ? nameKey : RelativeModel(model!);
				if (!Catalogue.TryGetValue((recipe.Value, familyKey), out string? prefab))
				{
					throw new InvalidOperationException($"no catalogue row for ({recipe}, \"{familyKey}\") - {objectName} in {chunk}; add it to TwinsanityConvert.Catalogue");
				}
				string script = recipe switch
				{
					Recipe.Crate => "TwCrate",
					Recipe.Actor => "TwActor",
					Recipe.Spawner => "TwSpawner",
					_ => "TwAgent",
				};

				var p = new Placed
				{
					Key = $"{chunk}#{instance.GetProperty("layer").GetInt32()}#{instance.GetProperty("id").GetInt32()}",
					Prefab = prefab,
					Name = objectName,
					Script = script,
					Position = Vector3.Transform(Vec(instance.GetProperty("position")), transform),
					Euler = TwinsanityLevel.EulerOf(TwinsanityLevel.SysRotation(Vec(instance.GetProperty("euler"))) * transform),
					Crate = recipe == Recipe.Crate ? kind : null,
				};
				if (recipe == Recipe.Wumpa)
				{
					p.Euler = new Vector3(0.0f, p.Euler.Y, 0.0f); // yaw only, as the bake and TwinsanityWumpa place it
				}
				DescriptorProperties(p, instance, chunk, objectId, objectName, model, kind);
				p.Points = WorldPoints(instance, "points", transform);
				p.Path = WorldPoints(instance, "path", transform);
				placed.Add(p);

				if (!templates.ContainsKey(prefab))
				{
					templates[prefab] = SpawnTemplate(prefab, recipe.Value, script, objectId, objectName, model, kind);
				}

				switch (recipe)
				{
					case Recipe.Crate: nCrates++; crates.Add(p); break;
					case Recipe.Wumpa: nWumpa++; break;
					case Recipe.Actor: nActors++; break;
					case Recipe.Spawner: nSpawners++; break;
				}
				if (isScripted)
				{
					nAgents++;
				}
			}

			if (level.TryGetProperty("triggers", out JsonElement triggers))
			{
				foreach (JsonElement t in triggers.EnumerateArray())
				{
					placed.Add(TriggerOf(t, chunk, transform, templates));
					nTriggers++;
				}
			}

			// Links and targets name instances in the same chunk and layer; a target nothing was built
			// for (not built today either) stays unset and is reported.
			var keys = new HashSet<string>(placed.Select(q => q.Key), StringComparer.Ordinal);
			foreach (Placed q in placed)
			{
				for (int i = q.Refs.Count - 1; i >= 0; i--)
				{
					if (!keys.Contains(q.Refs[i].Key))
					{
						Log.Warn($"[TwinsanityConvert] {q.Key} ({q.Name}) {q.Refs[i].Name} -> {q.Refs[i].Key}: nothing is built for the target - left unset");
						q.Refs.RemoveAt(i);
						unresolved++;
					}
				}
			}

			if (level.TryGetProperty("links", out JsonElement links))
			{
				foreach (JsonElement link in links.EnumerateArray())
				{
					string next = link.GetProperty("chunk").GetString()!;
					bool neighbour = link.TryGetProperty("flags", out JsonElement flags) && (flags.GetUInt32() & 0xFF) == 1;
					if (neighbour && next.StartsWith(levelFolder, StringComparison.OrdinalIgnoreCase))
					{
						queue.Enqueue((next, ChunkTransform(link) * transform));
					}
				}
			}
		}

		// StartOpen: the checkpoint OpenStartCheckpoint picked at play - the nearest to the spawn
		// (first in walk order on a tie, as MinBy).
		if (spawn != null)
		{
			Placed? first = crates.Where(c => c.Crate == CrateKind.Checkpoint)
				.MinBy(c => Vector3.DistanceSquared(c.Position, spawn.Position));
			first?.Properties.Add(("StartOpen", "bool", true));
		}

		string manifest = WriteManifest(templates.Values.OrderBy(t => t.Name, StringComparer.Ordinal).ToList(), areas, spawn,
			(nCrates, nWumpa, nActors, nSpawners, nAgents, nTriggers, unresolved));
		if (!Assets.WriteText(ManifestPath, manifest))
		{
			throw new InvalidOperationException($"could not write {ManifestPath}");
		}
		Log.Info($"[TwinsanityConvert] {templates.Count} prefab templates, {areas.Count} areas: {nCrates} crates, {nWumpa} wumpa, {nActors} actors, "
			+ $"{nSpawners} spawners, {nAgents} cutscene agents, {nTriggers} triggers, {unresolved} unresolved links -> {ManifestPath}");
	}

	// ---- templates ------------------------------------------------------------------------------

	// The family's prefab, built at the origin in today's baked layout (TwinsanityBake): the model
	// root, a crate's "<Kind> Crate Body" box child, wumpa without shadows and with its tag, the
	// actor's attached parts named. Descriptor defaults are the family's own data, so a placed copy
	// (Playground) behaves as its family with no per-instance edits.
	private static Template SpawnTemplate(string prefab, Recipe recipe, string script, int objectId, string objectName, string? model, CrateKind? kind)
	{
		Entity root;
		switch (recipe)
		{
			case Recipe.Crate:
				root = SpawnModel(prefab, model!, Vector3.Zero, Vector3.Zero);
				Entity body = World.Create();
				body.Name = kind!.Value + " Crate Body";
				body.AddTransform();
				body.SetParent(root);
				body.Position = new Vector3(0.0f, 0.5f, 0.0f);
				Physics.AddBoxBody(body, new Vector3(0.5f, 0.5f, 0.5f), dynamic: false);
				break;
			case Recipe.Wumpa:
				root = SpawnModel(prefab, model!, Vector3.Zero, Vector3.Zero);
				NoShadows(root);
				Tag(root, "tw_wumpa");
				break;
			case Recipe.Actor:
				root = SpawnModel(prefab, model!, Vector3.Zero, Vector3.Zero);
				TwinsanityActors.NameAttachedParts(root, model);
				if (TwinsanityActors.NameKeyOf(objectName).StartsWith("act_redwumpa", StringComparison.Ordinal))
				{
					NoShadows(root);
				}
				break;
			default:
				root = Empty(prefab);
				break;
		}
		root.AddScript(script);
		var t = new Template { Name = prefab, Script = script };
		t.Properties.Add(("ObjectId", "int", objectId));
		t.Properties.Add(("ObjectName", "string", objectName));
		if (model != null)
		{
			t.Properties.Add(("Model", "string", model));
		}
		if (kind != null)
		{
			t.Properties.Add(("Kind", "enum", (int)kind.Value));
		}
		return t;
	}

	private static Placed TriggerOf(JsonElement t, string chunk, Matrix4x4 transform, Dictionary<string, Template> templates)
	{
		int layer = t.GetProperty("layer").GetInt32();
		int id = t.GetProperty("id").GetInt32();
		JsonElement q = t.GetProperty("rotation");
		var rotation = Quaternion.Normalize(new Quaternion(q[0].GetSingle(), q[1].GetSingle(), q[2].GetSingle(), q[3].GetSingle()));
		var p = new Placed
		{
			Key = $"{chunk}#trig#{layer}#{id}",
			Prefab = TriggerPrefab,
			Name = "Trigger",
			Script = "TwTrigger",
			Position = Vector3.Transform(Vec(t.GetProperty("center")), transform),
			// DESIGN.md amendment A1: the disc quaternion, chunk-local as the cutscene system applies it.
			Euler = TwinsanityLevel.EulerOf(Matrix4x4.CreateFromQuaternion(rotation)),
			Scale = Vec(t.GetProperty("extents")),
		};
		p.Properties.Add(("Area", "string", chunk));
		p.Properties.Add(("Layer", "int", layer));
		p.Properties.Add(("Id", "int", id));
		p.Properties.Add(("Header", "int", t.GetProperty("header").GetInt32()));
		p.Properties.Add(("Args", "string", string.Join(",", t.GetProperty("args").EnumerateArray().Select(a => a.GetInt32().ToString(CultureInfo.InvariantCulture)))));
		int n = 0;
		foreach (JsonElement target in t.GetProperty("targets").EnumerateArray())
		{
			if (n == 10)
			{
				throw new InvalidOperationException($"trigger {p.Key} has more than 10 targets");
			}
			p.Refs.Add(($"Target{n++}", $"{chunk}#{layer}#{target.GetInt32()}"));
		}
		if (!templates.ContainsKey(TriggerPrefab))
		{
			Empty(TriggerPrefab).AddScript("TwTrigger");
			templates[TriggerPrefab] = new Template { Name = TriggerPrefab, Script = "TwTrigger" };
		}
		return p;
	}

	private static Placed SpawnOf(JsonElement spawn, string chunk, Matrix4x4 transform, Dictionary<string, Template> templates)
	{
		string floats = spawn.TryGetProperty("floats", out JsonElement fl) ? FloatCsv(fl) : "";
		// The facing TwinsanityLevel has always derived: the instance yaw turned to the camera yaw.
		float facing = TwinsanityLevel.EulerOf(TwinsanityLevel.SysRotation(Vec(spawn.GetProperty("euler"))) * transform).Y + 180.0f;
		var p = new Placed
		{
			Key = $"{chunk}#spawn",
			Prefab = SpawnPrefab,
			Name = "Spawn",
			Script = "TwSpawn",
			Position = Vector3.Transform(Vec(spawn.GetProperty("position")), transform),
			Euler = new Vector3(0.0f, facing, 0.0f),
		};
		p.Properties.Add(("Primary", "bool", true));
		p.Properties.Add(("Floats", "string", floats));
		Empty(SpawnPrefab).AddScript("TwSpawn");
		var template = new Template { Name = SpawnPrefab, Script = "TwSpawn" };
		template.Properties.Add(("Floats", "string", floats)); // any placed spawn tunes Crash as the beach does
		templates[SpawnPrefab] = template;
		return p;
	}

	private static void DescriptorProperties(Placed p, JsonElement instance, string chunk, int objectId, string objectName, string? model, CrateKind? kind)
	{
		p.Properties.Add(("Area", "string", chunk));
		p.Properties.Add(("Layer", "int", instance.GetProperty("layer").GetInt32()));
		p.Properties.Add(("Id", "int", instance.GetProperty("id").GetInt32()));
		p.Properties.Add(("ObjectId", "int", objectId));
		p.Properties.Add(("ObjectName", "string", objectName));
		if (model != null)
		{
			p.Properties.Add(("Model", "string", model));
		}
		if (instance.TryGetProperty("subtype", out JsonElement st))
		{
			p.Properties.Add(("Subtype", "int", unchecked((int)st.GetUInt32())));
		}
		if (instance.TryGetProperty("flags", out JsonElement flags))
		{
			p.Properties.Add(("Flags", "int", unchecked((int)flags.GetUInt32())));
		}
		if (instance.TryGetProperty("floats", out JsonElement floats) && floats.GetArrayLength() > 0)
		{
			p.Properties.Add(("Floats", "string", FloatCsv(floats)));
		}
		if (instance.TryGetProperty("params", out JsonElement ps) && ps.GetArrayLength() > 0)
		{
			p.Properties.Add(("Params", "string", string.Join(",", ps.EnumerateArray().Select(v => v.GetInt32().ToString(CultureInfo.InvariantCulture)))));
		}
		if (kind != null)
		{
			p.Properties.Add(("Kind", "enum", (int)kind.Value));
		}
		if (instance.TryGetProperty("links", out JsonElement links))
		{
			int layer = instance.GetProperty("layer").GetInt32();
			int n = 0;
			foreach (JsonElement link in links.EnumerateArray())
			{
				if (n == 10)
				{
					throw new InvalidOperationException($"{p.Key} ({objectName}) has more than 10 links");
				}
				p.Refs.Add(($"Link{n++}", $"{chunk}#{layer}#{link.GetInt32()}"));
			}
		}
	}

	// ---- static area content --------------------------------------------------------------------

	// "Area <chunk>": the chunk's scenery models and its collision pieces, as the bake built them.
	// Collision keeps the name "Collision" (the camera and cutscene ground probes test it) and carries
	// the tags the binder reads instead of the old marker.
	private static void SpawnStatics(JsonElement level, string chunk, Matrix4x4 transform)
	{
		Entity area = Empty("Area " + chunk);
		Vector3 origin = transform.Translation;
		Vector3 euler = TwinsanityLevel.EulerOf(transform);
		int index = 0;
		foreach (JsonElement sceneryPath in level.GetProperty("scenery").EnumerateArray())
		{
			string path = sceneryPath.GetString()!;
			if (Assets.List(path).Length > 0) // listed by naming convention; not every chunk has dynamic scenery
			{
				SpawnModel(index == 0 ? "Scenery " + chunk : "Scenery " + chunk + " dynamic", path, origin, euler).SetParent(area);
			}
			index++;
		}
		foreach (JsonElement piece in level.GetProperty("collision").EnumerateArray())
		{
			string path = piece.GetProperty("path").GetString()!;
			Entity e = SpawnModel("Collision", path, origin, euler);
			e.SetParent(area);
			Physics.AddMeshBody(e, path);
			// Invisible gameplay hulls: viewport clicks reach the visible world instead.
			e.Component("Not Pickable").Add();
			for (int i = 0; i < e.ChildCount; i++)
			{
				e.GetChild(i).Component("Not Pickable").Add();
			}
			Tag(e, "tw_collision");
			if (piece.GetProperty("deadly").GetBoolean())
			{
				Tag(e, "tw_deadly");
				if (piece.TryGetProperty("drown", out JsonElement drown) && drown.GetBoolean())
				{
					Tag(e, "tw_drown");
				}
			}
		}
	}

	// ---- manifest (DESIGN.md §2.8, contract B) ---------------------------------------------------

	private static string WriteManifest(List<Template> templates, List<(string Chunk, string Scene, List<Placed> Instances)> areas, Placed? spawn,
		(int Crates, int Wumpa, int Actors, int Spawners, int Agents, int Triggers, int Unresolved) counts)
	{
		using var stream = new MemoryStream();
		using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
		{
			w.WriteStartObject();
			w.WriteNumber("version", 1);
			w.WriteStartObject("counts");
			w.WriteNumber("crates", counts.Crates);
			w.WriteNumber("wumpa", counts.Wumpa);
			w.WriteNumber("actors", counts.Actors);
			w.WriteNumber("spawners", counts.Spawners);
			w.WriteNumber("agents", counts.Agents);
			w.WriteNumber("triggers", counts.Triggers);
			w.WriteNumber("unresolvedLinks", counts.Unresolved);
			w.WriteEndObject();

			w.WriteStartArray("prefabs");
			foreach (Template t in templates)
			{
				w.WriteStartObject();
				w.WriteString("name", t.Name);
				w.WriteString("template", t.Name);
				w.WriteString("script", t.Script);
				WriteProperties(w, t.Properties, null);
				w.WriteEndObject();
			}
			w.WriteEndArray();

			w.WriteStartArray("areas");
			foreach ((string chunk, string scene, List<Placed> instances) in areas.OrderBy(a => Array.FindIndex(Areas, x => x.Chunk == a.Chunk)))
			{
				w.WriteStartObject();
				w.WriteString("scene", scene);
				w.WriteString("chunk", chunk);
				w.WriteStartArray("static");
				w.WriteStringValue("Area " + chunk);
				w.WriteEndArray();
				w.WriteStartArray("instances");
				foreach (Placed p in instances)
				{
					WriteInstance(w, p);
				}
				w.WriteEndArray();
				w.WriteEndObject();
			}
			w.WriteEndArray();

			w.WriteStartObject("world");
			w.WriteString("scene", WorldScene);
			w.WriteStartArray("entities");
			w.WriteStringValue(SkyName);
			w.WriteEndArray();
			w.WriteStartArray("instances");
			if (spawn != null)
			{
				WriteInstance(w, spawn);
			}
			w.WriteEndArray();
			w.WriteStartArray("includes");
			foreach ((string chunk, string scene) in Areas)
			{
				if (areas.Exists(a => a.Chunk == chunk))
				{
					w.WriteStringValue(scene);
				}
			}
			w.WriteEndArray();
			w.WriteEndObject();
			w.WriteEndObject();
		}
		return Encoding.UTF8.GetString(stream.ToArray());
	}

	private static void WriteInstance(Utf8JsonWriter w, Placed p)
	{
		w.WriteStartObject();
		w.WriteString("key", p.Key);
		w.WriteString("prefab", p.Prefab);
		w.WriteString("name", p.Name);
		WriteVec(w, "position", p.Position);
		WriteVec(w, "euler", p.Euler);
		WriteVec(w, "scale", p.Scale);
		w.WriteString("script", p.Script);
		WriteProperties(w, p.Properties, p.Refs);
		WritePoints(w, "points", p.Points);
		WritePoints(w, "path", p.Path);
		w.WriteEndObject();
	}

	private static void WriteProperties(Utf8JsonWriter w, List<(string Name, string Type, object Value)> properties, List<(string Name, string Key)>? refs)
	{
		w.WriteStartObject("properties");
		foreach ((string name, string type, object value) in properties)
		{
			w.WriteStartObject(name);
			w.WriteString("t", type);
			switch (value)
			{
				case string s: w.WriteString("v", s); break;
				case bool b: w.WriteBoolean("v", b); break;
				case int i: w.WriteNumber("v", i); break;
				default: throw new InvalidOperationException($"property {name}: unsupported value {value.GetType()}");
			}
			w.WriteEndObject();
		}
		foreach ((string name, string key) in refs ?? new List<(string, string)>())
		{
			w.WriteStartObject(name);
			w.WriteString("t", "entity");
			w.WriteString("ref", key);
			w.WriteEndObject();
		}
		w.WriteEndObject();
	}

	private static void WritePoints(Utf8JsonWriter w, string name, Vector3[] points)
	{
		if (points.Length == 0)
		{
			return;
		}
		w.WriteStartArray(name);
		foreach (Vector3 v in points)
		{
			WriteVecValue(w, v);
		}
		w.WriteEndArray();
	}

	private static void WriteVec(Utf8JsonWriter w, string name, Vector3 v)
	{
		w.WritePropertyName(name);
		WriteVecValue(w, v);
	}

	private static void WriteVecValue(Utf8JsonWriter w, Vector3 v)
	{
		w.WriteStartArray();
		w.WriteNumberValue(v.X);
		w.WriteNumberValue(v.Y);
		w.WriteNumberValue(v.Z);
		w.WriteEndArray();
	}

	// ---- helpers --------------------------------------------------------------------------------

	private static string RelativeModel(string model)
		=> model.StartsWith(ModelFolder, StringComparison.Ordinal) ? model[ModelFolder.Length..] : model;

	private static string FloatCsv(JsonElement floats)
		=> string.Join(",", floats.EnumerateArray().Select(f => f.GetSingle().ToString("R", CultureInfo.InvariantCulture)));

	private static Vector3[] WorldPoints(JsonElement instance, string key, Matrix4x4 transform)
		=> instance.TryGetProperty(key, out JsonElement points)
			? points.EnumerateArray().Select(pt => Vector3.Transform(Vec(pt), transform)).ToArray()
			: Array.Empty<Vector3>();

	// A level.json link entry -> the chunk's local transform (rotation, then offset), row-vector form.
	private static Matrix4x4 ChunkTransform(JsonElement link)
	{
		Matrix4x4 m = Matrix4x4.CreateTranslation(Vec(link.GetProperty("offset")));
		if (link.TryGetProperty("rotation", out JsonElement r))
		{
			m = Matrix4x4.CreateFromQuaternion(new Quaternion(r[0].GetSingle(), r[1].GetSingle(), r[2].GetSingle(), r[3].GetSingle())) * m;
		}
		return m;
	}

	private static Vector3 Vec(JsonElement a) => new(a[0].GetSingle(), a[1].GetSingle(), a[2].GetSingle());

	private static Entity Empty(string name)
	{
		Entity e = World.Create();
		e.Name = name;
		e.AddTransform();
		return e;
	}

	private static Entity SpawnModel(string name, string path, Vector3 position, Vector3 euler)
	{
		Entity e = Empty(name);
		e.Position = position;
		e.EulerDegrees = euler;
		e.LoadModel(path);
		e.Name = name; // LoadModel names the root after the model file
		return e;
	}

	private static void NoShadows(Entity e)
	{
		for (int i = 0; i < e.ChildCount; i++)
		{
			MeshRenderer.SetCastShadows(e.GetChild(i), false);
		}
	}

	private static void Tag(Entity e, string tag) => Tags.Add(e, Tags.Create(tag));
}
