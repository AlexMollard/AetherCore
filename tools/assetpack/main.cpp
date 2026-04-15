// AssetPacker — build-time tool that bundles a source-asset directory into a
// single binary (.pak) file consumed by the runtime PakBackend.
//
// Usage:  AssetPacker <source-dir> <output.pak>
//
// File layout (all integers little-endian):
//   PakHeader        (48 bytes)
//   PakEntry[N]      (24 bytes each)
//   path-data        concatenated null-terminated UTF-8 strings
//   asset-data       raw file bytes

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// On-disk structures — must exactly match PakBackend.hpp in the engine.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct PakHeader
{
    char     magic[4]        = {'A', 'E', 'P', 'K'};
    uint32_t version         = 1;
    uint32_t numEntries      = 0;
    uint32_t reserved        = 0;
    uint64_t pathDataOffset  = 0;
    uint64_t pathDataSize    = 0;
    uint64_t assetDataOffset = 0;
    uint64_t assetDataSize   = 0;
};
static_assert(sizeof(PakHeader) == 48);

struct PakEntry
{
    uint32_t pathOffset = 0;   // byte offset within path-data section
    uint32_t pathLen    = 0;   // character count, NOT including null terminator
    uint64_t dataOffset = 0;   // byte offset within asset-data section
    uint64_t dataSize   = 0;   // byte count
};
static_assert(sizeof(PakEntry) == 24);
#pragma pack(pop)

// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: AssetPacker <source-dir> <output.pak>\n";
        return 1;
    }

    const fs::path sourceDir(argv[1]);
    const fs::path outPath(argv[2]);

    if (!fs::is_directory(sourceDir))
    {
        std::cerr << "AssetPacker: source directory not found: " << sourceDir << "\n";
        return 1;
    }

    // ── Collect files ───────────────────────────────────────────────────────
    struct FileRecord
    {
        std::string virtualPath; // relative to sourceDir, forward-slash separator
        fs::path    diskPath;
    };

    std::vector<FileRecord> files;

    for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const auto rel   = entry.path().lexically_relative(sourceDir);
        const auto vpath = rel.generic_string(); // forward slashes on all platforms
        files.push_back({vpath, entry.path()});
    }

    // Sort for deterministic, reproducible output.
    std::sort(files.begin(), files.end(),
        [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });

    std::cout << "AssetPacker: packing " << files.size() << " file(s) from "
              << sourceDir.generic_string() << "\n";

    // ── Build path-data + asset-data sections ───────────────────────────────
    std::vector<char>      pathData;
    std::vector<std::byte> assetData;
    std::vector<PakEntry>  entries;
    entries.reserve(files.size());

    for (const auto& file : files)
    {
        std::ifstream in(file.diskPath, std::ios::binary | std::ios::ate);
        if (!in)
        {
            std::cerr << "  WARNING: cannot open '" << file.diskPath << "' — skipping\n";
            continue;
        }

        const auto fileSize = static_cast<uint64_t>(in.tellg());
        in.seekg(0);

        const auto dataOffset = static_cast<uint64_t>(assetData.size());
        assetData.resize(assetData.size() + static_cast<std::size_t>(fileSize));
        in.read(reinterpret_cast<char*>(assetData.data() + dataOffset),
                static_cast<std::streamsize>(fileSize));

        PakEntry entry;
        entry.pathOffset = static_cast<uint32_t>(pathData.size());
        entry.pathLen    = static_cast<uint32_t>(file.virtualPath.size());
        entry.dataOffset = dataOffset;
        entry.dataSize   = fileSize;
        entries.push_back(entry);

        pathData.insert(pathData.end(), file.virtualPath.begin(), file.virtualPath.end());
        pathData.push_back('\0');

        std::cout << "  + " << file.virtualPath << " (" << fileSize << " B)\n";
    }

    // ── Compute offsets and fill header ─────────────────────────────────────
    const uint64_t entryTableSize  = sizeof(PakEntry) * entries.size();
    const uint64_t pathDataOffset  = sizeof(PakHeader) + entryTableSize;
    const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

    PakHeader header;
    header.numEntries      = static_cast<uint32_t>(entries.size());
    header.pathDataOffset  = pathDataOffset;
    header.pathDataSize    = static_cast<uint64_t>(pathData.size());
    header.assetDataOffset = assetDataOffset;
    header.assetDataSize   = static_cast<uint64_t>(assetData.size());

    // ── Write output ────────────────────────────────────────────────────────
    fs::create_directories(outPath.parent_path());

    std::ofstream out(outPath, std::ios::binary);
    if (!out)
    {
        std::cerr << "AssetPacker: failed to create output file: " << outPath << "\n";
        return 1;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(entries.data()),
              static_cast<std::streamsize>(entryTableSize));
    out.write(pathData.data(), static_cast<std::streamsize>(pathData.size()));
    out.write(reinterpret_cast<const char*>(assetData.data()),
              static_cast<std::streamsize>(assetData.size()));

    if (!out)
    {
        std::cerr << "AssetPacker: write error on " << outPath << "\n";
        return 1;
    }

    const auto totalBytes = sizeof(PakHeader)
                          + static_cast<std::size_t>(entryTableSize)
                          + pathData.size()
                          + assetData.size();

    std::cout << "AssetPacker: wrote " << outPath.generic_string()
              << " (" << totalBytes << " bytes total, "
              << entries.size() << " entries)\n";
    return 0;
}
