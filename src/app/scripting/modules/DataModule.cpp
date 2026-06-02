#include "scripting/DasModuleBase.hpp"
#include "daScript/daScript.h"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"
#include <string>

namespace
{
	using namespace aether::app::scripting;

	// 1. Change return type to char*
	// 2. Add 'das::Context* context' as the last parameter.
	//    daScript's binder will automatically fill this in; the script won't see it.
	char* das_read_text_file(const char* virtualPath, das::Context* context)
	{
		auto result = aether::io::FileSystem::ReadFile(virtualPath);
		if (!result)
		{
			AE_WARN(aether::LogCategory::App, "read_text_file: failed to read '{}' - {}", virtualPath, result.error().ToString());
			return nullptr; // In daScript, returning nullptr is safely treated as an empty string.
		}
		auto& bytes = result.value();

		// 3. Allocate the string directly on the daScript context's string heap.
		//    Now the script VM owns the memory and won't leak or crash.
		std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return context->stringHeap->allocateName(s);
	}
} // namespace

namespace aether::app::scripting
{
	struct DataModule : DasModuleBase
	{
		DataModule()
		      : DasModuleBase("data_io")
		{
			das::ModuleLibrary lib(this);

			// The binding remains exactly the same.
			// In the script, this will still be called as `read_text_file("path")`
			Bind<das_read_text_file>(lib, "read_text_file", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(DataModule, aether::app::scripting)
