using System;
using System.Globalization;
using System.Numerics;
using AetherCore;

public sealed class ReferencePlayer : EntityScript
{
    public int ticks;
    public int score;
    public bool collision;
    public bool trigger;
    public Entity spawned;

    public override void OnUpdate(float deltaTime)
    {
        ticks++;
        Physics2D.SetLinearVelocity(Self, new Vector2(2, 0));
        if (Input.IsKeyDown(Key.Space)) score++;
        Ui.SetText(Scene.Find("Counter"), $"Score {score}");
        if (ticks == 5)
        {
            spawned = Scene.Instantiate("ReferencePickup");
            if (!spawned.IsValid) throw new InvalidOperationException("Prefab failed to spawn.");
            if (Audio.Play("project://assets/audio/reference.wav") == 0)
                throw new InvalidOperationException("Reference audio failed to start.");
        }
        if (ticks == 15) spawned.Destroy();
        if (ticks == 60)
        {
            SaveData.Write("reference_score", $"{score},{collision.ToString().ToLowerInvariant()},{trigger.ToString().ToLowerInvariant()}");
            SaveData.Write("reference_label", "caf\u00e9\0agent");
            Scene.Load("ReferenceResult");
        }
    }

    public override void OnCollisionEnter2D(Entity other) => collision = true;
    public override void OnTriggerEnter2D(Entity other) => trigger = true;
}

public sealed class ReferenceResult : EntityScript
{
    public override void OnDetach()
    {
        if (Environment.GetEnvironmentVariable("AETHER_REFERENCE_DETACH_FAILURE") == "1")
            throw new InvalidOperationException("Reference detach failure");
    }
    public int ticks;
    public int score;
    public bool collision;
    public bool trigger;
    public string label = "";

    public override void OnAttach()
    {
        if (!SaveData.TryRead("reference_score", out string saved))
            throw new InvalidOperationException("Reference save is unavailable.");
        string[] parts = saved.Split(',');
        if (parts.Length != 3) throw new InvalidOperationException("Invalid reference score.");
        score = int.Parse(parts[0], CultureInfo.InvariantCulture);
        collision = parts[1] == "true";
        trigger = parts[2] == "true";
        if (!SaveData.TryRead("reference_label", out label))
            throw new InvalidOperationException("Reference label is unavailable.");
        Ui.SetText(Scene.Find("Counter"), $"Saved score {score}");
    }

    public override void OnUpdate(float deltaTime) => ticks++;
}
