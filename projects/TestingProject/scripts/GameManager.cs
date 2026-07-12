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
    private int _collected;
    private float _timeLeft;
    private bool _running;

    private static readonly Vector4 Amber = new(1.0f, 0.58f, 0.16f, 1.0f);
    private static readonly Vector4 Ink = new(0.9f, 0.95f, 1.0f, 1.0f);

    public override void OnAttach()
    {
        _orbTag = Tags.Create("orb");
        _playerTag = Tags.Create("player");
        BuildHud();
        Events.Subscribe("orb.collected", _ => OnOrbCollected());
        StartRound();
    }

    // ── HUD (built entirely from script) ──────────────────────────────────────────

    private void BuildHud()
    {
        Entity canvas = Ui.CreateCanvas();

        _title = MakeText(canvas, "ORB COLLECTOR", 0, 42, 640, 60, Amber, 46);
        _score = MakeText(canvas, "Orbs 0 / " + OrbCount, 0, 112, 420, 40, Ink, 28);

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

        _hint = MakeText(canvas, "WASD to move  ·  gather the orbs before the timer runs out", 0, -28, 760, 30, new Vector4(0.72f, 0.68f, 0.6f, 1.0f), 22);
        Ui.SetAnchors(_hint, new Vector2(0.5f, 1.0f), new Vector2(0.5f, 1.0f));
        Ui.SetPivot(_hint, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_hint, 0, -28, 760, 30);

        // Respawn button (a panel with a label; polled in OnUpdate).
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

    // ── Round flow ────────────────────────────────────────────────────────────────

    private void StartRound()
    {
        _collected = 0;
        _timeLeft = TimeLimit;
        _running = true;
        Ui.SetText(_title, "ORB COLLECTOR");
        Ui.SetText(_score, $"Orbs 0 / {OrbCount}");
        SpawnWave();
        StartCoroutine(Countdown());
    }

    private void SpawnWave()
    {
        for (int i = 0; i < OrbCount; i++)
        {
            float angle = (float)i / OrbCount * Mathf.PI * 2.0f + Random.Range(-0.35f, 0.35f);
            float radius = Random.Range(ArenaRadius * 0.4f, ArenaRadius);
            Vector3 pos = new(Mathf.Cos(angle) * radius, 1.2f, Mathf.Sin(angle) * radius);
            // Tag on spawn so collection never depends on the Orb script's attach
            // timing; the Orb script still handles the bob / spin / glow visuals.
            Entity orb = Scene.Instantiate("Orb", pos);
            if (orb.IsValid)
            {
                Tags.Add(orb, _orbTag);
            }
        }
    }

    private IEnumerator Countdown()
    {
        while (_running && _timeLeft > 0.0f)
        {
            yield return null;
            _timeLeft -= Time.DeltaTime;
            float frac = Mathf.Clamp01(_timeLeft / TimeLimit);
            Ui.SetRect(_timerBar, 0, 168, 414.0f * frac, 14);
            Ui.SetImageColor(_timerBar, new Vector4(1.0f - frac, 0.25f + 0.6f * frac, 0.22f, 1.0f));
        }
        if (_running && _timeLeft <= 0.0f)
        {
            EndRound("TIME UP");
        }
    }

    private void OnOrbCollected()
    {
        _collected++;
        Ui.SetText(_score, $"Orbs {_collected} / {OrbCount}");
        if (_collected >= OrbCount)
        {
            EndRound("ALL COLLECTED!");
        }
    }

    private void EndRound(string message)
    {
        _running = false;
        Ui.SetText(_title, message);
    }

    // ── Per-frame: collect, button, debug ─────────────────────────────────────────

    public override void OnUpdate(float dt)
    {
        if (Ui.WasClicked(_respawn))
        {
            DespawnOrbs();
            StartRound();
        }

        Entity player = FirstWithTag(_playerTag);
        if (!player.IsValid)
        {
            return;
        }
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
        // Only visible when Debug.DrawEnabled - the arena ring + collect radius.
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
        System.Span<Entity> orbs = stackalloc Entity[64];
        int n = Tags.GetEntitiesWith(_orbTag, orbs);
        for (int i = 0; i < n; i++)
        {
            orbs[i].Destroy();
        }
    }

    private static Entity FirstWithTag(TagId tag)
    {
        System.Span<Entity> one = stackalloc Entity[1];
        return Tags.GetEntitiesWith(tag, one) > 0 ? one[0] : default;
    }
}
