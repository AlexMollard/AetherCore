using System.Collections;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Level-end trigger at the flag pole. When <see cref="NextScene"/> is set,
/// the next level loads after a short victory pause.
/// </summary>
public sealed class GoalFlag : EntityScript
{
    public string NextScene = "";
    public float NextSceneDelay = 1.2f;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id || GameState.Won)
        {
            return;
        }
        GameState.NextSceneQueued = NextScene.Length > 0;
        GameState.Win();
        if (NextScene.Length > 0)
        {
            StartCoroutine(LoadNextScene());
        }
    }

    private IEnumerator LoadNextScene()
    {
        yield return new WaitForSeconds(NextSceneDelay);
        Log.Info($"[CoinDash] Loading '{NextScene}'...");
        Scene.Load(NextScene);
    }
}
