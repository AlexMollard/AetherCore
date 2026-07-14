#pragma once

#ifdef _WIN32
#	ifndef VK_USE_PLATFORM_WIN32_KHR
#		define VK_USE_PLATFORM_WIN32_KHR
#	endif
#endif

#ifndef VK_NO_PROTOTYPES
#	define VK_NO_PROTOTYPES
#endif

#include <volk.h>
