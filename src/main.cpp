#include <exception>
#include <iostream>
#include <memory>

#include "Application.hpp"
#include "layers/SandboxLayer.hpp"

int main()
{
	try
	{
		meow::app::Application application;
		application.PushLayer(std::make_unique<meow::app::SandboxLayer>());
		return application.Run();
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << "\n";
		return -1;
	}
}