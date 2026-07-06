namespace AetherCore;

/// <summary>
/// Keyboard keys, matching the engine's <c>aether::Key</c> (GLFW key codes).
/// </summary>
public enum Key
{
    None = -1,

    // Letters
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73,
    J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80, Q = 81, R = 82,
    S = 83, T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,

    // Numbers
    Num0 = 48, Num1 = 49, Num2 = 50, Num3 = 51, Num4 = 52,
    Num5 = 53, Num6 = 54, Num7 = 55, Num8 = 56, Num9 = 57,

    // Punctuation / symbols
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Semicolon = 59, Equal = 61, LeftBracket = 91, Backslash = 92, RightBracket = 93,
    GraveAccent = 96,

    // Navigation / system
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265, PageUp = 266, PageDown = 267,
    Home = 268, End = 269,

    // Locks
    CapsLock = 280, ScrollLock = 281, NumLock = 282, PrintScreen = 283, Pause = 284,

    // Function keys
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295,
    F7 = 296, F8 = 297, F9 = 298, F10 = 299, F11 = 300, F12 = 301,

    // Keypad
    Keypad0 = 320, Keypad1 = 321, Keypad2 = 322, Keypad3 = 323, Keypad4 = 324,
    Keypad5 = 325, Keypad6 = 326, Keypad7 = 327, Keypad8 = 328, Keypad9 = 329,
    KeypadDecimal = 330, KeypadDivide = 331, KeypadMultiply = 332, KeypadSubtract = 333,
    KeypadAdd = 334, KeypadEnter = 335, KeypadEqual = 336,

    // Modifiers
    LeftShift = 340, LeftCtrl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightCtrl = 345, RightAlt = 346, RightSuper = 347, Menu = 348,
}
