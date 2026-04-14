#pragma once

namespace aether::io
{
	struct FileGlobOptions
	{
		bool recursive = true;
		bool includeDirectories = false;
		bool caseSensitive = false;
	};
}
