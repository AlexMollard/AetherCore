#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "editor/ControlMethods.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether::editor
{
	// Localhost control endpoint shared by the Editor and the Launcher. The caller
	// supplies the method table, keeping the transport independent from scene,
	// scripting, and project-hub behaviour. It stays dormant unless a port is
	// supplied via AETHER_CONTROL_PORT, so normal sessions never open a socket.
	//
	// Protocol: each request is one reliable ENet packet carrying a JSON object
	//   { "id": <n>, "method": "<name>", "params": { ... } }
	// and the reply is one reliable packet
	//   { "id": <n>, "result": { ... } }   or   { "id": <n>, "error": "..." }
	//
	// Thread model: ENet is serviced on its own thread and is never touched from
	// anywhere else; the ECS / render graph are only touched on the main thread.
	// Inbound requests are parsed on the ENet thread and pushed to a queue that
	// DrainCommands() (called once per frame from ControlServerLayer::OnUpdate)
	// executes on the main thread; each result is pushed to an outbound queue that
	// the ENet thread flushes back to the originating peer.
	class ControlServer
	{
	public:
		using MethodBuilder = std::function<std::vector<ControlMethod>()>;

		ControlServer(ServiceContainer& services, MethodBuilder methodBuilder, std::string endpointName);
		~ControlServer();

		ControlServer(const ControlServer&) = delete;
		ControlServer& operator=(const ControlServer&) = delete;

		// Starts the ENet host on 127.0.0.1:<port> on a background thread.
		void Start(int port);
		// Stops the host and joins its thread.
		void Stop();

		// Executes every queued request on the CALLING thread (the main/game
		// thread) and queues the replies for the ENet thread to send. Must be
		// called once per frame while the server is running.
		void DrainCommands();

		// Snapshot of per-frame stats surfaced by the "info" method.
		void SetFrameInfo(std::uint64_t frameIndex, double fps) noexcept;

		[[nodiscard]] bool IsRunning() const noexcept;

		[[nodiscard]] int Port() const noexcept
		{
			return m_port;
		}

		// ── Live stats for the editor's Control Server panel ──────────────────
		struct RequestLogEntry
		{
			std::string method;
			bool ok = true;
		};

		[[nodiscard]] std::uint64_t RequestCount() const noexcept
		{
			return m_requestCount.load(std::memory_order_relaxed);
		}

		[[nodiscard]] int ConnectedClients() const noexcept
		{
			return m_connectedClients.load(std::memory_order_relaxed);
		}

		// Most-recent requests (newest last), capped. Copied under lock.
		[[nodiscard]] std::vector<RequestLogEntry> RecentRequests() const;

	private:
		struct Impl; // hides ENet + nlohmann/json from the header

		static constexpr std::size_t kMaxLog = 32;

		// Runs on the ENet thread: the receive/flush loop.
		void ServiceLoop();
		// Runs on the main thread (from DrainCommands): executes one request and
		// returns the JSON result object as a string (an object with an "error"
		// key on failure). paramsJson is the request's "params" object as text.
		std::string Dispatch(const std::string& method, const std::string& paramsJson);

		ServiceContainer& m_services;
		MethodBuilder m_methodBuilder;
		std::string m_endpointName;
		std::unique_ptr<Impl> m_impl;
		std::atomic<std::uint64_t> m_frameIndex{0};
		std::atomic<double> m_fps{0.0};
		std::atomic<std::uint64_t> m_requestCount{0};
		std::atomic<int> m_connectedClients{0};
		mutable std::mutex m_logMutex;
		std::deque<RequestLogEntry> m_log;
		int m_port = 0;
	};
} // namespace aether::editor
