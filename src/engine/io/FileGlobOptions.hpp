#pragma once

namespace meow::io
{
	struct FileGlobOptions
	{
		bool recursive = true;
		bool includeDirectories = false;
		bool caseSensitive = false;
	};
}
