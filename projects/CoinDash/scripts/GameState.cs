using AetherCore;

namespace AetherGame;

/// <summary>
/// Shared run state. Static fields live in the managed assembly, not in any
/// scene, so they survive Scene.Load - this is how data crosses levels
/// (the same pattern as a static game manager in Unity). Coins reset per
/// level; TotalCoins accumulates for the whole run.
/// </summary>
public static class GameState
{
    /// <summary>Coins collected in the current level.</summary>
    public static int Coins;

    /// <summary>Coins collected across every level this run.</summary>
    public static int TotalCoins;

    public static bool Won;

    /// <summary>True while a next-level load is pending (drives HUD copy).</summary>
    public static bool NextSceneQueued;

    /// <summary>Called by the player on level start: per-level state resets, run totals persist.</summary>
    public static void BeginLevel()
    {
        Coins = 0;
        Won = false;
        NextSceneQueued = false;
    }

    /// <summary>Reset everything, including run totals (fresh play session).</summary>
    public static void ResetRun()
    {
        BeginLevel();
        TotalCoins = 0;
    }

    public static void CollectCoin()
    {
        Coins++;
        TotalCoins++;
        Log.Info($"[CoinDash] Coin {Coins} collected! ({TotalCoins} this run)");
    }

    public static void Win()
    {
        if (Won)
        {
            return;
        }
        Won = true;
        Log.Info($"[CoinDash] Level complete with {Coins} coins ({TotalCoins} this run)!");
    }
}
