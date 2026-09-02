// Python ENet binding); the MCP itself never links ENet.

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <enet/enet.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace
{
	int Fail(const std::string& message)
	{
		std::cerr << message << "\n";
		return 1;
	}

	// The engine has PlatformPaths::ReadEnvironmentVariable for this, but aether-ctl deliberately links
	// nothing from the engine (see CMakeLists), so it carries its own six lines rather than a dependency.
	std::string ReadEnvironmentVariable(const char* name)
	{
#ifdef _MSC_VER
		char* value = nullptr;
		std::size_t size = 0;
		if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
		{
			return {};
		}
		std::string result(value);
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name);
		return value != nullptr ? value : "";
#endif
	}
} // namespace

int main(int argc, char** argv)
{
	int port = 0;
	std::vector<std::string> positional;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--port" && i + 1 < argc)
		{
			port = std::atoi(argv[++i]);
		}
		else if (arg == "-h" || arg == "--help")
		{
			std::cout << "usage: aether-ctl [--port N] <method> [params-json|-]\n"
			             // Six names read as the whole list. There are a hundred-odd, and the
			             // editor will enumerate them itself, so point at that instead.
			             "  methods: run 'aether-ctl describe' for all of them, with parameters and descriptions\n"
			             "           e.g. info | scene.entities | scene.create | scene.transform | scene.delete | rendergraph\n"
			             "  params:  pass '-' to read JSON from stdin (recommended for large batches)\n"
			             "  port:    --port, else $AETHER_CONTROL_PORT, else 8787\n";
			return 0;
		}
		else
		{
			positional.push_back(arg);
		}
	}

	if (positional.empty())
	{
		return Fail("error: missing <method> (try --help)");
	}
	const std::string method = positional[0];
	std::string paramsStr = positional.size() > 1 ? positional[1] : std::string("{}");
	if (paramsStr == "-")
	{
		paramsStr.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
		if (paramsStr.empty())
		{
			paramsStr = "{}";
		}
	}

	if (port == 0)
	{
		if (const std::string env = ReadEnvironmentVariable("AETHER_CONTROL_PORT"); !env.empty())
		{
			port = std::atoi(env.c_str());
		}
	}
	if (port <= 0)
	{
		port = 8787;
	}

	json params = json::parse(paramsStr, nullptr, false);
	if (params.is_discarded())
	{
		return Fail("error: params is not valid JSON: " + paramsStr);
	}

	if (enet_initialize() != 0)
	{
		return Fail("error: enet_initialize failed");
	}

	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if (client == nullptr)
	{
		enet_deinitialize();
		return Fail("error: enet_host_create failed");
	}

	ENetAddress address{};
	enet_address_set_host(&address, "127.0.0.1");
	address.port = static_cast<enet_uint16>(port);
	ENetPeer* peer = enet_host_connect(client, &address, 2, 0);
	if (peer == nullptr)
	{
		enet_host_destroy(client);
		enet_deinitialize();
		return Fail("error: enet_host_connect failed");
	}

	ENetEvent event;
	const bool connected = enet_host_service(client, &event, 3000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT;
	if (!connected)
	{
		enet_peer_reset(peer);
		enet_host_destroy(client);
		enet_deinitialize();
		return Fail("error: could not connect to control endpoint at 127.0.0.1:" + std::to_string(port) + " (is the editor running with AETHER_CONTROL_PORT set?)");
	}

	const json request = {{"id", 1}, {"method", method}, {"params", params}};
	const std::string body = request.dump();
	ENetPacket* packet = enet_packet_create(body.data(), body.size(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(peer, 0, packet);
	enet_host_flush(client);

	std::string reply;
	bool got = false;
	// thread) take many seconds before the editor drains the command and replies.
	int budgetMs = 30000;
	while (budgetMs > 0)
	{
		const int r = enet_host_service(client, &event, 200);
		budgetMs -= 200;
		if (r > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
		{
			reply.assign(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
			enet_packet_destroy(event.packet);
			got = true;
			break;
		}
		if (r > 0 && event.type == ENET_EVENT_TYPE_DISCONNECT)
		{
			break;
		}
	}

	enet_peer_disconnect_now(peer, 0);
	enet_host_destroy(client);
	enet_deinitialize();

	if (!got)
	{
		return Fail("error: no reply from control endpoint within 6s");
	}

	const json envelope = json::parse(reply, nullptr, false);
	if (envelope.is_discarded())
	{
		return Fail("error: malformed reply: " + reply);
	}
	if (envelope.contains("error"))
	{
		return Fail("error: " + envelope["error"].get<std::string>());
	}
	std::cout << (envelope.contains("result") ? envelope["result"].dump() : envelope.dump()) << "\n";
	return 0;
}
