using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Level select. The engine's UiNavigationSystem handles ←/→ movement over the
/// node UI Selectables and skips the locked (non-interactable) ones; this script styles the
/// nodes by focus, mirrors the focused node into the detail panel, and loads on activation.</summary>
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
    private Entity _name, _stats, _flavor;
    private int _shown = -1;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 Dim = new(0.106f, 0.125f, 0.188f, 1f);
    private static readonly Vector4 LockedCol = new(0.094f, 0.102f, 0.133f, 1f);
    private static readonly Vector4 NumOnDark = new(0.04f, 0.06f, 0.10f, 1f);
    private static readonly Vector4 NumMuted = new(0.6f, 0.63f, 0.7f, 1f);
    private static readonly Vector4 NumLocked = new(0.35f, 0.37f, 0.43f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<LevelSelectScreen>.Register("LevelSelectRoot", this);
        for (int i = 0; i < 6; i++)
        {
            _nodes[i] = Scene.Find($"Node{i}");
            _nums[i] = Scene.Find($"NodeNum{i}");
        }
        _name = Scene.Find("LSDetailName");
        _stats = Scene.Find("LSDetailStats");
        _flavor = Scene.Find("LSDetailFlavor");
    }

    public void OnShown()
    {
        if (_nodes[0].IsValid) Ui.SetFocus(_nodes[0]);
        _shown = -1;
    }

    public void HandleInput()
    {
        for (int i = 0; i < 6; i++)
        {
            if (!Nodes[i].Locked && _nodes[i].IsValid && Ui.WasActivated(_nodes[i]))
            {
                Log.Info($"[INKBOUND] descend to {Nodes[i].Scene}");
                Scene.Load(Nodes[i].Scene);
            }
        }
    }

    public override void OnUpdate(float dt)
    {
        int focused = -1;
        for (int i = 0; i < 6; i++)
        {
            bool on = _nodes[i].IsValid && Ui.IsFocused(_nodes[i]);
            if (on) focused = i;
            if (_nodes[i].IsValid)
                Ui.SetImageColor(_nodes[i], on ? Cyan : (Nodes[i].Locked ? LockedCol : Dim));
            if (_nums[i].IsValid)
                Ui.SetTextColor(_nums[i], on ? NumOnDark : (Nodes[i].Locked ? NumLocked : NumMuted));
        }

        if (focused >= 0 && focused != _shown)
        {
            _shown = focused;
            if (_name.IsValid) Ui.SetText(_name, Nodes[focused].Name);
            if (_stats.IsValid) Ui.SetText(_stats, Nodes[focused].Stats);
            if (_flavor.IsValid) Ui.SetText(_flavor, Nodes[focused].Flavor);
        }
    }
}
