#include <iostream>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

static const int WIDTH = 1280;
static const int HEIGHT = 720;

static const char* APP_NAME = "MeowCore";

int main() {
	// 1. Initialize GLFW
	if (!glfwInit())
	{
		return -1;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, APP_NAME, nullptr, nullptr);

	// 2. Initialize Vulkan Instance
	vkb::InstanceBuilder instBuilder;
	auto instRet = instBuilder.set_app_name(APP_NAME)
		.require_api_version(1, 4, 0)
		.request_validation_layers()
		.use_default_debug_messenger()
		.build();

	if (!instRet) {
		std::cerr << "Failed to create instance: " << instRet.error().message() << "\n";
		return -1;
	}
	vkb::Instance vkbInst = instRet.value();

	// 3. Create Surface
	VkSurfaceKHR surface;
	glfwCreateWindowSurface(vkbInst.instance, window, nullptr, &surface);

	// 4. Select Physical Device
	vkb::PhysicalDeviceSelector selector{ vkbInst };
	auto physRet = selector.set_surface(surface)
		.set_minimum_version(1, 4)
		.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) // Vulkan 1.4 Core
		.select();

	if (!physRet)
	{
		return -1;
	}

	vkb::PhysicalDevice vkbPhys = physRet.value();

	// 5. Create Logical Device
	vkb::DeviceBuilder deviceBuilder{ vkbPhys };
	auto devRet = deviceBuilder.build();
	vkb::Device vkbDevice = devRet.value();

	// Main Loop
	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();
		// Glorified idle loop, do nothing
	}

	// Cleanup
	vkb::destroy_device(vkbDevice);
	vkb::destroy_surface(vkbInst, surface);
	vkb::destroy_instance(vkbInst);

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}