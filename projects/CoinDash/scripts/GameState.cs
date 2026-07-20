using AetherCore;

namespace AetherGame;

/// <summary>Shared run state: coin tally and win/dead flags for the level.</summary>
public static class GameState
{
    public static int Coins;
    public static bool Won;

    public static void Reset()
    {
        Coins = 0;
        Won = false;
    }

    public static void CollectCoin()
    {
        Coins++;
        Log.Info($"[CoinDash] Coin {Coins} collected!");
    }

    public static void Win()
    {
        if (Won)
        {
            return;
        }
        Won = true;
        Log.Info($"[CoinDash] Level complete with {Coins} coins!");
    }
}
