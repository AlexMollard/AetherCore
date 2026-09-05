#include "project/ProjectCommon.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <system_error>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#	include <shobjidl.h>
#	undef CopyFile
#endif

#include "EngineContentPaths.hpp"
#include "io/FileUtil.hpp"
#include "utils/AetherExceptions.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"
#include "utils/TextIni.hpp"
#include "utils/TomlConfig.hpp"

using namespace std::string_view_literals;

namespace aether::app::project
{
	namespace
	{
		constexpr std::string_view kProjectFileName = "ProjectSettings.toml";

		std::filesystem::path ResolveProjectPath(const std::filesystem::path& root, std::string_view value, std::string_view fallback)
		{
			std::filesystem::path path = value.empty() ? std::filesystem::path(fallback) : std::filesystem::path(std::string(value));
			if (path.is_relative())
			{
				path = root / path;
			}
			return NormalizePath(std::move(path));
		}

		std::string EscapeTomlString(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				if (c == '\\' || c == '"')
				{
					out += '\\';
				}
				out += c;
			}
			return out;
		}

		std::string EscapeXmlAttribute(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				switch (c)
				{
					case '&':
						out += "&amp;";
						break;
					case '<':
						out += "&lt;";
						break;
					case '>':
						out += "&gt;";
						break;
					case '"':
						out += "&quot;";
						break;
					case '\'':
						out += "&apos;";
						break;
					default:
						out += c;
						break;
				}
			}
			return out;
		}

		std::filesystem::path AbsolutePath(const std::filesystem::path& path)
		{
			if (path.empty())
			{
				return {};
			}
			std::error_code ec;
			const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
			return ec ? path.lexically_normal() : absolute.lexically_normal();
		}

		std::filesystem::path ManagedSdkProjectPath()
		{
			return AbsolutePath(EngineManagedSdkProject());
		}

		// Forward-declared: MakeStarterScriptText dispatches to MakeMultiplayerPlayerScriptText
		// for the multiplayer templates, but is defined first (it is the oldest of these, and
		// the natural entry point to read first).
		std::string MakeMultiplayerPlayerScriptText(bool is2D);
		std::string MakeLobbyScriptText();
		std::string MakeSessionScriptText();
		std::string MakePlayerPrefabTomlText(bool is2D);

		// A new project used to be one camera, an empty scripts folder and nothing to copy
		// from, which hides the best thing about the engine behind "go read the SDK source".
		// One commented script that visibly does something is the whole difference.
		std::string MakeStarterScriptText(ProjectTemplate projectTemplate)
		{
			if (projectTemplate == ProjectTemplate::Multiplayer2D || projectTemplate == ProjectTemplate::Multiplayer3D)
			{
				return MakeMultiplayerPlayerScriptText(ProjectKindForTemplate(projectTemplate) == ProjectKind::Scene2D);
			}

			const bool is2D = ProjectKindForTemplate(projectTemplate) == ProjectKind::Scene2D;
			std::string src;
			src += "using System.Numerics;\n";
			src += "using AetherCore;\n\n";
			src += "namespace AetherGame;\n\n";
			// Both templates ship with this already attached to their Player, so the first
			// thing a new user reads must not be an instruction to do what is already done.
			src += "// Already attached to the Player in this scene - press Play and move with WASD.\n";
			src += "// Speed below shows up in the Inspector and saves with the scene. Change it, Play again.\n";
			src += "// F5 rebuilds and hot-reloads this file while Play is still running.\n";
			src += "// To use it elsewhere: select an entity, then Add Component > Script > Player.\n";
			src += "public sealed class Player : EntityScript\n";
			src += "{\n";
			src += "\tpublic float Speed = 5.0f;\n\n";
			src += "\tpublic override void OnUpdate(float deltaTime)\n";
			src += "\t{\n";
			src += "\t\t// A/D or left/right. GetAxisRaw returns -1, 0 or +1.\n";
			src += "\t\tfloat x = Input.GetAxisRaw(Key.A, Key.D) + Input.GetAxisRaw(Key.Left, Key.Right);\n";
			if (is2D)
			{
				src += "\t\tfloat y = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);\n";
				src += "\t\tVector3 move = new(x, y, 0.0f);\n";
			}
			else
			{
				src += "\t\tfloat z = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);\n";
				src += "\t\tVector3 move = new(x, 0.0f, z);\n";
			}
			src += "\t\tSelf.Position += move * Speed * deltaTime;\n";
			src += "\t}\n";
			src += "}\n";
			return src;
		}

		// The multiplayer templates' player script - shared line-for-line between the 2D and
		// 3D variants except for the movement plane and how "Boosting" is shown, exactly the
		// same is2D-branch shape MakeStarterScriptText already uses above. A second, near-
		// duplicate 80-line literal for the 3D side would be the same copy-paste problem this
		// whole batch exists to remove, just moved into this generator instead of a game.
		//
		// Attached to assets/prefabs/player.prefab.toml, not to a scene-placed entity - see
		// RequestWave's own comment for why that placement is load-bearing, not a preference.
		std::string MakeMultiplayerPlayerScriptText(bool is2D)
		{
			std::string src;
			src += "using System.Numerics;\n";
			src += "using AetherCore;\n\n";
			src += "namespace AetherGame;\n\n";
			src += "// Attached to assets/prefabs/player.prefab.toml - every peer's copy of every player\n";
			src += "// runs this script, but only the OWNER's copy is allowed to decide anything.\n";
			src += "// Net.HasAuthority is the gate: true for the entity you actually control, true for\n";
			src += "// everybody's entity when there is no session at all, and false for every other\n";
			src += "// player's copy on your screen. Get this gate wrong - check it once in OnAttach\n";
			src += "// instead of every frame, or skip it entirely - and two peers fight over the same\n";
			src += "// entity's position.\n";
			src += "public sealed class Player : EntityScript\n";
			src += "{\n";
			src += "\tpublic float Speed = 5.0f;\n";
			src += "\tpublic float BoostSpeedMultiplier = 2.0f;\n\n";
			src += "\t// [Replicated] fields are how the OWNER's decisions reach everyone else's screen:\n";
			src += "\t// this peer writes it while it owns the entity, the framework sends the change,\n";
			src += "\t// and every other copy - offline there are none - applies it below without\n";
			src += "\t// simulating anything itself.\n";
			src += "\t[Replicated] public bool Boosting;\n\n";
			if (is2D)
			{
				// Two fixed colours, one per spawn point in this template's Arena
				// (MultiplayerSession.SpawnPointCount = 2) - just enough that two players
				// are visually distinguishable, picked in OnOwnershipChanged below rather
				// than being the same static colour for everyone.
				src += "\tprivate static readonly Vector4[] NormalTints = { new(0.25f, 0.65f, 0.85f, 1.0f), new(0.85f, 0.45f, 0.25f, 1.0f) };\n";
				src += "\tprivate static readonly Vector4[] BoostTints = { new(1.0f, 0.85f, 0.2f, 1.0f), new(1.0f, 0.55f, 0.85f, 1.0f) };\n";
				src += "\tprivate Vector4 _normalTint = NormalTints[0];\n";
				src += "\tprivate Vector4 _boostTint = BoostTints[0];\n\n";
			}
			else
			{
				src += "\tprivate static readonly Vector3[] NormalColors = { new(0.25f, 0.65f, 0.85f), new(0.85f, 0.45f, 0.25f) };\n";
				src += "\tprivate static readonly Vector3[] BoostColors = { new(1.0f, 0.85f, 0.2f), new(1.0f, 0.55f, 0.85f) };\n";
				src += "\tprivate Vector3 _normalColor = NormalColors[0];\n";
				src += "\tprivate Vector3 _boostColor = BoostColors[0];\n\n";
			}
			src += "\tprivate const float WaveDuration = 0.3f;\n";
			src += "\tprivate const float WaveScale = 1.5f;\n";
			src += "\tprivate float _waveSecondsLeft;\n\n";
			src += "\t// The one-time hookup between \"this connection now owns this entity\" and \"pick\n";
			src += "\t// this player's colour\" - OnAttach cannot do it because the owning CONNECTION id\n";
			src += "\t// is not reliably known there: on a client it is only decided once the host's\n";
			src += "\t// Welcome lands (see OnOwnershipChanged's own remarks), which can be a frame or\n";
			src += "\t// more after OnAttach runs, and this fires again if ownership ever changes hands.\n";
			src += "\tpublic override void OnOwnershipChanged(uint owner, bool isOwner)\n";
			src += "\t{\n";
			if (is2D)
			{
				src += "\t\tint slot = (int)(owner % NormalTints.Length);\n";
				src += "\t\t_normalTint = NormalTints[slot];\n";
				src += "\t\t_boostTint = BoostTints[slot];\n";
			}
			else
			{
				src += "\t\tint slot = (int)(owner % NormalColors.Length);\n";
				src += "\t\t_normalColor = NormalColors[slot];\n";
				src += "\t\t_boostColor = BoostColors[slot];\n";
			}
			src += "\t}\n\n";
			src += "\tpublic override void OnUpdate(float deltaTime)\n";
			src += "\t{\n";
			src += "\t\t// Drive your own entity locally; replication drives everyone else's. True\n";
			src += "\t\t// offline too, so this if is the whole single-player/multiplayer split -\n";
			src += "\t\t// nothing below it runs for a player you do not own.\n";
			src += "\t\tif (Net.HasAuthority(Self))\n";
			src += "\t\t{\n";
			src += "\t\t\tBoosting = Input.IsKeyDown(Key.LeftShift);\n\n";
			src += "\t\t\t// A/D or left/right. GetAxisRaw returns -1, 0 or +1.\n";
			src += "\t\t\tfloat x = Input.GetAxisRaw(Key.A, Key.D) + Input.GetAxisRaw(Key.Left, Key.Right);\n";
			if (is2D)
			{
				src += "\t\t\tfloat y = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);\n";
				src += "\t\t\tfloat speed = Boosting ? Speed * BoostSpeedMultiplier : Speed;\n";
				src += "\t\t\tSelf.Position += new Vector3(x, y, 0.0f) * speed * deltaTime;\n\n";
			}
			else
			{
				src += "\t\t\tfloat z = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);\n";
				src += "\t\t\tfloat speed = Boosting ? Speed * BoostSpeedMultiplier : Speed;\n";
				src += "\t\t\tSelf.Position += new Vector3(x, 0.0f, z) * speed * deltaTime;\n\n";
			}
			src += "\t\t\tif (Input.IsKeyPressed(Key.E))\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\tNet.CallServer(Self, nameof(RequestWave));\n";
			src += "\t\t\t}\n";
			src += "\t\t}\n\n";
			src += "\t\t// Runs for every copy, owner and remote alike: Boosting already carries the\n";
			src += "\t\t// owner's decision across the wire, so applying it here is the only place\n";
			src += "\t\t// that needs to.\n";
			if (is2D)
			{
				src += "\t\tSpriteRenderer.SetTint(Self, Boosting ? _boostTint : _normalTint);\n";
			}
			else
			{
				src += "\t\tSelf.SetMaterialColor(Boosting ? _boostColor : _normalColor);\n";
			}
			src += "\t\tTickWave(deltaTime);\n";
			src += "\t}\n\n";
			src += "\t// A Server RPC is accepted only from the connection that OWNS the entity it is\n";
			src += "\t// addressed to - so this has to live here, on the player prefab an owner\n";
			src += "\t// actually owns, and never on a scene-placed entity like the session's\n";
			src += "\t// GameSession. Every client's call aimed at a host-owned entity would be\n";
			src += "\t// addressed to something none of them own, and the host would refuse every\n";
			src += "\t// single one of them without saying why.\n";
			src += "\t//\n";
			src += "\t// MaxPerSecond matters for every Server RPC a client can call, not just this one:\n";
			src += "\t// it runs on the HOST at whatever rate a modified or malicious client sends it, not\n";
			src += "\t// the rate a well-behaved client's own E-key cooldown implies - leaving it unset is\n";
			src += "\t// an unbounded flood vector the framework cannot see for you.\n";
			src += "\t[NetRpc(NetRpcTarget.Server, MaxPerSecond = 5)]\n";
			src += "\tpublic void RequestWave()\n";
			src += "\t{\n";
			src += "\t\t// Runs on the host (or locally, offline). Only the host may originate a\n";
			src += "\t\t// Multicast call, which this call already is by the time it gets here.\n";
			src += "\t\tNet.Call(Self, nameof(PlayWave));\n";
			src += "\t}\n\n";
			src += "\t// Host -> everyone, itself included: play the wave on this player's own copy.\n";
			src += "\t[NetRpc(NetRpcTarget.Multicast)]\n";
			src += "\tpublic void PlayWave() => _waveSecondsLeft = WaveDuration;\n\n";
			src += "\tprivate void TickWave(float deltaTime)\n";
			src += "\t{\n";
			src += "\t\tif (_waveSecondsLeft <= 0.0f)\n";
			src += "\t\t{\n";
			src += "\t\t\tSelf.Scale = Vector3.One;\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n";
			src += "\t\t_waveSecondsLeft -= deltaTime;\n";
			src += "\t\tfloat t = System.Math.Max(_waveSecondsLeft, 0.0f) / WaveDuration;\n";
			src += "\t\tSelf.Scale = Vector3.One * (1.0f + (WaveScale - 1.0f) * t);\n";
			src += "\t}\n";
			src += "}\n";
			return src;
		}

		// Shared verbatim by both multiplayer templates - nothing here reads or depends on
		// 2D vs 3D, so there is nothing to branch on and nothing to duplicate.
		std::string MakeLobbyScriptText()
		{
			std::string src;
			src += "using AetherCore;\n\n";
			src += "namespace AetherGame;\n\n";
			src += "// The one screen between \"press Play\" and being in a session: host under a fresh\n";
			src += "// room code, or type one back in to join. The connect ceremony itself - minting or\n";
			src += "// reading the code, flipping Net.ReplicationReady at exactly the right frame,\n";
			src += "// narrating traversal, the join timeout, cancelling cleanly - belongs to the SDK's\n";
			src += "// NetConnectMenu now, not to this file. MultiplayerLobby is a thin NetConnectMenu\n";
			src += "// SUBCLASS, the same shape as MultiplayerSession : NetSessionDirector below - and\n";
			src += "// for the same reason: a scene file can only attach a type this project's own\n";
			src += "// AetherGame.dll defines, never an SDK type directly, so \"use NetConnectMenu\" has\n";
			src += "// to mean subclassing it, not naming it in the scene. Everything below is only\n";
			src += "// what a game still has to decide for itself: how the screen looks, and which key\n";
			src += "// or click maps to which inherited call.\n";
			src += "//\n";
			src += "// The widgets are built here in OnAttach rather than authored in the scene on\n";
			src += "// purpose: an Entity-typed script property (the way a scene wires an authored\n";
			src += "// widget back to a field here, e.g. `public Entity HostButton;`) is never actually\n";
			src += "// applied to a loaded script instance in this engine build - the native/managed\n";
			src += "// property bridge (AetherCore.Interop.ScriptRegistry.SetProperty) has no case for\n";
			src += "// it, so the field silently stays at its default and every scene-wired Entity\n";
			src += "// reads back invalid. Building the widgets here sidesteps that entirely: each one\n";
			src += "// is created, held in a field, and used by the same code that created it, with no\n";
			src += "// cross-boundary lookup or binding to go missing.\n";
			src += "//\n";
			src += "// No port to forward, on either side - see NetSession.BeginHostWithCode/BeginJoinByCode\n";
			src += "// (which Host()/Join() below call through to) for why. Candidates already publish\n";
			src += "// over LAN broadcast with no call at all (Net.UseLanSignaling is the default until\n";
			src += "// something else is chosen), so two players on the same network need nothing here.\n";
			src += "// Two players on different networks need a rendezvous address: set\n";
			src += "// network.rendezvousHost (and network.rendezvousPort) in this project's\n";
			src += "// EngineSettings.toml, or call Net.UseRendezvousSignaling(\"host:port\") once before\n";
			src += "// hosting or joining. Neither is called here on purpose - an implicit choice in this\n";
			src += "// screen would silently override a settings screen's own choice.\n";
			src += "public sealed class MultiplayerLobby : NetConnectMenu\n";
			src += "{\n";
			src += "\tprivate const int RoomCodeLength = 6;\n\n";
			src += "\t// Set the moment Host() succeeds and never cleared - MultiplayerSession reads it\n";
			src += "\t// in the Arena to show the host their own code again. It has to be a STATIC field\n";
			src += "\t// rather than an instance one: this whole entity, this whole script instance, is\n";
			src += "\t// destroyed the moment EnterArena() loads a new scene, and a static is the only\n";
			src += "\t// thing that survives that. NetSession has the same \"survives a scene load\" job\n";
			src += "\t// for other fields, but NOT this one - NetSession.HostRoomCode is deliberately the\n";
			src += "\t// OPPOSITE value: it is where a CLIENT remembers the code it joined with so a\n";
			src += "\t// dropped link can retry it, and every hosting path (BeginHost, BeginHostWithCode,\n";
			src += "\t// BeginJoin) explicitly blanks it - so a host reading it back would always find it\n";
			src += "\t// empty. Read NetSession.cs before trusting a field's name over what it actually\n";
			src += "\t// stores; this one very nearly went into MultiplayerSession that way.\n";
			src += "\tpublic static string LastHostedRoomCode = string.Empty;\n\n";
			src += "\tprivate Entity _hostButton;\n";
			src += "\tprivate Entity _playSoloButton;\n";
			src += "\tprivate Entity _joinButton;\n";
			src += "\tprivate Entity _codeBox;\n";
			src += "\tprivate Entity _roomCode;\n";
			src += "\tprivate Entity _copyButton;\n";
			src += "\tprivate Entity _status;\n";
			src += "\tprivate float _copyFeedbackSecondsLeft;\n\n";
			src += "\tpublic MultiplayerLobby()\n";
			src += "\t{\n";
			src += "\t\tArenaScene = \"Arena\";\n";
			src += "\t}\n\n";
			src += "\tpublic override void OnAttach()\n";
			src += "\t{\n";
			src += "\t\tEntity canvas = Ui.CreateCanvas();\n\n";
			src += "\t\tEntity title = Ui.CreateText(canvas, \"AetherCore Multiplayer Template\");\n";
			src += "\t\tUi.SetAnchors(title, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(title, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(title, 0.0f, 40.0f, 700.0f, 44.0f);\n";
			src += "\t\tUi.SetTextAlign(title, UiHAlign.Center, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(title, 30.0f);\n\n";
			src += "\t\t// HOST and PLAY SOLO: two ways to start something new, side by side.\n";
			src += "\t\t_hostButton = Ui.CreateButton(canvas);\n";
			src += "\t\tUi.SetButtonLabel(_hostButton, \"HOST\");\n";
			src += "\t\tUi.SetAnchors(_hostButton, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_hostButton, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(_hostButton, -140.0f, 110.0f, 260.0f, 56.0f);\n";
			src += "\t\tUi.SetFontSize(_hostButton, 22.0f);\n\n";
			src += "\t\t_playSoloButton = Ui.CreateButton(canvas);\n";
			src += "\t\tUi.SetButtonLabel(_playSoloButton, \"PLAY SOLO\");\n";
			src += "\t\tUi.SetAnchors(_playSoloButton, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_playSoloButton, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(_playSoloButton, 140.0f, 110.0f, 260.0f, 56.0f);\n";
			src += "\t\tUi.SetFontSize(_playSoloButton, 22.0f);\n\n";
			src += "\t\tEntity divider = Ui.CreateText(canvas, \"or join a game\");\n";
			src += "\t\tUi.SetAnchors(divider, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(divider, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(divider, 0.0f, 192.0f, 400.0f, 22.0f);\n";
			src += "\t\tUi.SetTextAlign(divider, UiHAlign.Center, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(divider, 16.0f);\n\n";
			src += "\t\tEntity codeLabel = Ui.CreateText(canvas, \"ROOM CODE\");\n";
			src += "\t\tUi.SetAnchors(codeLabel, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(codeLabel, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(codeLabel, -195.0f, 228.0f, 250.0f, 18.0f);\n";
			src += "\t\tUi.SetTextAlign(codeLabel, UiHAlign.Left, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(codeLabel, 14.0f);\n\n";
			src += "\t\t_codeBox = Ui.CreateTextBox(canvas);\n";
			src += "\t\tUi.SetAnchors(_codeBox, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_codeBox, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(_codeBox, -195.0f, 252.0f, 230.0f, 48.0f);\n";
			src += "\t\tUi.SetPlaceholder(_codeBox, \"ROOM CODE\");\n";
			src += "\t\tUi.SetContentType(_codeBox, UiContentType.Alphanumeric);\n";
			src += "\t\tUi.SetMaxLength(_codeBox, RoomCodeLength);\n";
			src += "\t\tUi.SetFontSize(_codeBox, 22.0f);\n\n";
			src += "\t\t_joinButton = Ui.CreateButton(canvas);\n";
			src += "\t\tUi.SetButtonLabel(_joinButton, \"JOIN\");\n";
			src += "\t\tUi.SetAnchors(_joinButton, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_joinButton, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(_joinButton, 55.0f, 252.0f, 140.0f, 48.0f);\n";
			src += "\t\tUi.SetFontSize(_joinButton, 20.0f);\n\n";
			src += "\t\t// The minted code is the entire join credential, so it gets its own large,\n";
			src += "\t\t// prominent line once hosting - a status line alone is easy to misread aloud -\n";
			src += "\t\t// paired with a COPY button rather than only the silent auto-copy below, since a\n";
			src += "\t\t// clipboard write nobody can see happen is indistinguishable from one that failed.\n";
			src += "\t\t_roomCode = Ui.CreateText(canvas, string.Empty);\n";
			src += "\t\tUi.SetAnchors(_roomCode, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_roomCode, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(_roomCode, -215.0f, 320.0f, 300.0f, 40.0f);\n";
			src += "\t\tUi.SetTextAlign(_roomCode, UiHAlign.Right, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(_roomCode, 26.0f);\n\n";
			src += "\t\t_copyButton = Ui.CreateButton(canvas);\n";
			src += "\t\tUi.SetButtonLabel(_copyButton, \"COPY\");\n";
			src += "\t\tUi.SetAnchors(_copyButton, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_copyButton, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(_copyButton, 105.0f, 320.0f, 110.0f, 40.0f);\n";
			src += "\t\tUi.SetFontSize(_copyButton, 18.0f);\n";
			src += "\t\t_copyButton.SetActive(false); // nothing to copy until a Host() succeeds below\n\n";
			src += "\t\t_status = Ui.CreateText(canvas, string.Empty);\n";
			src += "\t\tUi.SetAnchors(_status, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_status, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(_status, 0.0f, 372.0f, 560.0f, 24.0f);\n";
			src += "\t\tUi.SetTextAlign(_status, UiHAlign.Center, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(_status, 18.0f);\n\n";
			src += "\t\tEntity footer = Ui.CreateText(canvas, \"H host   J join field   P solo   ENTER confirm   ESC cancel a join\");\n";
			src += "\t\tUi.SetAnchors(footer, new(0.5f, 0.0f), new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetPivot(footer, new(0.5f, 0.0f));\n";
			src += "\t\tUi.SetRect(footer, 0.0f, 414.0f, 700.0f, 18.0f);\n";
			src += "\t\tUi.SetTextAlign(footer, UiHAlign.Center, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(footer, 13.0f);\n\n";
			src += "\t\t// This project's ReturnScene, so a player kicked back here after a disconnect\n";
			src += "\t\t// is told why rather than landing on a silently blank Lobby.\n";
			src += "\t\tstring returned = NetSession.TakeStatusMessage();\n";
			src += "\t\tif (returned.Length > 0)\n";
			src += "\t\t{\n";
			src += "\t\t\tUi.SetText(_status, returned);\n";
			src += "\t\t}\n";
			src += "\t}\n\n";
			src += "\tpublic override void OnUpdate(float deltaTime)\n";
			src += "\t{\n";
			src += "\t\t// Base first: this is where hosting/joining actually ticks - Enter starts the\n";
			src += "\t\t// arena once hosting is ready, Escape cancels a join in progress, and a join\n";
			src += "\t\t// that connects or times out resolves here too. Nothing below reimplements any\n";
			src += "\t\t// of that; it only has to keep the screen in sync with the result.\n";
			src += "\t\tbase.OnUpdate(deltaTime);\n";
			src += "\t\tUi.SetText(_status, StatusText);\n\n";
			src += "\t\t// The COPY button lives outside the busy gate below on purpose: IsBusy is true\n";
			src += "\t\t// for the whole time this screen is showing a freshly hosted code (IsHosting is\n";
			src += "\t\t// one of the two things IsBusy means), which is exactly when re-copying it is\n";
			src += "\t\t// most useful - a host who missed the first silent copy, or wants to hand the\n";
			src += "\t\t// code to a second friend, still has a working button.\n";
			src += "\t\tbool hasCode = IsHosting && HostedRoomCode.Length > 0;\n";
			src += "\t\tUi.SetText(_roomCode, hasCode ? $\"Room code: {HostedRoomCode}\" : string.Empty);\n";
			src += "\t\t_copyButton.SetActive(hasCode);\n";
			src += "\t\tif (hasCode)\n";
			src += "\t\t{\n";
			src += "\t\t\tif (_copyFeedbackSecondsLeft > 0.0f)\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\t_copyFeedbackSecondsLeft -= deltaTime;\n";
			src += "\t\t\t\tif (_copyFeedbackSecondsLeft <= 0.0f)\n";
			src += "\t\t\t\t{\n";
			src += "\t\t\t\t\tUi.SetButtonLabel(_copyButton, \"COPY\");\n";
			src += "\t\t\t\t}\n";
			src += "\t\t\t}\n";
			src += "\t\t\tif (Ui.WasActivated(_copyButton) || (!Ui.IsEditing(_codeBox) && Input.IsKeyPressed(Key.C)))\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\tCopyRoomCode();\n";
			src += "\t\t\t}\n";
			src += "\t\t}\n\n";
			src += "\t\t// NetConnectMenu already refuses a second Host/Join/PlaySolo while one is in\n";
			src += "\t\t// flight - this just makes the screen SAY so instead of swallowing a click (or a\n";
			src += "\t\t// keystroke) that would silently have done nothing.\n";
			src += "\t\tbool idle = !IsBusy;\n";
			src += "\t\tUi.SetInteractable(_hostButton, idle);\n";
			src += "\t\tUi.SetInteractable(_joinButton, idle);\n";
			src += "\t\tUi.SetInteractable(_playSoloButton, idle);\n";
			src += "\t\tUi.SetInteractable(_codeBox, idle);\n";
			src += "\t\tif (!idle)\n";
			src += "\t\t{\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n\n";
			src += "\t\t// Ui.WasActivated fires on a click, on Enter/Space while focused, and on a\n";
			src += "\t\t// gamepad A press - the engine's own navigation system drives all three - so\n";
			src += "\t\t// clicking is the primary path and Tab-to-button-then-Enter or a controller work\n";
			src += "\t\t// identically, with nothing here tracking a highlight index.\n";
			src += "\t\t//\n";
			src += "\t\t// H/J/P remain as an accelerator on top, but gated on IsEditing rather than on\n";
			src += "\t\t// focus: a CodeBox that merely holds the highlight (the mouse rested over it,\n";
			src += "\t\t// nobody clicked) must never take these keys away - only actually TYPING into it\n";
			src += "\t\t// should. Gating on Ui.HasFocus instead is the bug this screen shipped with\n";
			src += "\t\t// originally: with no buttons, the CodeBox was the only selectable on the whole\n";
			src += "\t\t// screen, mouse movement focuses whatever it passes over with no click required,\n";
			src += "\t\t// and nothing else on screen could ever steal that focus back - so one incidental\n";
			src += "\t\t// hover over the box latched Ui.HasFocus true for the rest of the session and\n";
			src += "\t\t// every one of H, J and P went dead silently.\n";
			src += "\t\tbool typingCode = Ui.IsEditing(_codeBox);\n";
			src += "\t\tif (Ui.WasActivated(_hostButton) || (!typingCode && Input.IsKeyPressed(Key.H)))\n";
			src += "\t\t{\n";
			src += "\t\t\tif (Host())\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\tLastHostedRoomCode = HostedRoomCode;\n";
			src += "\t\t\t\tCopyRoomCode();\n";
			src += "\t\t\t}\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n";
			src += "\t\tif (Ui.WasActivated(_playSoloButton) || (!typingCode && Input.IsKeyPressed(Key.P)))\n";
			src += "\t\t{\n";
			src += "\t\t\tPlaySolo();\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n";
			src += "\t\tif (!typingCode && Input.IsKeyPressed(Key.J))\n";
			src += "\t\t{\n";
			src += "\t\t\tUi.BeginEdit(_codeBox);\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n";
			src += "\t\tif (Ui.WasActivated(_joinButton) || Ui.WasSubmitted(_codeBox))\n";
			src += "\t\t{\n";
			src += "\t\t\tif (Join(Ui.GetTextBoxText(_codeBox)))\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\tUi.ClearFocus();\n";
			src += "\t\t\t}\n";
			src += "\t\t}\n";
			src += "\t}\n\n";
			src += "\t// Visible confirmation, not just the clipboard write: silence is what made\n";
			src += "\t// Input.Clipboard = HostedRoomCode useless the first time this screen shipped -\n";
			src += "\t// a player had no way to tell a copy had happened at all.\n";
			src += "\tprivate void CopyRoomCode()\n";
			src += "\t{\n";
			src += "\t\tInput.Clipboard = HostedRoomCode;\n";
			src += "\t\t_copyFeedbackSecondsLeft = 1.5f;\n";
			src += "\t\tUi.SetButtonLabel(_copyButton, \"COPIED\");\n\n";
			src += "\t\t// Every other button is non-interactable while hosting (see OnUpdate), so this\n";
			src += "\t\t// is the only thing Tab can reach - and Enter/Space on a focused selectable is\n";
			src += "\t\t// CONSUMED by the engine's nav system whether or not this method reads it, which\n";
			src += "\t\t// would otherwise leave a keyboard-only host with no way to press Enter for\n";
			src += "\t\t// \"enter arena\" ever again: it would keep re-activating this button instead.\n";
			src += "\t\t// Clearing focus (also done after a successful Join, below) hands the very next\n";
			src += "\t\t// Enter press back to NetConnectMenu.WantsToEnterArena with nothing in the way.\n";
			src += "\t\tUi.ClearFocus();\n";
			src += "\t}\n";
			src += "}\n";
			return src;
		}

		// Shared verbatim by both multiplayer templates: setting these three fields is the
		// entire connection between this project and NetSessionDirector, which already does
		// everything else (spawning, rostering, reconnecting, the offline solo fallback).
		std::string MakeSessionScriptText()
		{
			std::string src;
			src += "using AetherCore;\n\n";
			src += "namespace AetherGame;\n\n";
			src += "// This template's session director. Spawning a player per connection at a spawn\n";
			src += "// marker, the offline single-player fallback, and returning to the Lobby when the\n";
			src += "// session ends all live in the SDK's NetSessionDirector - the constructor below is\n";
			src += "// the entire connection between this project and that machinery.\n";
			src += "public sealed class MultiplayerSession : NetSessionDirector\n";
			src += "{\n";
			src += "\tprivate Entity _hostCodeLabel;\n";
			src += "\tprivate float _copyFeedbackSecondsLeft;\n\n";
			src += "\tpublic MultiplayerSession()\n";
			src += "\t{\n";
			src += "\t\tPlayerPrefab = \"player\";\n";
			src += "\t\tSpawnPointCount = 2;\n";
			src += "\t\tReturnScene = \"default\"; // this project's Lobby - seeded as scenes/default.scene.toml\n";
			src += "\t}\n\n";
			src += "\tpublic override void OnAttach()\n";
			src += "\t{\n";
			src += "\t\tbase.OnAttach();\n\n";
			src += "\t\t// Host only, and only once there is a code to show. MultiplayerLobby.LastHostedRoomCode\n";
			src += "\t\t// is empty for a client (it joined, it never hosted) and for an offline solo session\n";
			src += "\t\t// (Net.IsHost is false with no session at all), so this label is simply never built\n";
			src += "\t\t// in either case - nothing further to trace for \"offline must be unaffected\". A\n";
			src += "\t\t// client sees nothing here on purpose: it already typed the code in to reach this\n";
			src += "\t\t// scene, so redisplaying it would only invite it to be misread as this player's own\n";
			src += "\t\t// invite rather than the host's.\n";
			src += "\t\tif (!Net.IsHost || MultiplayerLobby.LastHostedRoomCode.Length == 0)\n";
			src += "\t\t{\n";
			src += "\t\t\treturn;\n";
			src += "\t\t}\n\n";
			src += "\t\t// A small corner label, not a banner across the middle - this is a HUD detail for\n";
			src += "\t\t// the host to glance at while inviting a second friend, not the main event.\n";
			src += "\t\tEntity canvas = Ui.CreateCanvas();\n";
			src += "\t\t_hostCodeLabel = Ui.CreateText(canvas, string.Empty);\n";
			src += "\t\tUi.SetAnchors(_hostCodeLabel, new(0.0f, 0.0f), new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetPivot(_hostCodeLabel, new(0.0f, 0.0f));\n";
			src += "\t\tUi.SetRect(_hostCodeLabel, 16.0f, 16.0f, 340.0f, 24.0f);\n";
			src += "\t\tUi.SetTextAlign(_hostCodeLabel, UiHAlign.Left, UiVAlign.Middle);\n";
			src += "\t\tUi.SetFontSize(_hostCodeLabel, 16.0f);\n";
			src += "\t\tSetLabelText();\n";
			src += "\t}\n\n";
			src += "\tpublic override void OnUpdate(float deltaTime)\n";
			src += "\t{\n";
			src += "\t\tbase.OnUpdate(deltaTime);\n";
			src += "\t\tif (!_hostCodeLabel.IsValid)\n";
			src += "\t\t{\n";
			src += "\t\t\treturn; // client, or offline solo - see OnAttach\n";
			src += "\t\t}\n\n";
			src += "\t\tif (_copyFeedbackSecondsLeft > 0.0f)\n";
			src += "\t\t{\n";
			src += "\t\t\t_copyFeedbackSecondsLeft -= deltaTime;\n";
			src += "\t\t\tif (_copyFeedbackSecondsLeft <= 0.0f)\n";
			src += "\t\t\t{\n";
			src += "\t\t\t\tSetLabelText();\n";
			src += "\t\t\t}\n";
			src += "\t\t}\n";
			src += "\t\tif (Input.IsKeyPressed(Key.C))\n";
			src += "\t\t{\n";
			src += "\t\t\tInput.Clipboard = MultiplayerLobby.LastHostedRoomCode;\n";
			src += "\t\t\t_copyFeedbackSecondsLeft = 1.5f;\n";
			src += "\t\t\tUi.SetText(_hostCodeLabel, \"Copied!\");\n";
			src += "\t\t}\n";
			src += "\t}\n\n";
			src += "\tprivate void SetLabelText() => Ui.SetText(_hostCodeLabel, $\"Room {MultiplayerLobby.LastHostedRoomCode} - C to copy\");\n";
			src += "}\n";
			return src;
		}

		// The multiplayer templates' player prefab. `network_identity` is an empty table on
		// purpose: netId/owner are runtime state the host assigns at Net.Spawn/Scene.Instantiate,
		// and authoring them here would fight that - its PRESENCE, not its contents, is what
		// marks this entity as replicated. `network_transform` is what makes a REMOTE copy of
		// this player move smoothly (rendered slightly in the past so motion between snapshots
		// interpolates); it never touches the copy this peer owns, which Player.cs writes
		// directly every frame with no round trip.
		std::string MakePlayerPrefabTomlText(bool is2D)
		{
			std::string toml;
			toml += "# AetherCore prefab - this template's networked player.\n";
			toml += "[[entities]]\n";
			toml += "name = 'Player'\n";
			toml += "parent = -1\n";
			toml += "position = [ 0.0, 0.0, 0.0 ]\n";
			toml += "euler = [ 0.0, 0.0, 0.0 ]\n";
			toml += "scale = [ 1.0, 1.0, 1.0 ]\n\n";
			if (is2D)
			{
				toml += "    [entities.sprite]\n";
				toml += "    atlas = ''\n";
				toml += "    blend_mode = 'alpha'\n";
				toml += "    flip_x = false\n";
				toml += "    flip_y = false\n";
				toml += "    order_in_layer = 0\n";
				toml += "    pivot = [ 0.5, 0.5 ]\n";
				toml += "    pixel_art = false\n";
				toml += "    pixel_size = [ 16.0, 16.0 ]\n";
				toml += "    pixel_snap = false\n";
				toml += "    pixels_per_unit = 16.0\n";
				toml += "    sorting_layer = 0\n";
				toml += "    texture = ''\n";
				toml += "    tint = [ 0.25, 0.65, 0.85, 1.0 ]\n";
				toml += "    uv_rect = [ 0.0, 0.0, 1.0, 1.0 ]\n";
				toml += "    visible = true\n\n";
			}
			else
			{
				// Mesh + material + a KINEMATIC physics body, matching the Blank 3D template's
				// own Player: a script that writes Self.Position every frame is telling the body
				// where to be, which is what a kinematic body is for - a dynamic one is owned by
				// the solver and fights it instead.
				toml += "    [entities.material]\n";
				toml += "    alpha_blend = false\n";
				toml += "    alpha_cutoff = 0.5\n";
				toml += "    alpha_mask = false\n";
				toml += "    base_color = [ 0.25, 0.65, 0.85, 1.0 ]\n";
				toml += "    double_sided = false\n";
				toml += "    emissive = [ 0.0, 0.0, 0.0 ]\n";
				toml += "    metallic = 0.0\n";
				toml += "    occlusion = 1.0\n";
				toml += "    receive_shadows = true\n";
				toml += "    roughness = 0.55\n";
				toml += "    vertex_color = false\n\n";
				toml += "    [entities.mesh]\n";
				toml += "    index = 0\n";
				toml += "    kind = 'primitive'\n";
				toml += "    path = 'cube'\n\n";
				toml += "    [entities.physics]\n";
				toml += "    allow_sleeping = true\n";
				toml += "    angular_damping = 0.05\n";
				toml += "    ccd = false\n";
				toml += "    center = [ 0.0, 0.0, 0.0 ]\n";
				toml += "    friction = 0.5\n";
				toml += "    gravity_factor = 1.0\n";
				toml += "    half_extents = [ 0.5, 0.5, 0.5 ]\n";
				toml += "    half_height = 0.5\n";
				toml += "    linear_damping = 0.05\n";
				toml += "    lock_position = [ 0.0, 0.0, 0.0 ]\n";
				toml += "    lock_rotation = [ 0.0, 0.0, 0.0 ]\n";
				toml += "    mass = 0.0\n";
				toml += "    max_angular_vel = 47.124\n";
				toml += "    max_linear_vel = 500.0\n";
				toml += "    motion = 'kinematic'\n";
				toml += "    radius = 0.5\n";
				toml += "    restitution = 0.0\n";
				toml += "    sensor = false\n";
				toml += "    shape = 'box'\n\n";
			}
			toml += "    [entities.net_player]\n";
			toml += "    display_name = ''\n\n";
			toml += "    [entities.network_identity]\n\n";
			toml += "    # A REMOTE copy of this entity is projected toward the present by default\n";
			toml += "    # (extrapolated, capped at maxExtrapolationSeconds = 0.15s past the newest real\n";
			toml += "    # sample) - lower that cap for a fast-turning entity, since a turn mid-flight is\n";
			toml += "    # exactly what extrapolation undershoots on until the next real sample corrects it.\n";
			toml += "    [entities.network_transform]\n";
			toml += "    interpolation_delay = 0.1\n\n";
			toml += "    [[entities.scripts]]\n";
			toml += "    type = 'Player'\n";
			return toml;
		}

		bool SeedProjectTemplateFiles(const std::filesystem::path& root, ProjectTemplate projectTemplate, std::string& error)
		{
			const bool isMultiplayer = projectTemplate == ProjectTemplate::Multiplayer2D || projectTemplate == ProjectTemplate::Multiplayer3D;
			const bool is2D = ProjectKindForTemplate(projectTemplate) == ProjectKind::Scene2D;

			const std::filesystem::path starter = root / "scripts" / "Player.cs";
			if (!io::file_util::Exists(starter))
			{
				// Non-fatal: a project without the sample is still a valid project.
				if (auto written = io::file_util::WriteText(starter, MakeStarterScriptText(projectTemplate)); !written)
				{
					AE_WARN(LogCategory::App, "Could not write the starter script: {}", written.error().message);
				}
			}

			if (isMultiplayer)
			{
				const std::filesystem::path lobbyScript = root / "scripts" / "MultiplayerLobby.cs";
				if (!io::file_util::Exists(lobbyScript))
				{
					if (auto written = io::file_util::WriteText(lobbyScript, MakeLobbyScriptText()); !written)
					{
						AE_WARN(LogCategory::App, "Could not write the multiplayer lobby script: {}", written.error().message);
					}
				}

				const std::filesystem::path sessionScript = root / "scripts" / "MultiplayerSession.cs";
				if (!io::file_util::Exists(sessionScript))
				{
					if (auto written = io::file_util::WriteText(sessionScript, MakeSessionScriptText()); !written)
					{
						AE_WARN(LogCategory::App, "Could not write the multiplayer session script: {}", written.error().message);
					}
				}

				const std::filesystem::path playerPrefab = root / "assets" / "prefabs" / "player.prefab.toml";
				if (!io::file_util::Exists(playerPrefab))
				{
					if (auto written = io::file_util::WriteText(playerPrefab, MakePlayerPrefabTomlText(is2D)); !written)
					{
						AE_WARN(LogCategory::App, "Could not write the multiplayer player prefab: {}", written.error().message);
					}
				}
			}

			const std::filesystem::path scriptsProject = root / "scripts" / "AetherGame.csproj";
			if (!io::file_util::Exists(scriptsProject))
			{
				if (auto writeResult = io::file_util::WriteText(scriptsProject, MakeProjectScriptCsprojText(ManagedSdkProjectPath())); !writeResult)
				{
					error = "Could not write project scripts file: " + writeResult.error().message;
					return false;
				}
			}

			// A VS solution pairing the game scripts with the engine SDK, so the IDE
			// resolves engine types. Non-fatal if it can't be written.
			std::string solutionError;
			if (!EnsureGameSolution(root, solutionError))
			{
				error = solutionError;
				return false;
			}

			const std::filesystem::path seedScene = root / "scenes" / "default.scene.toml";
			if (!io::file_util::Exists(seedScene))
			{
				const std::filesystem::path templates = EngineSceneTemplatesDir();
				if (templates.empty())
				{
					error = "Could not find the engine's scene templates. The install looks incomplete: expected them beside the executable in data/templates/scenes.";
					return false;
				}
				if (auto dirResult = io::file_util::CreateDirectories(seedScene.parent_path()); !dirResult)
				{
					error = "Could not create project folder: " + dirResult.error().message;
					return false;
				}
				std::string_view sourceScene = "default.scene.toml";
				if (projectTemplate == ProjectTemplate::Blank2D)
				{
					sourceScene = "default2d.scene.toml";
				}
				else if (projectTemplate == ProjectTemplate::Multiplayer2D)
				{
					sourceScene = "multiplayer2d.scene.toml";
				}
				else if (projectTemplate == ProjectTemplate::Multiplayer3D)
				{
					sourceScene = "multiplayer3d.scene.toml";
				}
				if (auto copyResult = io::file_util::CopyFile(templates / sourceScene, seedScene); !copyResult)
				{
					error = "Could not copy project template scene: " + copyResult.error().message;
					return false;
				}
			}

			if (isMultiplayer)
			{
				const std::filesystem::path arenaScene = root / "scenes" / "Arena.scene.toml";
				if (!io::file_util::Exists(arenaScene))
				{
					const std::filesystem::path templates = EngineSceneTemplatesDir();
					if (templates.empty())
					{
						error = "Could not find the engine's scene templates. The install looks incomplete: expected them beside the executable in data/templates/scenes.";
						return false;
					}
					const std::string_view sourceArena = is2D ? "multiplayer2d_arena.scene.toml" : "multiplayer3d_arena.scene.toml";
					if (auto copyResult = io::file_util::CopyFile(templates / sourceArena, arenaScene); !copyResult)
					{
						error = "Could not copy the multiplayer arena scene template: " + copyResult.error().message;
						return false;
					}
				}
			}
			return true;
		}
	} // namespace

	std::filesystem::path NormalizePath(std::filesystem::path path)
	{
		std::error_code ec;
		if (path.empty())
		{
			return {};
		}
		path = std::filesystem::absolute(path, ec);
		if (ec)
		{
			return path.lexically_normal();
		}
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return ec ? path.lexically_normal() : canonical;
	}

	std::string DisplayPath(const std::filesystem::path& path)
	{
		return path.empty() ? std::string{} : path.lexically_normal().string();
	}

	std::filesystem::path ProjectFilePath(const std::filesystem::path& root)
	{
		return root / kProjectFileName;
	}

	std::filesystem::path ResolveProjectRoot(std::filesystem::path path)
	{
		path = NormalizePath(std::move(path));
		if (path.empty())
		{
			return {};
		}
		if (path.filename() == kProjectFileName)
		{
			path = path.parent_path();
		}
		return path;
	}

	bool HasProjectDescriptor(const std::filesystem::path& root)
	{
		return io::file_util::Exists(ProjectFilePath(root));
	}

	std::string FallbackProjectName(const std::filesystem::path& root)
	{
		const std::string name = root.filename().string();
		return name.empty() ? "Aether Project" : name;
	}

	std::filesystem::path PreviewImagePath(const std::filesystem::path& root)
	{
		return root / ".aether" / "preview.png";
	}

	std::string LastModifiedLabel(const std::filesystem::path& root)
	{
		namespace fs = std::filesystem;
		fs::file_time_type newest{};
		bool any = false;
		for (const fs::path& candidate: {ProjectFilePath(root), PreviewImagePath(root)})
		{
			std::error_code ec;
			const fs::file_time_type time = fs::last_write_time(candidate, ec);
			if (!ec && (!any || time > newest))
			{
				newest = time;
				any = true;
			}
		}
		if (!any)
		{
			return {};
		}

		const auto when = std::chrono::clock_cast<std::chrono::system_clock>(newest);
		const long long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - when).count();
		if (secs < 60)
		{
			return "just now";
		}
		// Past a month a relative span stops being useful and an actual date is what you
		// want; everything shorter shares utils::DurationLabel with the recovery prompt.
		if (secs >= 60LL * 60LL * 24LL * 30LL)
		{
			return std::format("{:%b %d, %Y}", std::chrono::floor<std::chrono::days>(when));
		}
		return aether::utils::DurationLabel(secs) + " ago";
	}

	Expected<EditorProjectContext> ReadProjectDescriptor(const std::filesystem::path& root)
	{
		EditorProjectContext project;
		project.root = NormalizePath(root);
		project.projectFile = ProjectFilePath(project.root);
		project.name = FallbackProjectName(project.root);
		project.assetsDir = ResolveProjectPath(project.root, {}, "assets");
		project.scenesDir = ResolveProjectPath(project.root, {}, "scenes");
		project.prefabsDir = ResolveProjectPath(project.root, {}, "assets/prefabs");
		project.scriptsDir = ResolveProjectPath(project.root, {}, "scripts");

		auto descriptorText = io::file_util::ReadText(ProjectFilePath(root));
		if (!descriptorText)
		{
			AE_UNEXPECTED(AetherError::Engine("No project descriptor found."));
		}

		std::string assetsPath;
		std::string scenesPath;
		std::string prefabsPath;
		std::string scriptsPath;
		bool parsed = false;
		try
		{
			parsed = text::ParseToml(*descriptorText,
			        [&](const text::IniEntry& entry)
			        {
				        if (entry.fullKey == "project.name")
				        {
					        project.name = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "project.kind")
				        {
					        project.kind = text::StripQuotes(entry.value) == "2d" ? ProjectKind::Scene2D : ProjectKind::Scene3D;
				        }
				        else if (entry.fullKey == "paths.assets")
				        {
					        assetsPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.scenes")
				        {
					        scenesPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.prefabs")
				        {
					        prefabsPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.scripts")
				        {
					        scriptsPath = text::StripQuotes(entry.value);
				        }
			        });
		}
		catch (...)
		{
			return project;
		}

		// A descriptor that does not parse emits NO entries, so every path and the project
		// name would silently fall back to defaults - the scenes folder would come up empty
		// and the project would look mysteriously broken rather than saying why.
		if (!parsed)
		{
			AE_UNEXPECTED(AetherError::Engine("ProjectSettings.toml is not readable TOML: " + ProjectFilePath(root).generic_string()));
		}

		if (project.name.empty())
		{
			project.name = FallbackProjectName(project.root);
		}
		project.assetsDir = ResolveProjectPath(project.root, assetsPath, "assets");
		project.scenesDir = ResolveProjectPath(project.root, scenesPath, "scenes");
		const std::string inferredPrefabsPath = !prefabsPath.empty() ? prefabsPath : (!assetsPath.empty() ? assetsPath + "/prefabs" : std::string{});
		project.prefabsDir = ResolveProjectPath(project.root, inferredPrefabsPath, "assets/prefabs");
		project.scriptsDir = ResolveProjectPath(project.root, scriptsPath, "scripts");
		return project;
	}

	std::string ReadProjectName(const std::filesystem::path& root)
	{
		auto result = ReadProjectDescriptor(root);
		return result.has_value() ? result->name : FallbackProjectName(root);
	}

	std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject)
	{
		const std::filesystem::path sdkProject = AbsolutePath(managedSdkProject);
		return "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
		       "\n"
		       "  <!--\n"
		       "    Project-owned game scripts. The editor builds this assembly at runtime\n"
		       "    from the open project, then loads AetherGame.dll through the collectible\n"
		       "    scripting context.\n"
		       "\n"
		       "    AetherCore is compile-only because the engine already loads the SDK assembly.\n"
		       "    The path below is absolute so a project created outside the engine checkout\n"
		       "    still compiles - but an absolute path is machine-specific, so it is only the\n"
		       "    DEFAULT. Set the AETHERCORE_SDK environment variable to another checkout's\n"
		       "    AetherCore.csproj and that wins instead, which is what makes this project\n"
		       "    shareable with someone whose engine lives somewhere else.\n"
		       "  -->\n"
		       "  <PropertyGroup>\n"
		       "    <AssemblyName>AetherGame</AssemblyName>\n"
		       "    <RootNamespace>AetherGame</RootNamespace>\n"
		       "    <TargetFramework>net10.0</TargetFramework>\n"
		       "    <Nullable>enable</Nullable>\n"
		       "    <LangVersion>latest</LangVersion>\n"
		       "    <ImplicitUsings>disable</ImplicitUsings>\n"
		       "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
		       "  </PropertyGroup>\n"
		       "\n"
		       "  <!-- MSBuild surfaces environment variables as properties, so AETHERCORE_SDK\n"
		       "       needs no other plumbing. -->\n"
		       "  <PropertyGroup>\n"
		       "    <AetherCoreSdkProject Condition=\"'$(AetherCoreSdkProject)' == ''\">$(AETHERCORE_SDK)</AetherCoreSdkProject>\n"
		       "    <AetherCoreSdkProject Condition=\"'$(AetherCoreSdkProject)' == ''\">"
		       + EscapeXmlAttribute(sdkProject.generic_string())
		       + "</AetherCoreSdkProject>\n"
		         "  </PropertyGroup>\n"
		         "\n"
		         "  <Target Name=\"AetherCoreSdkPresent\" BeforeTargets=\"PrepareForBuild\">\n"
		         "    <Error Condition=\"!Exists('$(AetherCoreSdkProject)')\"\n"
		         "           Text=\"AetherCore SDK not found at '$(AetherCoreSdkProject)'. This project was created against an engine checkout at a different path; set the AETHERCORE_SDK environment variable to this machine's managed/AetherCore/AetherCore.csproj.\" />\n"
		         "  </Target>\n"
		         "\n"
		         "  <ItemGroup>\n"
		         "    <ProjectReference Include=\"$(AetherCoreSdkProject)\"\n"
		         "                      Private=\"false\"\n"
		         "                      ExcludeAssets=\"runtime\" />\n"
		         "  </ItemGroup>\n"
		         "\n"
		         "</Project>\n";
	}

	namespace
	{
		// A solution-relative path when one exists on the same root, else absolute.
		// Keeps in-repo projects portable (../../managed/...) while still working for
		// projects created elsewhere.
		std::string SolutionProjectPath(const std::filesystem::path& target, const std::filesystem::path& slnxDir)
		{
			std::error_code ec;
			const std::filesystem::path rel = std::filesystem::relative(target, slnxDir, ec);
			if (!ec && !rel.empty())
			{
				return rel.generic_string();
			}
			return AbsolutePath(target).generic_string();
		}
	} // namespace

	std::string MakeGameSolutionText(const std::filesystem::path& root, const std::filesystem::path& managedSdkProject)
	{
		const std::filesystem::path slnxDir = AbsolutePath(root);
		const std::string gamePath = SolutionProjectPath(slnxDir / "scripts" / "AetherGame.csproj", slnxDir);
		const std::string sdkPath = SolutionProjectPath(AbsolutePath(managedSdkProject), slnxDir);
		return "<Solution>\n"
		       "  <!-- Generated by the AetherCore editor on project open. Open this in\n"
		       "       Visual Studio to edit game scripts with the engine SDK resolved. -->\n"
		       "  <Configurations>\n"
		       "    <Platform Name=\"Any CPU\" />\n"
		       "    <Platform Name=\"x64\" />\n"
		       "  </Configurations>\n"
		       "  <Project Path=\""
		       + EscapeXmlAttribute(gamePath)
		       + "\" />\n"
		         "  <Project Path=\""
		       + EscapeXmlAttribute(sdkPath)
		       + "\" />\n"
		         "</Solution>\n";
	}

	bool EnsureGameSolution(const std::filesystem::path& root, std::string& error)
	{
		const std::filesystem::path managedSdkProject = ManagedSdkProjectPath();
		if (root.empty() || managedSdkProject.empty())
		{
			return true; // nothing to generate (e.g. no managed SDK configured)
		}
		const std::filesystem::path slnx = AbsolutePath(root) / "AetherGame.slnx";
		const std::string content = MakeGameSolutionText(root, managedSdkProject);
		// Only rewrite when the content actually changes, to avoid churn / touching
		// the file (and its VS load) on every project open.
		if (const auto existing = io::file_util::ReadText(slnx); existing && *existing == content)
		{
			return true;
		}
		if (auto writeResult = io::file_util::WriteText(slnx, content); !writeResult)
		{
			error = "Could not write the game solution: " + writeResult.error().message;
			return false;
		}
		return true;
	}

	std::string_view TemplateName(ProjectTemplate projectTemplate)
	{
		switch (projectTemplate)
		{
			case ProjectTemplate::Blank2D:
				return "blank_2d";
			case ProjectTemplate::Multiplayer2D:
				return "multiplayer_2d";
			case ProjectTemplate::Multiplayer3D:
				return "multiplayer_3d";
			case ProjectTemplate::Blank3D:
			default:
				return "blank_3d";
		}
	}

	std::optional<ProjectTemplate> ProjectTemplateFromName(std::string_view name)
	{
		for (const ProjectTemplateInfo& info: kProjectTemplates)
		{
			if (TemplateName(info.value) == name)
			{
				return info.value;
			}
		}
		return std::nullopt;
	}

	bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error, ProjectTemplate projectTemplate)
	{
		if (auto dirResult = io::file_util::CreateDirectories(root); !dirResult)
		{
			error = "Could not create project directory: " + dirResult.error().message;
			return false;
		}

		for (const std::string_view dir: {"assets"sv, "assets/models"sv, "assets/materials"sv, "assets/textures"sv, "assets/animations"sv, "assets/prefabs"sv, "data"sv, "scenes"sv, "scripts"sv})
		{
			if (auto dirResult = io::file_util::CreateDirectories(root / std::filesystem::path(dir)); !dirResult)
			{
				error = "Could not create project folder: " + dirResult.error().message;
				return false;
			}
		}

		const bool is2D = ProjectKindForTemplate(projectTemplate) == ProjectKind::Scene2D;
		const std::string descriptor = "# AetherCore project file.\n\n"
		                               "[project]\nversion = 2\nname = \""
		                               + EscapeTomlString(name) + "\"\nkind = \"" + (is2D ? "2d" : "3d") + "\"\ntemplate = \"" + std::string(TemplateName(projectTemplate))
		                               + "\"\n\n"
		                                 "[paths]\nassets = \"assets\"\nscenes = \"scenes\"\nprefabs = \"assets/prefabs\"\nscripts = \"scripts\"\n\n"
		                                 "[app]\nstartupScene = \"default\"\n\n"
		                                 "[publish]\nplatformName = \"Windows\"\nproductName = \""
		                               + EscapeTomlString(name) + "\"\n";
		if (auto writeResult = io::file_util::WriteText(ProjectFilePath(root), descriptor); !writeResult)
		{
			error = "Could not write ProjectSettings.toml.";
			return false;
		}
		return SeedProjectTemplateFiles(root, projectTemplate, error);
	}

#ifdef _WIN32
	std::optional<std::filesystem::path> PickProjectFolder()
	{
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool uninitialize = SUCCEEDED(coInit);

		IFileDialog* dialog = nullptr;
		HRESULT const hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		if (FAILED(hr) || dialog == nullptr)
		{
			if (uninitialize)
			{
				CoUninitialize();
			}
			return std::nullopt;
		}

		DWORD options = 0;
		if (SUCCEEDED(dialog->GetOptions(&options)))
		{
			dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		}
		dialog->SetTitle(L"Select AetherCore Project Folder");

		std::optional<std::filesystem::path> selected;
		if (SUCCEEDED(dialog->Show(nullptr)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
			{
				PWSTR rawPath = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr)
				{
					selected = std::filesystem::path(rawPath);
					CoTaskMemFree(rawPath);
				}
				item->Release();
			}
		}

		dialog->Release();
		if (uninitialize)
		{
			CoUninitialize();
		}
		return selected;
	}

	std::optional<std::filesystem::path> PickProjectFile()
	{
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool uninitialize = SUCCEEDED(coInit);

		IFileDialog* dialog = nullptr;
		HRESULT const hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		if (FAILED(hr) || dialog == nullptr)
		{
			if (uninitialize)
			{
				CoUninitialize();
			}
			return std::nullopt;
		}

		DWORD options = 0;
		if (SUCCEEDED(dialog->GetOptions(&options)))
		{
			dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
		}
		const COMDLG_FILTERSPEC filters[] = {{L"AetherCore project", L"ProjectSettings.toml"}, {L"TOML files", L"*.toml"}};
		dialog->SetFileTypes(2, filters);
		dialog->SetFileName(L"ProjectSettings.toml");
		dialog->SetTitle(L"Select ProjectSettings.toml");

		std::optional<std::filesystem::path> selected;
		if (SUCCEEDED(dialog->Show(nullptr)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
			{
				PWSTR rawPath = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr)
				{
					selected = std::filesystem::path(rawPath);
					CoTaskMemFree(rawPath);
				}
				item->Release();
			}
		}

		dialog->Release();
		if (uninitialize)
		{
			CoUninitialize();
		}
		return selected;
	}

	void OpenPathInFileManager(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (path.empty() || !std::filesystem::exists(path, ec))
		{
			return;
		}
		const std::filesystem::path folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
		ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
#else
	std::optional<std::filesystem::path> PickProjectFolder()
	{
		return std::nullopt;
	}

	std::optional<std::filesystem::path> PickProjectFile()
	{
		return std::nullopt;
	}

	void OpenPathInFileManager(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (path.empty() || !std::filesystem::exists(path, ec))
		{
			return;
		}
		const std::filesystem::path folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
		const std::string cmd = "xdg-open \"" + folder.string() + "\" >/dev/null 2>&1 &";
		std::system(cmd.c_str());
	}
#endif

	std::vector<EditorProjectContext> LoadRecentProjects(TomlConfig& config)
	{
		std::vector<EditorProjectContext> recents;
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			EditorProjectContext project;
			project.root = NormalizePath(config.GetString(key + ".path"));
			project.name = config.GetString(key + ".name");
			if (project.root.empty())
			{
				continue;
			}
			if (project.name.empty())
			{
				project.name = ReadProjectName(project.root);
			}
			if (std::ranges::none_of(recents, [&](const EditorProjectContext& existing) { return NormalizePath(existing.root) == project.root; }))
			{
				recents.push_back(std::move(project));
			}
		}
		return recents;
	}

	void SaveRecentProjects(TomlConfig& config, std::span<const EditorProjectContext> recents)
	{
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			if (i < static_cast<int>(recents.size()))
			{
				config.Set(key + ".path", DisplayPath(recents[static_cast<std::size_t>(i)].root));
				config.Set(key + ".name", recents[static_cast<std::size_t>(i)].name);
			}
			else
			{
				config.Set(key + ".path", std::string_view{});
				config.Set(key + ".name", std::string_view{});
			}
		}
	}
} // namespace aether::app::project
