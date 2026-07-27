#include "net/EnetInit.hpp"

#include <mutex>

#include <enet/enet.h>

namespace aether::net
{
	namespace
	{
		// ENet's global init is process-wide and not refcounted by the library.
		// Every caller of AcquireEnet()/ReleaseEnet() in this process shares this
		// single mutex/counter pair - see EnetInit.hpp for why.
		std::mutex g_enetInitMutex;
		int g_enetRefCount = 0;
	} // namespace

	bool AcquireEnet()
	{
		const std::lock_guard<std::mutex> lock(g_enetInitMutex);
		if (g_enetRefCount == 0 && enet_initialize() != 0)
		{
			return false;
		}
		++g_enetRefCount;
		return true;
	}

	void ReleaseEnet()
	{
		const std::lock_guard<std::mutex> lock(g_enetInitMutex);
		if (g_enetRefCount > 0 && --g_enetRefCount == 0)
		{
			enet_deinitialize();
		}
	}
} // namespace aether::net
