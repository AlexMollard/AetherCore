using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The level's sound, all of it from the disc (tools/tw-extract writes assets/audio/): the music
/// and ambience streams, 3D world one-shots, and 2D sounds for Crash and the menu. Gameplay
/// scripts call the small hooks below; nothing here decides gameplay.
///
/// Which sound plays for which event comes from the disc's behaviour scripts (DoSound picks a slot
/// in the object's sound list; audio/sfx/.../sounds.json has the raw table) and was checked on the
/// rig by correlating PCSX2 audio captures against every clip. Volumes are the gains measured in
/// those captures (clip full scale = 1). logs/audio/README.md has the table and the evidence.
/// </summary>
public static class TwinsanityAudio
{
	private const string LevelBank = "project://assets/audio/sfx/Earth/Hub/beach/";
	private const string GlobalBank = "project://assets/audio/sfx/Startup/Default/";
	private const string MenuBank = "project://assets/audio/sfx/Startup/Frontend/";
	private const string Streams = "project://assets/audio/music/track_";

	// Rig-measured levels (logs/audio/levels.json). Unconfirmed ones say so.
	private const float MusicVolume = 0.69f;
	private const float AmbienceVolume = 0.48f;
	private const float JumpVolume = 0.6f;       // 137: 0.66 / 0.44 in two captures
	private const float SpinVolume = 0.28f;      // 133/134
	private const float SlamLandVolume = 0.63f;  // 136
	private const float LandVolume = 0.5f;       // 158/159: 0.26-0.83
	private const float StepVolume = 0.13f;      // 105/109
	private const float WumpaVolume = 0.49f;     // 27
	private const float WumpaHudVolume = 0.31f;  // 211, 1.1 s after the pickup
	private const float MenuVolume = 0.87f;      // 32769
	private const float WorldVolume = 1.0f;      // crates and creatures: not isolated on the rig

	// Crash's own sound list (act_CRASH Sounds[]): 137 jump, 138 double jump, 133/134 spin,
	// 136 slam landing (135 is the ground body slam: the air slam plays only 136, 0.58 s after
	// the press, in logs/audio/rig_run2.wav). Land thuds and footsteps are the engine's own
	// surface sounds, from Startup/Default.rm2.
	private const float StepStride = 2.2f;  // metres of running per footstep
	private const float WumpaHudDelay = 1.1f;
	private const float HardLandImpact = 12.0f;

	private static readonly System.Random s_rng = new(0xA0D10);
	private static readonly List<(float Due, string Path, float Volume)> s_pending = new();
	private static float s_stepDistance;
	private static bool s_paused;
	private static float s_musicBus = 1.0f, s_ambienceBus = 1.0f;

	/// <summary>Level start (TwinsanityLevel.OnAttach): the music and ambience streams the
	/// placed actors name, and the 3D waterfall loops.</summary>
	public static void Start(string levelPath)
	{
		s_pending.Clear();
		s_stepDistance = 0.0f;
		s_paused = false;
		CrashPlayer.Landed -= OnCrashLanded; // idempotent across play sessions
		CrashPlayer.Landed += OnCrashLanded;
		string? text = Assets.ReadText(levelPath);
		if (text == null)
		{
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		foreach (JsonElement ins in doc.RootElement.GetProperty("instances").EnumerateArray())
		{
			string name = ins.GetProperty("name").GetString() ?? "";
			JsonElement pos = ins.GetProperty("position");
			Vector3 at = new(pos[0].GetSingle(), pos[1].GetSingle(), pos[2].GetSingle());
			if (name == "act_DJ" && Param(ins, 0) is int music)
			{
				Audio.PlayMusic(Streams + music + ".wav", 1.0f, true, MusicVolume);
			}
			else if (name.StartsWith("act_GLOBAL_AMBIENT_SOUND") && Param(ins, 2) is int ambience)
			{
				Audio.Play(Streams + ambience + ".wav", AmbienceVolume, 1.0f, true, Audio.Bus.Ambience);
			}
			else if (name.StartsWith("act_SOUND_SPOT_WATERFALL"))
			{
				// COM_SOUND_SPOT_WATERFALL_DEFAULT loops sound 516; floats[3] is the spot's level.
				float volume = ins.TryGetProperty("floats", out JsonElement f) && f.GetArrayLength() > 3 ? f[3].GetSingle() : 1.0f;
				Audio.PlayAt(LevelBank + "516.wav", at, volume, 1.0f, true, Audio.Bus.Ambience, 6.0f, 45.0f, Audio.AttenuationModel.Linear);
			}
		}
	}

	private static int? Param(JsonElement ins, int i) =>
		ins.TryGetProperty("params", out JsonElement p) && p.GetArrayLength() > i ? p[i].GetInt32() : null;

	/// <summary>Per frame from TwinsanityLevel.OnUpdate: footsteps and delayed stings.</summary>
	public static void Update(float dt, CrashPlayer crash)
	{
		float now = Time.UnscaledTime;
		for (int i = s_pending.Count - 1; i >= 0; i--)
		{
			if (s_pending[i].Due <= now)
			{
				Audio.Play(s_pending[i].Path, s_pending[i].Volume);
				s_pending.RemoveAt(i);
			}
		}
		Vector3 v = crash.Velocity;
		float speed = new Vector2(v.X, v.Z).Length();
		if (!crash.IsGrounded || crash.IsSliding || speed < 1.0f)
		{
			s_stepDistance = StepStride * 0.5f; // the first step lands half a stride in
			return;
		}
		s_stepDistance += speed * dt;
		if (s_stepDistance >= StepStride)
		{
			s_stepDistance -= StepStride;
			Audio.Play(GlobalBank + Pick(105, 109) + ".wav", StepVolume);
		}
	}

	// ---- Crash (2D: he is always at the listener's focus) ----------------------------------

	public static void Jump() => Audio.Play(LevelBank + "137.wav", JumpVolume);

	public static void DoubleJump() => Audio.Play(LevelBank + "138.wav", JumpVolume);

	public static void Spin() => Audio.Play(LevelBank + Pick(133, 134) + ".wav", SpinVolume);

	private static void OnCrashLanded(CrashPlayer crash, float impact, bool slam)
	{
		if (slam)
		{
			Audio.Play(LevelBank + "136.wav", SlamLandVolume);
		}
		else if (impact >= HardLandImpact)
		{
			Audio.Play(GlobalBank + Pick(158, 159) + ".wav", LandVolume);
		}
		else
		{
			Audio.Play(GlobalBank + Pick(105, 109) + ".wav", StepVolume);
		}
		s_stepDistance = 0.0f;
	}

	// ---- Pickups (2D, like the HUD they feed) ---------------------------------------------

	public static void Wumpa()
	{
		Audio.Play(LevelBank + "27.wav", WumpaVolume);
		s_pending.Add((Time.UnscaledTime + WumpaHudDelay, GlobalBank + "211.wav", WumpaHudVolume));
	}

	public static void ExtraLife() => Audio.Play(GlobalBank + "266.wav", WorldVolume);

	// Aku Aku (AKUMASK Sounds[] = 59, 200, 59, 200, 59, -, 200; no script plays them, the engine
	// does): 59 on gaining a mask, 200 on losing one. The split is read from the slot order, not
	// isolated on the rig; the level is not measured either.
	public static void AkuGained() => Audio.Play(LevelBank + "59.wav", WorldVolume);

	public static void AkuLost() => Audio.Play(LevelBank + "200.wav", WorldVolume);

	// ---- Crates (3D) ----------------------------------------------------------------------

	/// <summary>A crate was jumped on. TNT answers with its fuse tick (TNTCRATE Sounds[0]).</summary>
	public static void CrateBounce(Vector3 at, int objectId) => World(objectId == 5 ? "14" : "20", at);

	public static void TntTick(Vector3 at) => World("14", at);

	/// <summary>Wooden crate break (COM_*_CRATE_BREAK, Sounds[1] = 21). TNT and nitro go through Explosion.</summary>
	public static void CrateBreak(Vector3 at, int objectId)
	{
		if (objectId is not (4 or 5))
		{
			World("21", at);
		}
	}

	public static void Checkpoint(Vector3 at) => World("23", at);

	public static void Explosion(Vector3 at) => At(GlobalBank + "22.wav", at, 8.0f, 90.0f);

	/// <summary>Nitro idle hop (COM_NITRO_CRATE_DEFAULT, Sounds[0] = 51).</summary>
	public static void NitroHop(Vector3 at) => At(GlobalBank + "51.wav", at, 2.0f, 25.0f);

	private static void World(string id, Vector3 at) => At(GlobalBank + id + ".wav", at, 4.0f, 50.0f);

	// Linear falloff reaches silence at maxDistance (inverse would floor at min/max and leave the
	// far waterfalls audible across the whole hub). The hub's actors all run at once (dozens of
	// nitros and chickens): skip voices out of range rather than let them steal the 64-voice
	// pool's ambience loops.
	private static void At(string path, Vector3 at, float minDistance, float maxDistance)
	{
		Entity camera = Camera.Main;
		if (camera.IsValid && Vector3.DistanceSquared(camera.Position, at) > maxDistance * maxDistance)
		{
			return;
		}
		Audio.PlayAt(path, at, WorldVolume, 1.0f, false, Audio.Bus.Sfx, minDistance, maxDistance, Audio.AttenuationModel.Linear);
	}

	// ---- Creatures (3D) -------------------------------------------------------------------

	public enum Call
	{
		Death,      // GENERIC_CREATURE_DAMAGED -> Sounds[3] = 40 for crab, monkey, worm and chicken
		GullTakeOff,// GLOBAL_SEAGULL_HIGHFLIGHT -> 93 (wing flap)
		Cluck,      // GLOBAL_CHICKEN_IDLE -> 82/83/84
		Chatter,    // CREATURE_BASIC_IDLE_STATIONARY (monkey) -> 114/118
		Throw,      // CREATURE_BASIC_ATTACK_MELEE (monkey) -> 77
		TreeShake,  // WUMPA_TREE -> 235
		WormPop,    // EARTH_WORM_IDLE -> 779/780
		CrabCharge, // GLOBAL_CRAB -> 70
	}

	public static void Creature(Call call, Vector3 at)
	{
		string id = call switch
		{
			Call.Death => "40",
			Call.GullTakeOff => "93",
			Call.Cluck => Pick(82, 83, 84).ToString(),
			Call.Chatter => Pick(114, 118).ToString(),
			Call.Throw => "77",
			Call.TreeShake => "235",
			Call.WormPop => Pick(779, 780).ToString(),
			_ => "70",
		};
		At(LevelBank + id + ".wav", at, 3.0f, 40.0f);
	}

	// ---- Pause menu (2D) ------------------------------------------------------------------

	/// <summary>The rig goes fully silent while paused (the SPU stops): only the menu sounds play.</summary>
	public static void PauseOpened()
	{
		if (s_paused)
		{
			return;
		}
		s_paused = true;
		s_musicBus = Audio.GetBusVolume(Audio.Bus.Music);
		s_ambienceBus = Audio.GetBusVolume(Audio.Bus.Ambience);
		Audio.SetBusVolume(Audio.Bus.Music, 0.0f);
		Audio.SetBusVolume(Audio.Bus.Ambience, 0.0f);
	}

	public static void PauseClosed()
	{
		if (!s_paused)
		{
			return;
		}
		s_paused = false;
		Audio.SetBusVolume(Audio.Bus.Music, s_musicBus);
		Audio.SetBusVolume(Audio.Bus.Ambience, s_ambienceBus);
	}

	public static void MenuMove() => Audio.Play(MenuBank + "32769.wav", MenuVolume);

	public static void MenuSelect() => Audio.Play(MenuBank + "32768.wav", MenuVolume);

	private static int Pick(params int[] ids) => ids[s_rng.Next(ids.Length)];
}
