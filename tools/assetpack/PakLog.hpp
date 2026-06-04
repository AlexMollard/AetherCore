#pragma once

#include "PipelineUtils.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <PakFormat.hpp>

struct LogEntry
{
    std::string virtualPath;
    uint64_t    rawSize    = 0;
    uint64_t    onDiskSize = 0;
    uint64_t    hash       = 0;
    uint32_t    flags      = 0;
};

inline void SaveLog(
    const std::filesystem::path& logPath,
    const std::filesystem::path& sourceDir,
    const std::filesystem::path& pakPath,
    int                          compressionLevel,
    const std::vector<LogEntry>& entries,
    uint64_t                     totalRawBytes,
    uint64_t                     pakBytes,
    double                       elapsedSecs)
{
    std::ofstream out(logPath);
    if (!out) return;

    out << "AetherPak Build Log\n";
    out << "Generated   : " << FormatTimestamp() << "\n";
    out << "Source      : " << sourceDir.generic_string() << "\n";
    out << "Output      : " << pakPath.generic_string() << "\n";
    out << "Compression : " << (compressionLevel > 0 ? "zstd level " + std::to_string(compressionLevel) : "disabled") << "\n";
    out << "\n";

    constexpr int kNameW    = 56;
    constexpr int kSizeW    = 10;
    constexpr int kSavingsW = 9;
    const int     kLineW    = kNameW + kSizeW + kSizeW + kSavingsW + 18 + 4;

    const std::string rule(kLineW, '-');

    out << std::left  << std::setw(kNameW)    << "  File"
        << std::right << std::setw(kSizeW)     << "Raw"
        << std::setw(kSizeW)                   << "Pak"
        << std::setw(kSavingsW)                << "Savings"
        << "  " << "Hash"
        << "\n" << rule << "\n";

    for (const auto& e : entries)
    {
        const bool   compressed = (e.flags & PAK_FLAG_ZSTD) != 0;
        const double ratio      = (compressed && e.rawSize > 0)
            ? 100.0 * (1.0 - static_cast<double>(e.onDiskSize) / static_cast<double>(e.rawSize))
            : 0.0;

        std::ostringstream hashStr;
        hashStr << std::hex << std::setfill('0') << std::setw(16) << e.hash;

        std::ostringstream savingsStr;
        if (compressed)
            savingsStr << std::fixed << std::setprecision(1) << ratio << "%";
        else
            savingsStr << "stored";

        out << std::left  << std::setw(kNameW)    << ("  " + e.virtualPath)
            << std::right << std::setw(kSizeW)     << FormatSize(e.rawSize)
            << std::setw(kSizeW)                   << FormatSize(e.onDiskSize)
            << std::setw(kSavingsW)                << savingsStr.str()
            << "  " << hashStr.str()
            << "\n";
    }

    const double savings = totalRawBytes > 0
        ? 100.0 * (1.0 - static_cast<double>(pakBytes) / static_cast<double>(totalRawBytes))
        : 0.0;

    out << rule << "\n";
    std::ostringstream totalSavings;
    totalSavings << std::fixed << std::setprecision(1) << savings << "%";
    out << std::left  << std::setw(kNameW)    << "  TOTAL"
        << std::right << std::setw(kSizeW)     << FormatSize(totalRawBytes)
        << std::setw(kSizeW)                   << FormatSize(pakBytes)
        << std::setw(kSavingsW)                << totalSavings.str()
        << "\n";
    out << "\n";
    out << "  " << entries.size() << " entries  |  " << FormatDuration(elapsedSecs) << "\n";
}
