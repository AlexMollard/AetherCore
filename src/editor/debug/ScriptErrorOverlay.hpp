#pragma once

#include <deque>
#include <string>

namespace aether::app
{
	struct LayerContext;
}

namespace aether::editor
{
	// "Foo.cs(10,17)" or the older "Foo.cs:10" out of a compiler message. Shared so that
	// anything offering to open the offending file agrees on where it is.
	void ParseScriptErrorLocation(const std::string& error, std::string& outPath, int& outLine);

	class ScriptErrorOverlay final
	{
	public:
		void Poll(app::LayerContext& context);
		// Returns true when the user asked to see the full list. The overlay does not know
		// what the Console is, so the caller - which does - opens it.
		[[nodiscard]] bool Draw();
		void Clear();

	private:
		struct Toast
		{
			std::string message;
			std::string summary;
			std::string filePath;
			int line = 0;
		};


		std::deque<Toast> m_toasts;
	};
} // namespace aether::editor
