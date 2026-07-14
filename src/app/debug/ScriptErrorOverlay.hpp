#pragma once

#include <deque>
#include <string>

namespace aether::app
{
	struct LayerContext;
}

namespace aether::editor
{
	class ScriptErrorOverlay final
	{
	public:
		void Poll(app::LayerContext& context);
		void Draw();
		void Clear();

	private:
		struct Toast
		{
			std::string message;
			std::string summary;
			std::string filePath;
			int line = 0;
		};

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);

		std::deque<Toast> m_toasts;
	};
} // namespace aether::editor
