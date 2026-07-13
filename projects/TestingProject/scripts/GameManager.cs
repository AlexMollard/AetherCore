using System.Collections;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// "Orb Collector" game loop, exercising the whole system: builds the HUD from
/// script (UI toolkit), spawns orbs from a prefab at random positions, runs a
/// countdown coroutine, collects orbs by proximity, and reacts to an event bus +
/// a UI button + debug drawing.
/// </summary>
public sealed class GameManager : EntityScript
{
	public int OrbCount = 8;
	public float ArenaRadius = 6.0f;
	public float CollectRadius = 1.2f;
	public float TimeLimit = 30.0f;

	private Entity _title, _score, _timerBar, _hint, _respawn;
	private TagId _orbTag, _playerTag;

	// Game State
	private int _collected;
	private int _scoreValue;
	private int _highScore;
	private float _timeLeft;
	private bool _running;
	private int _roundId; // Fixes coroutine leaks on respawn

	// Combo System
	private float _lastCollectTime;
	private int _comboCount;
	private const float ComboWindow = 1.5f;

	private static readonly Vector4 Amber = new(1.0f, 0.58f, 0.16f, 1.0f);
	private static readonly Vector4 Ink = new(0.9f, 0.95f, 1.0f, 1.0f);
	private static readonly Vector4 Danger = new(1.0f, 0.2f, 0.2f, 1.0f);

	public override void OnAttach()
	{
		_orbTag = Tags.Create("orb");
		_playerTag = Tags.Create("player");
		BuildHud();
		Events.Subscribe("orb.collected", _ => OnOrbCollected());
		StartRound();
	}

	// -- HUD (built entirely from script) ----------------------------------------
	private void BuildHud()
	{
		Entity canvas = Ui.CreateCanvas();
		_title = MakeText(canvas, "ORB COLLECTOR", 0, 42, 640, 60, Amber, 46);
		_score = MakeText(canvas, "Score: 0 | Best: 0", 0, 112, 420, 40, Ink, 28);

		// Timer bar: a dark rounded track with a coloured fill on top.
		Entity track = Ui.CreateImage(canvas);
		Anchor(track, 0.5f, 0.0f);
		Ui.SetRect(track, 0, 168, 420, 18);
		Ui.SetImageColor(track, new Vector4(0.10f, 0.09f, 0.08f, 0.85f));
		Ui.SetImageCornerRadius(track, 9);

		_timerBar = Ui.CreateImage(canvas);
		Anchor(_timerBar, 0.5f, 0.0f);
		Ui.SetRect(_timerBar, 0, 168, 414, 14);
		Ui.SetImageColor(_timerBar, new Vector4(0.24f, 0.92f, 0.45f, 1.0f));
		Ui.SetImageCornerRadius(_timerBar, 7);

		// Fixed UTF-8 character (replaced middle dot with hyphen)
		_hint = MakeText(canvas, "WASD to move - gather the orbs before the timer runs out", 0, -28, 760, 30, new Vector4(0.72f, 0.68f, 0.6f, 1.0f), 22);
		Ui.SetAnchors(_hint, new Vector2(0.5f, 1.0f), new Vector2(0.5f, 1.0f));
		Ui.SetPivot(_hint, new Vector2(0.5f, 1.0f));
		Ui.SetRect(_hint, 0, -28, 760, 30);

		// Respawn button
		_respawn = Ui.CreateImage(canvas);
		Ui.SetAnchors(_respawn, new Vector2(1.0f, 0.0f), new Vector2(1.0f, 0.0f));
		Ui.SetPivot(_respawn, new Vector2(1.0f, 0.0f));
		Ui.SetRect(_respawn, -24, 24, 168, 52);
		Ui.SetImageColor(_respawn, new Vector4(0.18f, 0.14f, 0.09f, 0.92f));
		Ui.SetImageCornerRadius(_respawn, 10);

		Entity label = Ui.CreateText(canvas, "RESPAWN");
		label.SetParent(_respawn);
		Anchor(label, 0.5f, 0.5f);
		Ui.SetRect(label, 0, 0, 158, 44);
		Ui.SetTextColor(label, Amber);
		Ui.SetFontSize(label, 24);
		Ui.SetTextAlign(label, UiHAlign.Center, UiVAlign.Middle);
	}

	private static Entity MakeText(Entity canvas, string text, float x, float y, float w, float h, Vector4 color, float size)
	{
		Entity e = Ui.CreateText(canvas, text);
		Anchor(e, 0.5f, 0.0f);
		Ui.SetRect(e, x, y, w, h);
		Ui.SetTextColor(e, color);
		Ui.SetFontSize(e, size);
		Ui.SetTextAlign(e, UiHAlign.Center, UiVAlign.Middle);
		return e;
	}

	private static void Anchor(Entity e, float ax, float ay)
	{
		Ui.SetAnchors(e, new Vector2(ax, ay), new Vector2(ax, ay));
		Ui.SetPivot(e, new Vector2(ax, ay));
	}

	// -- Round flow --------------------------------------------------------------
	private void StartRound()
	{
		_roundId++; // Invalidate any running countdown coroutines from previous rounds
		_collected = 0;
		_scoreValue = 0;
		_comboCount = 0;
		_timeLeft = TimeLimit;
		_running = true;

		Ui.SetText(_title, "ORB COLLECTOR");
		UpdateScoreUI();

		DespawnOrbs(); // Ensure clean slate before spawning
		SpawnWave();
		StartCoroutine(Countdown(_roundId));
	}

	private void SpawnWave()
	{
		for (int i = 0; i < OrbCount; i++)
		{
			float angle = (float)i / OrbCount * Mathf.PI * 2.0f + Random.Range(-0.35f, 0.35f);
			float radius = Random.Range(ArenaRadius * 0.4f, ArenaRadius);
			Vector3 pos = new(Mathf.Cos(angle) * radius, 1.2f, Mathf.Sin(angle) * radius);

			Entity orb = Scene.Instantiate("Orb", pos);
			if (orb.IsValid)
			{
				Tags.Add(orb, _orbTag);
			}
		}
	}

	// Pass the roundId to ensure old coroutines die gracefully if respawn is clicked
	private IEnumerator Countdown(int currentRoundId)
	{
		while (_running && _timeLeft > 0.0f && _roundId == currentRoundId)
		{
			yield return null;
			_timeLeft -= Time.DeltaTime;
			float frac = Mathf.Clamp01(_timeLeft / TimeLimit);
			Ui.SetRect(_timerBar, 0, 168, 414.0f * frac, 14);

			// Color interpolation: Green -> Yellow -> Red
			Vector4 color = new(1.0f - frac, 0.25f + 0.6f * frac, 0.22f, 1.0f);

			// Pulse effect when time is low
			if (_timeLeft < 5.0f)
			{
				float pulse = Mathf.Abs(Mathf.Sin(Time.TotalTime * 8.0f));
				color = Vector4.Lerp(color, Danger, pulse);
			}

			Ui.SetImageColor(_timerBar, color);
		}

		if (_running && _timeLeft <= 0.0f && _roundId == currentRoundId)
		{
			EndRound("TIME UP");
		}
	}

	private void OnOrbCollected()
	{
		_collected++;

		// Combo logic
		float currentTime = Time.TotalTime;
		if (currentTime - _lastCollectTime < ComboWindow)
		{
			_comboCount++;
			if (_comboCount > 2)
			{
				Ui.SetText(_title, $"COMBO x{_comboCount}!");
			}
		}
		else
		{
			_comboCount = 1;
		}
		_lastCollectTime = currentTime;

		// Score calculation with combo multiplier
		int points = 100 * _comboCount;
		_scoreValue += points;

		UpdateScoreUI();

		if (_collected >= OrbCount)
		{
			EndRound("ALL COLLECTED!");
		}
	}

	private void UpdateScoreUI()
	{
		Ui.SetText(_score, $"Score: {_scoreValue} | Best: {_highScore}");
	}

	private void EndRound(string message)
	{
		_running = false;
		if (_scoreValue > _highScore)
		{
			_highScore = _scoreValue;
			message += " (NEW BEST!)";
		}
		Ui.SetText(_title, message);
		UpdateScoreUI();
	}

	// -- Per-frame: collect, button, debug ---------------------------------------
	public override void OnUpdate(float dt)
	{
		if (Ui.WasClicked(_respawn))
		{
			StartRound(); // StartRound now handles despawning internally
		}

		Entity player = FirstWithTag(_playerTag);
		if (!player.IsValid) return;

		Vector3 pp = player.Position;
		System.Span<Entity> orbs = stackalloc Entity[64];
		int n = Tags.GetEntitiesWith(_orbTag, orbs);

		for (int i = 0; i < n; i++)
		{
			if (_running && Vector3.Distance(orbs[i].Position, pp) < CollectRadius)
			{
				orbs[i].Destroy();
				Events.Publish("orb.collected", orbs[i].Id);
			}
		}
		DrawArena(pp);
	}

	private void DrawArena(Vector3 playerPos)
	{
		Vector4 ring = new(0.3f, 0.5f, 0.9f, 1.0f);
		Vector3 prev = new(ArenaRadius, 0.05f, 0.0f);
		for (int i = 1; i <= 40; i++)
		{
			float a = (float)i / 40 * Mathf.PI * 2.0f;
			Vector3 cur = new(Mathf.Cos(a) * ArenaRadius, 0.05f, Mathf.Sin(a) * ArenaRadius);
			Debug.DrawLine(prev, cur, ring);
			prev = cur;
		}
		Debug.DrawSphere(playerPos, CollectRadius, new Vector4(0.24f, 0.92f, 0.45f, 1.0f));
	}

	private void DespawnOrbs()
	{
		System.Span<Entity> orbs = stackalloc Entity[128]; // Increased buffer just in case
		int n = Tags.GetEntitiesWith(_orbTag, orbs);
		for (int i = 0; i < n; i++)
		{
			// Force remove tag first to prevent phantom collections during deferred destruction
			Tags.Remove(orbs[i], _orbTag);
			orbs[i].Destroy();
		}
	}

	private static Entity FirstWithTag(TagId tag)
	{
		System.Span<Entity> one = stackalloc Entity[1];
		return Tags.GetEntitiesWith(tag, one) > 0 ? one[0] : default;
	}
}