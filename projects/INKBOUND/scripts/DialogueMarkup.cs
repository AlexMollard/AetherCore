using System.Collections.Generic;
using System.Text;

namespace AetherGame;

/// <summary>Per-glyph rich-text effect selectable per run. Ids match ui_dialogue_text.slang.</summary>
public enum InkEffect { Normal = 0, Shake = 1, Wave = 2, Flicker = 3, Whisper = 4, Glitch = 5 }

/// <summary>One contiguous run of body text sharing a single effect.</summary>
public readonly record struct TextRun(string Text, InkEffect Effect);

/// <summary>Splits inline "[effect]...[/effect]" markup into styled runs. Untagged text takes the
/// node-level default effect. Returns the tag-free plain string (for width/typewriter math) and the
/// run list. Unknown tags are treated as literal text (kept, no effect) so authoring typos are visible.</summary>
public static class DialogueMarkup
{
    public static (string Plain, List<TextRun> Runs) Tokenize(string raw, InkEffect nodeDefault)
    {
        var runs = new List<TextRun>();
        var plain = new StringBuilder();
        var cur = new StringBuilder();
        InkEffect curEffect = nodeDefault;

        void Flush()
        {
            if (cur.Length > 0) { runs.Add(new TextRun(cur.ToString(), curEffect)); cur.Clear(); }
        }

        int i = 0;
        while (i < raw.Length)
        {
            if (raw[i] == '[')
            {
                int close = raw.IndexOf(']', i + 1);
                if (close > i)
                {
                    string tag = raw.Substring(i + 1, close - i - 1);
                    if (tag.StartsWith("/") && TryEffect(tag.Substring(1), out _))
                    {
                        Flush(); curEffect = nodeDefault; i = close + 1; continue;
                    }
                    if (TryEffect(tag, out InkEffect eff))
                    {
                        Flush(); curEffect = eff; i = close + 1; continue;
                    }
                }
            }
            cur.Append(raw[i]);
            plain.Append(raw[i]);
            i++;
        }
        Flush();
        if (runs.Count == 0) { runs.Add(new TextRun(string.Empty, nodeDefault)); }
        return (plain.ToString(), runs);
    }

    public static bool TryEffect(string name, out InkEffect effect)
    {
        switch (name)
        {
            case "normal":  effect = InkEffect.Normal;  return true;
            case "shake":   effect = InkEffect.Shake;   return true;
            case "wave":    effect = InkEffect.Wave;    return true;
            case "flicker": effect = InkEffect.Flicker; return true;
            case "whisper": effect = InkEffect.Whisper; return true;
            case "glitch":  effect = InkEffect.Glitch;  return true;
            default:        effect = InkEffect.Normal;  return false;
        }
    }
}
