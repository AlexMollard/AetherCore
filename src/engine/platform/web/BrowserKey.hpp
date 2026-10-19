#pragma once

#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace aether::web
{
	inline int BrowserKey(std::string_view code)
	{
		if (code.size() == 4 && code.starts_with("Key") && code[3] >= 'A' && code[3] <= 'Z')
		{
			return code[3];
		}
		if (code.size() == 6 && code.starts_with("Digit") && code[5] >= '0' && code[5] <= '9')
		{
			return code[5];
		}
		if (code.size() == 7 && code.starts_with("Numpad") && code[6] >= '0' && code[6] <= '9')
		{
			return 320 + code[6] - '0';
		}
		if (code.starts_with("F"))
		{
			int number = 0;
			const auto parsed = std::from_chars(code.data() + 1, code.data() + code.size(), number);
			if (parsed.ec == std::errc{} && parsed.ptr == code.data() + code.size() && number >= 1 && number <= 12)
			{
				return 289 + number;
			}
		}
		constexpr std::pair<std::string_view, int> named[]{{"Space", 32},
		        {"Quote", 39},
		        {"Comma", 44},
		        {"Minus", 45},
		        {"Period", 46},
		        {"Slash", 47},
		        {"Semicolon", 59},
		        {"Equal", 61},
		        {"BracketLeft", 91},
		        {"Backslash", 92},
		        {"BracketRight", 93},
		        {"Backquote", 96},
		        {"Escape", 256},
		        {"Enter", 257},
		        {"Tab", 258},
		        {"Backspace", 259},
		        {"Insert", 260},
		        {"Delete", 261},
		        {"ArrowRight", 262},
		        {"ArrowLeft", 263},
		        {"ArrowDown", 264},
		        {"ArrowUp", 265},
		        {"PageUp", 266},
		        {"PageDown", 267},
		        {"Home", 268},
		        {"End", 269},
		        {"CapsLock", 280},
		        {"ScrollLock", 281},
		        {"NumLock", 282},
		        {"PrintScreen", 283},
		        {"Pause", 284},
		        {"NumpadDecimal", 330},
		        {"NumpadDivide", 331},
		        {"NumpadMultiply", 332},
		        {"NumpadSubtract", 333},
		        {"NumpadAdd", 334},
		        {"NumpadEnter", 335},
		        {"NumpadEqual", 336},
		        {"ShiftLeft", 340},
		        {"ControlLeft", 341},
		        {"AltLeft", 342},
		        {"MetaLeft", 343},
		        {"ShiftRight", 344},
		        {"ControlRight", 345},
		        {"AltRight", 346},
		        {"MetaRight", 347},
		        {"ContextMenu", 348}};
		for (const auto& [name, key]: named)
		{
			if (code == name)
			{
				return key;
			}
		}
		return -1;
	}
} // namespace aether::web
