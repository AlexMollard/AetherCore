using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Level select. The engine's UiNavigationSystem handles ←/→ movement over the
/// node UI Selectables and skips the locked (non-interactable) ones; this script styles the
/// nodes by focus, mirrors the focused node into the detail panel, and loads on activation.
/// The nodes, connector line and preview panel carry the ui_ink_ui material so the screen reads
/// as brushed pixel-ink like the title/settings screens rather than clean vector.</summary>
public sealed class LevelSelectScreen : EntityScript, IMenuScreen
{
    private readonly struct Node
    {
        public readonly string Name, Scene, Stats, Flavor;
        public readonly bool Locked;
        public Node(string name, string scene, string stats, string flavor, bool locked)
        {
            Name = name; Scene = scene; Stats = stats; Flavor = flavor; Locked = locked;
        }
    }

    private static readonly Node[] Nodes =
    {
        new("1-1  The Cheerful Plunge", "Level1", "INK 3/5     PAR 01:10", "you were smiling when you fell in.", false),
        new("1-2  Quiet, Please", "Level2", "INK 4/5     PAR 01:25", "the dark prefers you keep your voice down.", false),
        new("1-3  The Hollow Descent", "Level3", "INK 4/5     PAR 01:40", "the walls here still remember every route you've drawn.", false),
        new("1-4  The Fourth Descent", "Level4", "INK 5/5     PAR 02:05", "you don't remember a fourth. and yet.", false),
        new("it's not ready for you yet", "", "INK -/-     PAR --:--", "best not to look too closely.", true),
        new("best not to think about this one", "", "INK -/-     PAR --:--", "...", true),
    };

    private readonly Entity[] _nodes = new Entity[6];
    private readonly Entity[] _nums = new Entity[6];
    private Entity _path, _preview;
    private Entity _name, _stats, _flavor;
    private int _shown = -1;
    private float _t;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 Dim = new(0.106f, 0.125f, 0.188f, 1f);
    private static readonly Vector4 LockedCol = new(0.094f, 0.102f, 0.133f, 1f);
    private static readonly Vector4 NumOnDark = new(0.04f, 0.06f, 0.10f, 1f);
    private static readonly Vector4 NumMuted = new(0.6f, 0.63f, 0.7f, 1f);
    private static readonly Vector4 NumLocked = new(0.35f, 0.37f, 0.43f, 1f);

    // Dark teal ink rim shared with the settings widgets, so the node blots read as brushed ink.
    private static readonly Vector4 InkEdge = new(0.02f, 0.06f, 0.09f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<LevelSelectScreen>.Register("LevelSelectRoot", this);
        for (int i = 0; i < 6; i++)
        {
            _nodes[i] = Scene.Find($"Node{i}");
            _nums[i] = Scene.Find($"NodeNum{i}");
        }
        _path = Scene.Find("LSPathLine");
        _preview = Scene.Find("LSDetailPreview");
        _name = Scene.Find("LSDetailName");
        _stats = Scene.Find("LSDetailStats");
        _flavor = Scene.Find("LSDetailFlavor");

        // Ink the shapes (not the numbers/text): the nodes become pixel-ink blots and the preview
        // panel a wet-ink frame. The thin connector line darkens its own colour (edge = 0) so it stays
        // a visible brushed stroke instead of dissolving into the dark rim like the chunky shapes do.
        foreach (Entity n in _nodes) ApplyInk(n, InkEdge);
        ApplyInk(_preview, InkEdge);
        ApplyInk(_path, Vector4.Zero);
    }

    private static void ApplyInk(Entity e, Vector4 edge)
    {
        if (!e.IsValid) return;
        Ui.SetMaterial(e, "ui_ink_ui");
        Ui.SetMaterialColors(e, Vector4.Zero, edge);
    }

    public void OnShown()
    {
        if (_nodes[0].IsValid) Ui.SetFocus(_nodes[0]);
        _shown = -1;
    }

    // A real level (index 0-3) is locked when the active slot has not unlocked it; the two
    // placeholder nodes (4-5) are always locked flavor.
    private static bool IsLocked(int i)
    {
        if (Nodes[i].Scene.Length == 0) return true;              // placeholder nodes
        SaveProfile? p = SaveSystem.Active;
        return p != null && !p.IsUnlocked(Nodes[i].Scene);
    }

    private static bool IsDone(int i)
    {
        SaveProfile? p = SaveSystem.Active;
        return Nodes[i].Scene.Length > 0 && p != null && p.Level(Nodes[i].Scene).Completed;
    }

    private static string FormatTime(float s)
    {
        int total = (int)s;
        return $"{total / 60:00}:{total % 60:00}";
    }

    public void HandleInput()
    {
        for (int i = 0; i < 6; i++)
        {
            if (!IsLocked(i) && _nodes[i].IsValid && Ui.WasActivated(_nodes[i]))
            {
                Log.Info($"[INKBOUND] descend to {Nodes[i].Scene}");
                Scene.Load(Nodes[i].Scene);
            }
        }
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;

        // Drift the ink grain so the blots/panel shimmer slowly (material params.x = time).
        Vector4 p = new(_t, 0f, 0f, 0f);
        foreach (Entity n in _nodes) if (n.IsValid) Ui.SetMaterialParams(n, p);
        if (_path.IsValid) Ui.SetMaterialParams(_path, p);
        if (_preview.IsValid) Ui.SetMaterialParams(_preview, p);

        int focused = -1;
        for (int i = 0; i < 6; i++)
        {
            bool on = _nodes[i].IsValid && Ui.IsFocused(_nodes[i]);
            if (on) focused = i;
            if (_nodes[i].IsValid)
            {
                Vector4 col;
                if (on)
                {
                    col = Cyan;
                    col.W = 0.78f + 0.22f * MathF.Sin(_t * 4.2f); // focused blot breathes, like the title markers
                }
                else
                {
                    col = IsLocked(i) ? LockedCol : Dim;
                }
                Ui.SetImageColor(_nodes[i], col);
            }
            if (_nums[i].IsValid)
                Ui.SetTextColor(_nums[i], on ? NumOnDark : (IsLocked(i) ? NumLocked : NumMuted));
            if (_nodes[i].IsValid) Ui.SetInteractable(_nodes[i], !IsLocked(i));
        }

        if (focused >= 0 && focused != _shown)
        {
            _shown = focused;
            SaveProfile? prof = SaveSystem.Active;
            string done = IsDone(focused) ? "  DONE" : "";
            if (_name.IsValid) Ui.SetText(_name, Nodes[focused].Name + done);
            string stats = Nodes[focused].Stats;
            if (prof != null && Nodes[focused].Scene.Length > 0)
            {
                LevelRecord r = prof.Level(Nodes[focused].Scene);
                string best = r.BestTimeSeconds == null ? "--:--" : FormatTime(r.BestTimeSeconds.Value);
                stats = $"BEST COINS {r.BestCoins}     TRIAL {best}";
            }
            if (_stats.IsValid) Ui.SetText(_stats, stats);
            if (_flavor.IsValid) Ui.SetText(_flavor, Nodes[focused].Flavor);
        }
    }
}
