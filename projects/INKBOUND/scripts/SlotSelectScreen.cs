using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>The 3-slot save screen, entered by both 'descend' (new-run mode) and 'return'
/// (resume-only mode). New-run mode lets you start/overwrite any slot (used slots ask to confirm);
/// resume mode only lets you continue a used slot. Styled with ui_ink_ui to match the menu suite.</summary>
public sealed class SlotSelectScreen : EntityScript, IMenuScreen
{
    private static bool s_resumeOnly;
    /// <summary>Set the mode, then switch to the screen. Called from Title.</summary>
    public static void Open(bool resumeOnly)
    {
        s_resumeOnly = resumeOnly;
        MenuController.Instance?.Go(MenuScreen.SlotSelect);
    }

    private readonly Entity[] _slots = new Entity[SaveSystem.SlotCount];
    private readonly Entity[] _texts = new Entity[SaveSystem.SlotCount];
    private Entity _confirm, _confirmYes, _confirmNo;
    private int _pendingOverwrite = -1;   // slot awaiting overwrite confirmation, -1 = none
    private float _t;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 Dim = new(0.106f, 0.125f, 0.188f, 1f);
    private static readonly Vector4 InkEdge = new(0.02f, 0.06f, 0.09f, 1f);
    private static readonly Vector4 TextMuted = new(0.6f, 0.63f, 0.7f, 1f);
    private static readonly Vector4 TextOn = new(0.04f, 0.06f, 0.10f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<SlotSelectScreen>.Register("SlotSelectRoot", this);
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            _slots[i] = Scene.Find($"Slot{i}");
            _texts[i] = Scene.Find($"SlotText{i}");
            ApplyInk(_slots[i]);
        }
        _confirm = Scene.Find("SlotConfirm");
        _confirmYes = Scene.Find("SlotConfirmYes");
        _confirmNo = Scene.Find("SlotConfirmNo");
    }

    private static void ApplyInk(Entity e)
    {
        if (!e.IsValid) return;
        Ui.SetMaterial(e, "ui_ink_ui");
        Ui.SetMaterialColors(e, Vector4.Zero, InkEdge);
    }

    public void OnShown()
    {
        SaveSystem.EnsureLoaded();
        _pendingOverwrite = -1;
        if (_confirm.IsValid) _confirm.SetActive(false);
        RefreshLabels();
        // Focus the first selectable slot (in resume mode, the first USED slot).
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (_slots[i].IsValid && Selectable(i)) { Ui.SetFocus(_slots[i]); break; }
        }
    }

    private bool Selectable(int i) => !s_resumeOnly || SaveSystem.Slot(i).Exists;

    private void RefreshLabels()
    {
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_texts[i].IsValid) continue;
            SaveProfile p = SaveSystem.Slot(i);
            string label;
            if (!p.Exists) label = $"SLOT {Roman(i)}   empty - a new dark";
            else label = $"SLOT {Roman(i)}   {LevelName(p.FurthestLevel)} - {p.CompletedCount()}/4 done - {SlotCoins(p)} coins";
            Ui.SetText(_texts[i], label);
            Ui.SetInteractable(_slots[i], Selectable(i));
        }
    }

    private static int SlotCoins(SaveProfile p)
    {
        int c = 0;
        foreach (string k in SaveProfile.LevelKeys) c += p.Peek(k).BestCoins;
        return c;
    }

    private static string Roman(int i) => i == 0 ? "I" : i == 1 ? "II" : "III";
    private static string LevelName(string key) => key switch
    {
        "Level1" => "1-1 The Cheerful Plunge",
        "Level2" => "1-2 Quiet, Please",
        "Level3" => "1-3 The Hollow Descent",
        "Level4" => "1-4 The Fourth Descent",
        _ => key,
    };

    public void HandleInput()
    {
        // Overwrite confirmation takes priority when open.
        if (_pendingOverwrite >= 0)
        {
            if (_confirmYes.IsValid && Ui.WasActivated(_confirmYes)) { StartFresh(_pendingOverwrite); }
            else if (_confirmNo.IsValid && Ui.WasActivated(_confirmNo)) { _pendingOverwrite = -1; if (_confirm.IsValid) _confirm.SetActive(false); OnShown(); }
            return;
        }

        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_slots[i].IsValid || !Selectable(i) || !Ui.WasActivated(_slots[i])) continue;
            SaveProfile p = SaveSystem.Slot(i);
            if (s_resumeOnly) { Resume(i); return; }
            if (p.Exists)
            {
                _pendingOverwrite = i;
                if (_confirm.IsValid) _confirm.SetActive(true);
                // Disable the rows so nav/hover can't steal focus back off the dialog, then move
                // focus onto the default confirm button (the nav system won't focus a
                // newly-shown element on its own).
                for (int j = 0; j < SaveSystem.SlotCount; j++) if (_slots[j].IsValid) Ui.SetInteractable(_slots[j], false);
                if (_confirmYes.IsValid) Ui.SetFocus(_confirmYes);
                return;
            }
            StartFresh(i);
            return;
        }
    }

    private void StartFresh(int i)
    {
        _pendingOverwrite = -1;
        SaveSystem.Wipe(i);
        SaveSystem.SetActive(i);
        GameState.TrialMode = false;
        GameState.ResumeLevel = "";
        GameState.ResumeCheckpoint = 0;
        Log.Info($"[INKBOUND] new dark in slot {i}");
        MenuController.Instance?.Go(MenuScreen.LevelSelect);
    }

    private void Resume(int i)
    {
        SaveSystem.SetActive(i);
        SaveProfile p = SaveSystem.Slot(i);
        GameState.TrialMode = false;
        GameState.ResumeLevel = p.FurthestLevel;
        GameState.ResumeCheckpoint = p.Level(p.FurthestLevel).FurthestCheckpoint;
        Log.Info($"[INKBOUND] resume slot {i} at {p.FurthestLevel} cp {GameState.ResumeCheckpoint}");
        MenuController.Instance?.Go(MenuScreen.LevelSelect);
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        Vector4 mp = new(_t, 0f, 0f, 0f);
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_slots[i].IsValid) continue;
            Ui.SetMaterialParams(_slots[i], mp);
            bool on = Ui.IsFocused(_slots[i]);
            Vector4 col = Selectable(i) ? (on ? Cyan : Dim) : new Vector4(0.06f, 0.07f, 0.09f, 1f);
            if (on) col.W = 0.78f + 0.22f * MathF.Sin(_t * 4.2f);
            Ui.SetImageColor(_slots[i], col);
            if (_texts[i].IsValid) Ui.SetTextColor(_texts[i], on ? TextOn : (Selectable(i) ? TextMuted : new Vector4(0.3f, 0.32f, 0.38f, 1f)));
        }
    }
}
