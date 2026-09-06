#include "KenneyImport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <span>
#include <utility>
#include <unordered_set>

#include <cgltf.h>
#include <toml++/toml.hpp>

#ifndef _WIN32
#	include <sys/wait.h>
#endif

#include "BakeOutputs.hpp"
#include "FontProcessor.hpp"
#include "MeshProcessor.hpp"

namespace aether::assetpipeline::kenney
{
	namespace fs = std::filesystem;

	// UTF-8 byte sequences for punctuation this file must either MATCH byte-for-byte against
	// already-committed prose (the em dash in CREDITS.md's "## Kenney — <Pack>" headings) or
	// wants to emit consistently with it (the box-drawing dash in PropSpawner.cs's section
	// comments). Written as raw `\x` escapes rather than the literal character: a narrow
	// string literal's encoding otherwise depends on the compiler's source/execution charset
	// (MSVC without `/utf-8` can silently re-encode a literal UTF-8 character through the
	// local ANSI code page), while a `\x` byte escape is charset-independent by definition.
	namespace
	{
		constexpr const char* kEmDash = "\xE2\x80\x94";     // U+2014 EM DASH
		constexpr const char* kBoxDash = "\xE2\x94\x80"; // U+2500 BOX DRAWINGS LIGHT HORIZONTAL

		std::string Repeat(const char* unit, int count)
		{
			std::string out;
			out.reserve(std::strlen(unit) * static_cast<std::size_t>(count));
			for (int i = 0; i < count; ++i)
			{
				out += unit;
			}
			return out;
		}

		std::string QuoteArg(const std::string& s)
		{
			return "\"" + s + "\"";
		}

		int NormalizeExitCode(int status)
		{
#ifdef _WIN32
			return status;
#else
			if (status == -1)
			{
				return -1;
			}
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
		}

		// Runs `command` and captures stdout as raw bytes. Neither `curl` nor `tar` need
		// stdin, and both report failures on their exit code (which is all callers here act
		// on) plus stderr text that lands in the terminal directly - not worth capturing.
		int RunCapture(const std::string& command, std::string& output)
		{
#ifdef _WIN32
			FILE* pipe = _popen(command.c_str(), "rb");
#else
			FILE* pipe = popen(command.c_str(), "r");
#endif
			if (pipe == nullptr)
			{
				return -1;
			}
			std::array<char, 4096> buf{};
			std::size_t n = 0;
			while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0)
			{
				output.append(buf.data(), n);
			}
#ifdef _WIN32
			return NormalizeExitCode(_pclose(pipe));
#else
			return NormalizeExitCode(pclose(pipe));
#endif
		}

		int RunSilent(const std::string& command)
		{
			return NormalizeExitCode(std::system(command.c_str()));
		}

		std::string ReadFileText(const fs::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
			{
				return {};
			}
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}

		bool ReadFileBytes(const fs::path& path, std::vector<std::byte>& out)
		{
			std::ifstream in(path, std::ios::binary | std::ios::ate);
			if (!in)
			{
				return false;
			}
			const auto size = in.tellg();
			if (size < 0)
			{
				return false;
			}
			out.resize(static_cast<std::size_t>(size));
			in.seekg(0);
			in.read(reinterpret_cast<char*>(out.data()), size);
			return static_cast<bool>(in);
		}


		bool FilesIdentical(const fs::path& a, const fs::path& b)
		{
			std::error_code ec;
			const auto sizeA = fs::file_size(a, ec);
			if (ec)
			{
				return false;
			}
			const auto sizeB = fs::file_size(b, ec);
			if (ec || sizeA != sizeB)
			{
				return false;
			}
			std::ifstream fa(a, std::ios::binary);
			std::ifstream fb(b, std::ios::binary);
			return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
		}

		std::string ToLower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}
	} // namespace

	// ── Manifest ────────────────────────────────────────────────────────────────────────

	std::vector<PackInfo> LoadManifest(const fs::path& manifestPath, std::string& error)
	{
		std::vector<PackInfo> packs;
		const std::string text = ReadFileText(manifestPath);
		if (text.empty())
		{
			error = "cannot read manifest '" + manifestPath.generic_string() + "'";
			return packs;
		}

		toml::table tbl;
		try
		{
			tbl = toml::parse(text);
		}
		catch (const toml::parse_error& e)
		{
			error = "TOML parse error in " + manifestPath.generic_string() + ": " + std::string(e.description());
			return packs;
		}

		const auto* const arr = tbl["pack"].as_array();
		if (arr == nullptr)
		{
			return packs; // a manifest with zero packs is valid, not an error
		}

		auto str = [](const toml::table& t, const char* key) -> std::string
		{
			const auto* const s = t[key].as_string();
			return s != nullptr ? s->get() : std::string{};
		};

		for (const auto& node: *arr)
		{
			const auto* const t = node.as_table();
			if (t == nullptr)
			{
				continue;
			}
			PackInfo pack;
			pack.slug = str(*t, "slug");
			pack.name = str(*t, "name");
			pack.version = str(*t, "version");
			pack.pageUrl = str(*t, "pageUrl");
			pack.zipUrl = str(*t, "zipUrl");
			pack.previewImageUrl = str(*t, "previewImageUrl");
			pack.modelDir = str(*t, "modelDir");
			pack.license = str(*t, "license");
			pack.licenseUrl = str(*t, "licenseUrl");
			pack.author = str(*t, "author");
			if (!pack.slug.empty() && !pack.zipUrl.empty())
			{
				packs.push_back(std::move(pack));
			}
		}
		return packs;
	}

	std::optional<PackInfo> FindPack(const std::vector<PackInfo>& packs, const std::string& slug)
	{
		const auto it = std::find_if(packs.begin(), packs.end(), [&](const PackInfo& p) { return p.slug == slug; });
		return it != packs.end() ? std::optional<PackInfo>(*it) : std::nullopt;
	}

	// ── Pack cache ──────────────────────────────────────────────────────────────────────

	CacheResult EnsurePackCached(const PackInfo& pack, const fs::path& cacheDir)
	{
		CacheResult result;
		std::error_code ec;
		fs::create_directories(cacheDir, ec);

		const fs::path zipPath = cacheDir / (pack.slug + "-" + pack.version + ".zip");
		result.zipPath = zipPath;

		if (fs::exists(zipPath, ec) && fs::file_size(zipPath, ec) > 0)
		{
			result.ok = true;
			result.wasAlreadyCached = true;
			return result;
		}

		const fs::path tmpPath(zipPath.generic_string() + ".part");
		std::error_code rmEc;
		fs::remove(tmpPath, rmEc);

		const std::string command = "curl -sL --fail --max-time 180 -o " + QuoteArg(tmpPath.generic_string()) + " " + QuoteArg(pack.zipUrl);
		const int rc = RunSilent(command);
		if (rc != 0)
		{
			fs::remove(tmpPath, rmEc);
			result.error = "curl failed (exit " + std::to_string(rc) + ") downloading " + pack.zipUrl + " - is `curl` on PATH and is there a network connection?";
			return result;
		}

		std::error_code sizeEc;
		const auto size = fs::file_size(tmpPath, sizeEc);
		if (sizeEc || size == 0)
		{
			fs::remove(tmpPath, rmEc);
			result.error = "downloaded file is empty: " + pack.zipUrl;
			return result;
		}

		// curl --fail only rejects non-2xx HTTP statuses; a captive portal or a stale
		// content-hashed URL can still return 200 with an HTML error page. A local zip
		// signature check catches that before it gets cached and silently corrupts every
		// later import from this pack.
		{
			std::ifstream check(tmpPath, std::ios::binary);
			char magic[2] = {};
			check.read(magic, 2);
			if (!(magic[0] == 'P' && magic[1] == 'K'))
			{
				fs::remove(tmpPath, rmEc);
				result.error = "downloaded file is not a zip archive (unexpected content) for " + pack.zipUrl + " - the manifest entry for '" + pack.slug + "' likely needs refreshing (see kenney_packs.toml's header comment)";
				return result;
			}
		}

		std::error_code renameEc;
		fs::rename(tmpPath, zipPath, renameEc);
		if (renameEc)
		{
			result.error = "downloaded pack but could not place it at " + zipPath.generic_string() + ": " + renameEc.message();
			return result;
		}

		result.ok = true;
		result.wasAlreadyCached = false;
		return result;
	}

	// ── Pack listing ────────────────────────────────────────────────────────────────────

	std::vector<PackEntry> ListPackModels(const fs::path& zipPath, const std::string& subDir, std::string& error, const std::vector<std::string>& extensions)
	{
		std::vector<PackEntry> entries;
		std::string output;
		const int rc = RunCapture("tar -tf " + QuoteArg(zipPath.generic_string()), output);
		if (rc != 0)
		{
			error = "tar -tf failed (exit " + std::to_string(rc) + ") listing " + zipPath.generic_string() + " - is `tar` on PATH?";
			return entries;
		}

		const std::string prefix = subDir + "/";
		std::istringstream lines(output);
		std::string line;
		while (std::getline(lines, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			{
				line.pop_back();
			}
			if (line.empty() || line.back() == '/' || line.rfind(prefix, 0) != 0)
			{
				continue;
			}
			const std::string rest = line.substr(prefix.size());
			if (rest.find('/') != std::string::npos)
			{
				continue; // a subfolder (e.g. this pack's shared Textures/) - not a model itself
			}
			const std::string ext = ToLower(fs::path(rest).extension().generic_string());
			if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
			{
				continue;
			}
			entries.push_back(PackEntry{.zipMemberPath = line, .fileName = rest});
		}
		std::sort(entries.begin(), entries.end(), [](const PackEntry& a, const PackEntry& b) { return a.fileName < b.fileName; });
		return entries;
	}

	// ── Extraction ──────────────────────────────────────────────────────────────────────

	namespace
	{
		// Extracts one member into a scratch directory (tar preserves the member's own
		// internal path under it, e.g. extracting "Models/GLB format/x.glb" into `scratch`
		// produces `scratch/Models/GLB format/x.glb`) and returns that path, or empty on
		// failure. Scratch is fully owned by this call - cleared first, so a previous call's
		// leftovers can never be mistaken for this one's output.
		fs::path ExtractMember(const fs::path& zipPath, const std::string& memberPath, const fs::path& scratch, std::string& error)
		{
			std::error_code ec;
			fs::remove_all(scratch, ec);
			fs::create_directories(scratch, ec);

			const std::string command = "tar -xf " + QuoteArg(zipPath.generic_string()) + " -C " + QuoteArg(scratch.generic_string()) + " " + QuoteArg(memberPath);
			if (RunSilent(command) != 0)
			{
				error = "tar failed extracting '" + memberPath + "' from " + zipPath.generic_string();
				return {};
			}
			fs::path extracted = scratch / fs::path(memberPath);
			if (!fs::exists(extracted, ec))
			{
				error = "tar reported success but '" + memberPath + "' is not present in the extracted output";
				return {};
			}
			return extracted;
		}

		struct ParsedGlb
		{
			cgltf_data* data = nullptr;
			~ParsedGlb()
			{
				if (data != nullptr)
				{
					cgltf_free(data);
				}
			}
			ParsedGlb() = default;
			ParsedGlb(const ParsedGlb&) = delete;
			ParsedGlb& operator=(const ParsedGlb&) = delete;
		};

		bool ParseGlbFile(const fs::path& path, ParsedGlb& out, std::string& error)
		{
			const cgltf_options options{};
			const std::string pathStr = path.string();
			if (cgltf_parse_file(&options, pathStr.c_str(), &out.data) != cgltf_result_success)
			{
				error = "cgltf_parse_file failed for " + path.generic_string();
				return false;
			}
			if (cgltf_load_buffers(&options, out.data, pathStr.c_str()) != cgltf_result_success)
			{
				error = "cgltf_load_buffers failed for " + path.generic_string();
				return false;
			}
			if (cgltf_validate(out.data) != cgltf_result_success)
			{
				error = "cgltf_validate failed for " + path.generic_string();
				return false;
			}
			return true;
		}

		// Every external (non-data-URI) image this glTF's materials reference, in first-seen
		// order and de-duplicated - Kenney packs typically point every material at the same
		// one shared "colormap.png".
		std::vector<std::string> CollectImageUris(const cgltf_data& data)
		{
			std::vector<std::string> uris;
			std::unordered_set<std::string> seen;
			for (std::size_t i = 0; i < data.images_count; ++i)
			{
				const cgltf_image& img = data.images[i];
				if (img.uri == nullptr)
				{
					continue;
				}
				const std::string uri(img.uri);
				if (uri.rfind("data:", 0) == 0)
				{
					continue; // embedded base64 image - nothing external to copy
				}
				if (seen.insert(uri).second)
				{
					uris.push_back(uri);
				}
			}
			return uris;
		}

		struct AccumTransform
		{
			Vec3 translation{0.0f, 0.0f, 0.0f};
			Vec3 scale{1.0f, 1.0f, 1.0f};
		};

		void WalkNodeBounds(const cgltf_node* node, const AccumTransform& parent, bool& any, Vec3& outMin, Vec3& outMax)
		{
			AccumTransform cur;
			Vec3 localT{0.0f, 0.0f, 0.0f};
			Vec3 localS{1.0f, 1.0f, 1.0f};
			if (node->has_translation != 0)
			{
				localT = Vec3(node->translation[0], node->translation[1], node->translation[2]);
			}
			if (node->has_scale != 0)
			{
				localS = Vec3(node->scale[0], node->scale[1], node->scale[2]);
			}
			// Translation and scale only - rotation is deliberately dropped. This matches the
			// manual derivation this tool automates (see PropCatalogExtension.md's TrashCan
			// note): correct for every axis-aligned Kenney single-object export, which is what
			// this importer targets; a model authored with a rotated mesh node would need a
			// human to re-derive its collider by eye, same as it always has.
			cur.translation = parent.translation + Vec3(parent.scale.x * localT.x, parent.scale.y * localT.y, parent.scale.z * localT.z);
			cur.scale = Vec3(parent.scale.x * localS.x, parent.scale.y * localS.y, parent.scale.z * localS.z);

			if (node->mesh != nullptr)
			{
				for (std::size_t p = 0; p < node->mesh->primitives_count; ++p)
				{
					const cgltf_primitive& prim = node->mesh->primitives[p];
					for (std::size_t a = 0; a < prim.attributes_count; ++a)
					{
						if (prim.attributes[a].type != cgltf_attribute_type_position)
						{
							continue;
						}
						const cgltf_accessor* acc = prim.attributes[a].data;
						Vec3 mn(0.0f);
						Vec3 mx(0.0f);
						if (acc->has_min != 0 && acc->has_max != 0)
						{
							mn = Vec3(acc->min[0], acc->min[1], acc->min[2]);
							mx = Vec3(acc->max[0], acc->max[1], acc->max[2]);
						}
						else
						{
							// Spec requires POSITION accessors to carry min/max, but a
							// malformed export might not - fall back to reading every vertex.
							bool first = true;
							for (std::size_t v = 0; v < acc->count; ++v)
							{
								cgltf_float out3[3] = {};
								if (cgltf_accessor_read_float(acc, v, out3, 3) == 0)
								{
									continue;
								}
								const Vec3 p3(out3[0], out3[1], out3[2]);
								if (first)
								{
									mn = mx = p3;
									first = false;
								}
								else
								{
									mn = glm::min(mn, p3);
									mx = glm::max(mx, p3);
								}
							}
							if (first)
							{
								continue; // no readable vertices at all
							}
						}
						Vec3 wmn = cur.translation + Vec3(cur.scale.x * mn.x, cur.scale.y * mn.y, cur.scale.z * mn.z);
						Vec3 wmx = cur.translation + Vec3(cur.scale.x * mx.x, cur.scale.y * mx.y, cur.scale.z * mx.z);
						if (wmn.x > wmx.x)
						{
							std::swap(wmn.x, wmx.x);
						}
						if (wmn.y > wmx.y)
						{
							std::swap(wmn.y, wmx.y);
						}
						if (wmn.z > wmx.z)
						{
							std::swap(wmn.z, wmx.z);
						}
						if (!any)
						{
							outMin = wmn;
							outMax = wmx;
							any = true;
						}
						else
						{
							outMin = glm::min(outMin, wmn);
							outMax = glm::max(outMax, wmx);
						}
					}
				}
			}

			for (std::size_t c = 0; c < node->children_count; ++c)
			{
				WalkNodeBounds(node->children[c], cur, any, outMin, outMax);
			}
		}

		ColliderFit FitFromParsed(const cgltf_data& data, const std::string& modelNameForHints, ColliderShape requestedShape)
		{
			Vec3 mn(0.0f);
			Vec3 mx(0.0f);
			bool any = false;
			const AccumTransform identity{};

			if (data.scene != nullptr)
			{
				for (std::size_t i = 0; i < data.scene->nodes_count; ++i)
				{
					WalkNodeBounds(data.scene->nodes[i], identity, any, mn, mx);
				}
			}
			else
			{
				for (std::size_t i = 0; i < data.nodes_count; ++i)
				{
					if (data.nodes[i].parent == nullptr)
					{
						WalkNodeBounds(&data.nodes[i], identity, any, mn, mx);
					}
				}
			}

			ColliderFit fit;
			if (!any)
			{
				fit.shape = ColliderShape::Box;
				fit.notes.push_back("no POSITION geometry found at all - collider is a zero-size placeholder, verify manually");
				return fit;
			}

			const Vec3 dims = mx - mn;
			const Vec3 center = (mn + mx) * 0.5f;
			fit.nativeSize = dims;
			fit.center = center;

			const ColliderShape shape = requestedShape == ColliderShape::Auto ? ColliderShape::Box : requestedShape;
			fit.shape = shape;

			switch (shape)
			{
				case ColliderShape::Sphere:
					fit.radius = std::max({dims.x, dims.y, dims.z}) * 0.5f;
					break;
				case ColliderShape::Cylinder:
					fit.radius = (dims.x + dims.z) * 0.25f;
					fit.halfHeight = dims.y * 0.5f;
					break;
				case ColliderShape::Capsule:
					fit.radius = (dims.x + dims.z) * 0.25f;
					fit.halfHeight = std::max(0.0f, dims.y * 0.5f - fit.radius);
					if (dims.y * 0.5f < fit.radius)
					{
						fit.notes.push_back("model is wider than it is tall - a capsule degenerates toward a sphere here, verify visually");
					}
					break;
				case ColliderShape::None:
					break;
				case ColliderShape::Box:
				case ColliderShape::Auto:
				default:
					fit.halfExtents = dims * 0.5f;
					break;
			}

			if (shape == ColliderShape::Box)
			{
				const float maxXz = std::max(dims.x, dims.z);
				if (maxXz > 0.0f && std::abs(dims.x - dims.z) / maxXz < 0.15f)
				{
					fit.notes.push_back("footprint is roughly square (" + std::to_string(dims.x) + " x " + std::to_string(dims.z) + ") - if this model is actually round, re-import with an explicit colliderShape of cylinder or capsule instead of trusting this box");
				}
			}

			static const std::array<const char*, 6> kTaperHints = {"cone", "wedge", "taper", "pyramid", "spike", "funnel"};
			const std::string lowerName = ToLower(modelNameForHints);
			for (const char* hint: kTaperHints)
			{
				if (lowerName.find(hint) != std::string::npos)
				{
					fit.notes.push_back(std::string("filename suggests tapered geometry ('") + hint + "') - no box/sphere/capsule/cylinder primitive fits a taper well; see PropCatalogExtension.md's convex-hull worklist before trusting this collider");
					break;
				}
			}

			return fit;
		}
	} // namespace

	std::string ToString(ColliderShape shape)
	{
		switch (shape)
		{
			case ColliderShape::Box:
				return "box";
			case ColliderShape::Sphere:
				return "sphere";
			case ColliderShape::Capsule:
				return "capsule";
			case ColliderShape::Cylinder:
				return "cylinder";
			case ColliderShape::None:
				return "none";
			case ColliderShape::Auto:
			default:
				return "auto";
		}
	}

	std::optional<ColliderShape> ParseColliderShape(const std::string& text)
	{
		const std::string lower = ToLower(text);
		if (lower == "auto" || lower.empty())
		{
			return ColliderShape::Auto;
		}
		if (lower == "box")
		{
			return ColliderShape::Box;
		}
		if (lower == "sphere")
		{
			return ColliderShape::Sphere;
		}
		if (lower == "capsule")
		{
			return ColliderShape::Capsule;
		}
		if (lower == "cylinder")
		{
			return ColliderShape::Cylinder;
		}
		if (lower == "none")
		{
			return ColliderShape::None;
		}
		return std::nullopt;
	}

	std::optional<ColliderFit> FitCollider(const fs::path& glbPath, const std::string& modelNameForHints, ColliderShape requestedShape, std::string& error)
	{
		ParsedGlb glb;
		if (!ParseGlbFile(glbPath, glb, error))
		{
			return std::nullopt;
		}
		return FitFromParsed(*glb.data, modelNameForHints, requestedShape);
	}

	// ── CREDITS.md ──────────────────────────────────────────────────────────────────────

	namespace
	{
		// Appends `bulletLine` under the pack's existing "## Kenney <em-dash> <name>" section
		// in CREDITS.md if one exists (inserted after that section's last `  - ` bullet), or
		// creates a new section at the end of the file otherwise. Returns {appended,
		// alreadyPresent}; never duplicates a bullet that is already present verbatim.
		std::pair<bool, bool> AppendCreditsLine(const fs::path& creditsPath, const PackInfo& pack, const std::string& bulletLine)
		{
			std::string text = ReadFileText(creditsPath);
			if (text.find(bulletLine) != std::string::npos)
			{
				return {false, true};
			}

			// Search on name only (no version): robust to a pack bumping its version later -
			// re-finds the SAME section instead of creating a duplicate one for the new
			// version. The version is still recorded, just in the heading text created
			// below, not in what this call searches for.
			const std::string heading = "## Kenney " + std::string(kEmDash) + " " + pack.name;
			const std::size_t headingPos = text.find(heading);

			if (headingPos == std::string::npos)
			{
				if (!text.empty() && text.back() != '\n')
				{
					text += "\n";
				}
				text += "\n" + heading + (pack.version.empty() ? "" : " (v" + pack.version + ")") + "\n";
				text += "- Source: " + pack.pageUrl + "\n";
				text += "- Author: " + pack.author + "\n";
				text += "- Licence: " + pack.license + " (" + pack.licenseUrl + ")\n";
				// modelDir names one shared folder every model in the pack was copied from -
				// meaningless for a non-model pack (e.g. Input Prompts' fonts each come from
				// their own per-family folder), so it is only mentioned when set.
				text += (pack.modelDir.empty() ? std::string("- Files used:") : "- Files used (copied from `" + pack.modelDir + "/`):") + "\n";
				text += "  " + bulletLine + "\n";
			}
			else
			{
				// Find this section's end: the next "## " heading at column 0, or EOF.
				std::size_t sectionEnd = text.find("\n## ", headingPos);
				if (sectionEnd == std::string::npos)
				{
					sectionEnd = text.size();
				}
				// Within the section, insert after the LAST "  - `" bullet line so the new
				// entry lands with its siblings instead of before them.
				std::size_t insertAt = std::string::npos;
				std::size_t searchFrom = headingPos;
				while (true)
				{
					const std::size_t found = text.find("\n  - ", searchFrom);
					if (found == std::string::npos || found >= sectionEnd)
					{
						break;
					}
					insertAt = found;
					searchFrom = found + 1;
				}
				if (insertAt == std::string::npos)
				{
					insertAt = sectionEnd; // no existing bullet found; append right at section end
				}
				else
				{
					const std::size_t lineEnd = text.find('\n', insertAt + 1);
					insertAt = lineEnd == std::string::npos ? text.size() : lineEnd;
				}
				text.insert(insertAt, "\n  " + bulletLine);
			}

			std::ofstream out(creditsPath, std::ios::binary | std::ios::trunc);
			out << text;
			return {true, false};
		}
	} // namespace

	// ── PropSpawner.cs catalogue ────────────────────────────────────────────────────────

	namespace
	{
		std::string FormatFloat(float v)
		{
			std::ostringstream ss;
			ss.setf(std::ios::fixed);
			ss.precision(4);
			ss << v << "f";
			return ss.str();
		}

		std::string FormatVec3(const Vec3& v)
		{
			return "new Vector3(" + FormatFloat(v.x) + ", " + FormatFloat(v.y) + ", " + FormatFloat(v.z) + ")";
		}

		std::string BuildCatalogEntry(const ImportRequest& request, const ColliderFit& fit, const std::string& modelPathVfs)
		{
			std::ostringstream ss;
			ss << "        new(\"" << request.displayName << "\", IsSphere: false, Size: 1f, Mass: " << FormatFloat(request.mass) << ", Color: default,\n";
			ss << "            ModelPath: \"" << modelPathVfs << "\",\n";
			ss << "            ColliderShape: PropColliderShape." << (fit.shape == ColliderShape::Box ? "Box" : fit.shape == ColliderShape::Sphere ? "Sphere" : fit.shape == ColliderShape::Capsule ? "Capsule" : "Cylinder");
			switch (fit.shape)
			{
				case ColliderShape::Sphere:
					ss << ", ColliderRadius: " << FormatFloat(fit.radius);
					break;
				case ColliderShape::Capsule:
				case ColliderShape::Cylinder:
					ss << ", ColliderRadius: " << FormatFloat(fit.radius) << ", ColliderHalfHeight: " << FormatFloat(fit.halfHeight);
					break;
				case ColliderShape::Box:
				case ColliderShape::Auto:
				case ColliderShape::None:
				default:
					ss << ", ColliderHalfExtents: " << FormatVec3(fit.halfExtents);
					break;
			}
			ss << ", ColliderCenter: " << FormatVec3(fit.center) << "),\n";
			return ss.str();
		}

		// Inserts `entryText` just before the closing "};" of `Catalog`'s array initializer,
		// under a one-time section comment this tool owns. Returns {appended, alreadyPresent}
		// keyed on the entry's own ModelPath string, so a second import of the same model is
		// a no-op instead of a duplicate catalogue row.
		std::pair<bool, bool> AppendCatalogEntry(const fs::path& catalogPath, const std::string& modelPathVfs, const std::string& entryText)
		{
			std::string text = ReadFileText(catalogPath);
			if (text.empty())
			{
				return {false, false};
			}
			const std::string modelPathNeedle = "\"" + modelPathVfs + "\"";
			if (text.find(modelPathNeedle) != std::string::npos)
			{
				return {false, true};
			}

			const std::string declNeedle = "PropDef[] Catalog =";
			const std::size_t declPos = text.find(declNeedle);
			if (declPos == std::string::npos)
			{
				return {false, false};
			}
			const std::size_t openBrace = text.find('{', declPos);
			if (openBrace == std::string::npos)
			{
				return {false, false};
			}
			int depth = 0;
			std::size_t closeBrace = std::string::npos;
			for (std::size_t i = openBrace; i < text.size(); ++i)
			{
				if (text[i] == '{')
				{
					++depth;
				}
				else if (text[i] == '}')
				{
					--depth;
					if (depth == 0)
					{
						closeBrace = i;
						break;
					}
				}
			}
			if (closeBrace == std::string::npos)
			{
				return {false, false};
			}

			const std::string sectionComment = "// " + Repeat(kBoxDash, 2) + " Kenney import (native, via the AssetPacker `kenney` tool / MCP / Editor Kenney Browser) " + Repeat(kBoxDash, 2);
			std::string insertion;
			if (text.find(sectionComment) == std::string::npos)
			{
				insertion += "        " + sectionComment + "\n";
			}
			insertion += entryText; // entryText already ends in ",\n" - self-terminated, no leading/trailing blank lines

			// Insert at the START of the closing brace's own line (not right before the
			// brace) so that line's original indent stays attached to "};" instead of being
			// orphaned onto a blank line above it.
			const std::size_t closeLineStart = text.rfind('\n', closeBrace);
			text.insert(closeLineStart == std::string::npos ? 0 : closeLineStart + 1, insertion);
			std::ofstream out(catalogPath, std::ios::binary | std::ios::trunc);
			out << text;
			return {true, false};
		}
	} // namespace

	// ── ImportModel ─────────────────────────────────────────────────────────────────────

	ImportResult ImportModel(const ImportRequest& request)
	{
		ImportResult result;

		if (request.category.empty() || request.propName.empty() || request.displayName.empty())
		{
			result.error = "category, propName and displayName are all required";
			return result;
		}

		const CacheResult cache = EnsurePackCached(request.pack, request.cacheDir);
		if (!cache.ok)
		{
			result.error = cache.error;
			return result;
		}

		const std::string modelRel = "assets/models/" + request.category + "/" + request.propName + ".glb";
		const fs::path modelDiskPath = request.projectRoot / fs::path(modelRel);
		result.modelPath = modelDiskPath;

		std::error_code ec;
		if (fs::exists(modelDiskPath, ec))
		{
			result.modelAlreadyPresent = true;
		}
		else
		{
			const fs::path scratch = request.cacheDir / "_extract";
			std::string extractError;
			const fs::path extracted = ExtractMember(cache.zipPath, request.zipMemberPath, scratch, extractError);
			if (extracted.empty())
			{
				result.error = extractError;
				return result;
			}
			fs::create_directories(modelDiskPath.parent_path(), ec);
			fs::copy_file(extracted, modelDiskPath, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.error = "could not place model at " + modelDiskPath.generic_string() + ": " + ec.message();
				return result;
			}
		}

		ParsedGlb glb;
		std::string parseError;
		if (!ParseGlbFile(modelDiskPath, glb, parseError))
		{
			result.error = parseError;
			return result;
		}

		// Shared texture(s), extracted only if referenced and only what is referenced -
		// never the whole pack, and never overwriting a same-named texture that already
		// exists with DIFFERENT content (a silent overwrite would change the look of every
		// other already-imported model sharing that atlas).
		const std::string zipModelParent = fs::path(request.zipMemberPath).parent_path().generic_string();
		for (const std::string& uri: CollectImageUris(*glb.data))
		{
			const std::string textureZipMember = zipModelParent.empty() ? uri : zipModelParent + "/" + uri;
			const fs::path textureDiskPath = request.projectRoot / fs::path("assets/models/" + request.category + "/" + uri).lexically_normal();

			if (fs::exists(textureDiskPath, ec))
			{
				const fs::path scratch = request.cacheDir / "_extract_tex_check";
				std::string extractError;
				const fs::path extracted = ExtractMember(cache.zipPath, textureZipMember, scratch, extractError);
				if (extracted.empty())
				{
					result.error = extractError;
					return result;
				}
				if (!FilesIdentical(extracted, textureDiskPath))
				{
					result.error = "'" + textureDiskPath.generic_string() + "' already exists with DIFFERENT content than this pack's copy of '" + uri + "' - refusing to overwrite a shared texture other imported props may depend on; resolve by hand";
					return result;
				}
				if (result.texturePath.empty())
				{
					result.texturePath = textureDiskPath;
					result.textureAlreadyPresent = true;
				}
				continue;
			}

			const fs::path scratch = request.cacheDir / "_extract_tex";
			std::string extractError;
			const fs::path extracted = ExtractMember(cache.zipPath, textureZipMember, scratch, extractError);
			if (extracted.empty())
			{
				result.error = extractError;
				return result;
			}
			fs::create_directories(textureDiskPath.parent_path(), ec);
			fs::copy_file(extracted, textureDiskPath, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.error = "could not place texture at " + textureDiskPath.generic_string() + ": " + ec.message();
				return result;
			}
			if (result.texturePath.empty())
			{
				result.texturePath = textureDiskPath;
			}
		}

		// Bake: same MeshProcessor the editor's drag-drop path (EnsureModelBaked) and the
		// standalone `AssetPacker bake` CLI subcommand both use, through the one shared
		// output writer (BakeOutputs.hpp) rather than a third copy of the tracked-write
		// discipline.
		const fs::path meshOutPath = modelDiskPath.parent_path() / (request.propName + ".mesh");
		if (!fs::exists(meshOutPath, ec))
		{
			std::vector<std::byte> raw;
			if (!ReadFileBytes(modelDiskPath, raw))
			{
				result.error = "cannot read '" + modelDiskPath.generic_string() + "' for baking";
				return result;
			}
			const auto baked = MeshProcessor::Process(std::span<const std::byte>(raw.data(), raw.size()), modelDiskPath, modelRel, request.projectRoot);
			if (baked.meshData.empty())
			{
				result.error = "no mesh geometry produced from '" + modelDiskPath.generic_string() + "' (animation-only or unsupported glTF)";
				return result;
			}
			const BakeWriteResult write = WriteBakedOutputs(baked, modelDiskPath, request.projectRoot);
			if (!write.ok)
			{
				result.error = write.error;
				return result;
			}
			for (const std::string& w: write.warnings)
			{
				result.warnings.push_back(w);
			}
			result.bakedNow = true;
		}

		const std::string sourceFileName = fs::path(request.zipMemberPath).filename().generic_string();
		result.collider = FitFromParsed(*glb.data, sourceFileName, request.requestedShape);
		for (const std::string& note: result.collider.notes)
		{
			result.warnings.push_back(note);
		}

		const std::string destRel = request.category + "/" + request.propName + ".glb";
		result.creditsLine = "- `" + sourceFileName + "` -> `" + destRel + "`";
		const fs::path creditsPath = request.projectRoot / fs::path(request.creditsRelPath);
		const auto [creditsAppended, creditsAlready] = AppendCreditsLine(creditsPath, request.pack, result.creditsLine);
		result.creditsAppended = creditsAppended;
		result.creditsAlreadyPresent = creditsAlready;

		if (request.requestedShape == ColliderShape::None)
		{
			result.catalogSkippedNoCollider = true;
			result.warnings.push_back("colliderShape=none: not registered in PropSpawner.cs (viewmodel-style import) - wire it in by hand if it should be spawnable");
		}
		else
		{
			const std::string modelPathVfs = "project://" + modelRel;
			result.catalogEntry = BuildCatalogEntry(request, result.collider, modelPathVfs);
			const fs::path catalogPath = request.projectRoot / fs::path(request.catalogRelPath);
			const auto [catalogAppended, catalogAlready] = AppendCatalogEntry(catalogPath, modelPathVfs, result.catalogEntry);
			result.catalogAppended = catalogAppended;
			result.catalogAlreadyPresent = catalogAlready;
			if (!catalogAppended && !catalogAlready)
			{
				result.warnings.push_back("could not locate 'PropDef[] Catalog' in " + catalogPath.generic_string() + " - catalogue entry NOT written, register it by hand:\n" + result.catalogEntry);
			}
		}

		result.ok = true;
		return result;
	}

	FontImportResult ImportFont(const FontImportRequest& request)
	{
		FontImportResult result;

		if (request.fontName.empty())
		{
			result.error = "fontName is required";
			return result;
		}

		const CacheResult cache = EnsurePackCached(request.pack, request.cacheDir);
		if (!cache.ok)
		{
			result.error = cache.error;
			return result;
		}

		const fs::path fontsDir = request.projectRoot / "assets" / "fonts";
		const std::string sourceExt = ToLower(fs::path(request.zipMemberPath).extension().generic_string());
		result.fontPath = fontsDir / (request.fontName + sourceExt);

		std::error_code ec;
		if (fs::exists(result.fontPath, ec))
		{
			result.fontAlreadyPresent = true;
		}
		else
		{
			const fs::path scratch = request.cacheDir / "_extract_font";
			std::string extractError;
			const fs::path extracted = ExtractMember(cache.zipPath, request.zipMemberPath, scratch, extractError);
			if (extracted.empty())
			{
				result.error = extractError;
				return result;
			}
			fs::create_directories(fontsDir, ec);
			fs::copy_file(extracted, result.fontPath, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.error = "could not place font at " + result.fontPath.generic_string() + ": " + ec.message();
				return result;
			}
		}

		// The glyph-name -> codepoint reference text is not consumed by the engine at all -
		// it exists so a script author knows which \uE0xx escape draws which key/button icon.
		// Best-effort: its absence never fails the import.
		if (!request.charMapZipMemberPath.empty())
		{
			result.charMapPath = fontsDir / (request.fontName + ".charmap.txt");
			if (!fs::exists(result.charMapPath, ec))
			{
				const fs::path scratch = request.cacheDir / "_extract_font_map";
				std::string extractError;
				const fs::path extracted = ExtractMember(cache.zipPath, request.charMapZipMemberPath, scratch, extractError);
				if (extracted.empty())
				{
					result.warnings.push_back("could not extract glyph map '" + request.charMapZipMemberPath + "': " + extractError);
					result.charMapPath.clear();
				}
				else
				{
					fs::copy_file(extracted, result.charMapPath, fs::copy_options::overwrite_existing, ec);
					if (ec)
					{
						result.warnings.push_back("could not place glyph map at " + result.charMapPath.generic_string() + ": " + ec.message());
						result.charMapPath.clear();
					}
				}
			}
		}

		// Same baker `AssetPacker bake-font` calls (FontProcessor.hpp) - not a subprocess to
		// that CLI, a direct call to the library function it and this share.
		result.curvesPath = fontsDir / (request.fontName + ".fontcurves");
		if (!fs::exists(result.curvesPath, ec))
		{
			const FontProcessor::BakeResult baked = FontProcessor::BakeFont(result.fontPath, fontsDir);
			if (!baked.success)
			{
				result.error = "font bake failed for '" + result.fontPath.generic_string() + "': " + baked.error;
				return result;
			}
			result.bakedNow = true;
			result.glyphCount = baked.glyphCount;
		}

		const std::string sourceFileName = fs::path(request.zipMemberPath).filename().generic_string();
		result.creditsLine = "- `" + sourceFileName + "` -> `assets/fonts/" + request.fontName + sourceExt + "`" + (result.charMapPath.empty() ? std::string() : " (+ glyph map `" + request.fontName + ".charmap.txt`)");
		const fs::path creditsPath = request.projectRoot / fs::path(request.creditsRelPath);
		const auto [creditsAppended, creditsAlready] = AppendCreditsLine(creditsPath, request.pack, result.creditsLine);
		result.creditsAppended = creditsAppended;
		result.creditsAlreadyPresent = creditsAlready;

		result.ok = true;
		return result;
	}
} // namespace aether::assetpipeline::kenney
