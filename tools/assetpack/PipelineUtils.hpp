#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	using ByteBuffer = std::vector<std::byte>;

	// Reads an entire file into a fresh ByteBuffer. Returns false (leaving `out` in an
	// unspecified but valid state) on any stat/read failure - shared by every caller that
	// needs a whole model file in memory (AssetPacker's `bake`/`bake-all` CLI subcommands,
	// BakeAll.cpp's project-wide walk) so the read path has one definition instead of a
	// copy per caller.
	inline bool ReadWholeFile(const fs::path& path, ByteBuffer& out)
	{
		std::error_code ec;
		const auto size = fs::file_size(path, ec);
		if (ec)
		{
			return false;
		}
		out.resize(size);
		std::ifstream in(path, std::ios::binary);
		return static_cast<bool>(in && in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
	}

	inline std::string Stem(const fs::path& p)
	{
		return p.stem().string();
	}

	inline std::string FormatSize(uint64_t bytes)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(1);
		if (bytes >= 1024ull * 1024 * 1024)
		{
			ss << static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) << " GB";
		}
		else if (bytes >= 1024ull * 1024)
		{
			ss << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
		}
		else if (bytes >= 1024ull)
		{
			ss << static_cast<double>(bytes) / 1024.0 << " KB";
		}
		else
		{
			ss << bytes << " B";
		}
		return ss.str();
	}

	inline std::string FormatDuration(double seconds)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(2) << seconds << " s";
		return ss.str();
	}

	inline std::string FormatTimestamp()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
#if defined(_WIN32)
		localtime_s(&tm, &tt);
#else
		localtime_r(&tt, &tm);
#endif
		std::ostringstream ss;
		ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
		return ss.str();
	}

	template<typename T>
	inline void Append(std::vector<std::byte>& buf, const T& value)
	{
		const auto* const p = reinterpret_cast<const std::byte*>(&value);
		buf.insert(buf.end(), p, p + sizeof(T));
	}

	inline void AppendBytes(std::vector<std::byte>& buf, const void* src, std::size_t n)
	{
		const auto* const p = reinterpret_cast<const std::byte*>(src);
		buf.insert(buf.end(), p, p + n);
	}

	inline void AppendStr(std::vector<std::byte>& buf, const std::string& s)
	{
		assert(s.size() <= std::numeric_limits<uint16_t>::max() && "string too long for uint16_t length prefix");
		const uint16_t len = static_cast<uint16_t>(s.size());
		AppendBytes(buf, &len, sizeof(len));
		AppendBytes(buf, s.data(), s.size());
	}

	inline void AppendStringData(std::vector<std::byte>& buf, const std::string& s)
	{
		AppendBytes(buf, s.data(), s.size());
	}

	// Minimal math types for in-memory processing (not layout-controlled).
	using Vec3 = glm::vec3;
	using Vec4 = glm::vec4;

	using LogFn = void (*)(std::string_view);

	inline void DefaultLog(std::string_view msg)
	{
		std::cerr.write(msg.data(), static_cast<std::streamsize>(msg.size()));
		std::cerr << '\n';
	}
} // namespace aether::assetpipeline
