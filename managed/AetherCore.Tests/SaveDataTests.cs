using System;
using Xunit;

namespace AetherCore.Tests;

public sealed class SaveDataTests
{
    [Theory]
    [InlineData("")]
    [InlineData("../score")]
    [InlineData("score.txt")]
    [InlineData("score/next")]
    [InlineData("caf\u00e9")]
    public void RejectsInvalidKeysBeforeCallingNativeCode(string key)
    {
        Assert.Throws<ArgumentException>(() => SaveData.Write(key, "42"));
        Assert.Throws<ArgumentException>(() => SaveData.TryRead(key, out _));
    }

    [Fact]
    public void RejectsOversizedAndNullArgumentsBeforeCallingNativeCode()
    {
        Assert.Throws<ArgumentException>(() => SaveData.Write(new string('a', 129), "42"));
        Assert.Throws<ArgumentNullException>(() => SaveData.Write(null!, "42"));
        Assert.Throws<ArgumentNullException>(() => SaveData.Write("score", null!));
    }
}
