#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "debug/PixelArtDocument.hpp"
#include "editor/EditorProjectContext.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		std::uint32_t ParseColor(const json& params, const char* key, std::uint32_t fallback)
		{
			if (!params.contains(key) || !params[key].is_array() || params[key].size() < 3)
			{
				return fallback;
			}
			const json& c = params[key];
			const auto ch = [](const json& v) { return static_cast<std::uint32_t>(std::clamp(v.is_number_integer() ? v.get<int>() : 0, 0, 255)); };
			const std::uint32_t r = ch(c[0]);
			const std::uint32_t g = ch(c[1]);
			const std::uint32_t b = ch(c[2]);
			const std::uint32_t a = c.size() >= 4 ? ch(c[3]) : 255u;
			return a << 24 | b << 16 | g << 8 | r;
		}

		int Int(const json& params, const char* key, int fallback = 0)
		{
			return params.contains(key) && params[key].is_number_integer() ? params[key].get<int>() : fallback;
		}

		// Wire-supplied paths must stay inside the project root. ResolveProjectPath joins
		// with operator/, which REPLACES the root for an absolute right-hand side
		// ("C:/anywhere.png"), and ".." segments are never folded away, so without this
		// check pixel.save/pixel.open could read and write anywhere the process can.
		// Returns nullopt for anything that is not a relative path resolving back under
		// the root (including when no project is open).
		[[nodiscard]] std::optional<std::filesystem::path> ResolveProjectLocalPath(const app::EditorProjectContext* project, const std::string& vpath)
		{
			if (project == nullptr || project->root.empty())
			{
				return std::nullopt;
			}
			std::string_view rel = vpath;
			constexpr std::string_view kPrefix = "project://";
			if (rel.starts_with(kPrefix))
			{
				rel.remove_prefix(kPrefix.size());
			}
			const std::filesystem::path candidate(rel);
			if (rel.empty() || candidate.is_absolute())
			{
				return std::nullopt;
			}
			std::string root = project->root.lexically_normal().generic_string();
			while (root.size() > 1 && root.back() == '/')
			{
				root.pop_back();
			}
			// Lexical containment: normalization folds "a/.." away, and a leading climb
			// ("../elsewhere") leaves the root and fails the prefix test.
			const std::filesystem::path disk = (project->root / candidate).lexically_normal();
			if (disk.generic_string().rfind(root + "/", 0) != 0)
			{
				return std::nullopt;
			}
			return disk;
		}


		json DocInfo(const PixelArtDocument& doc)
		{
			return json{{"width", doc.Width()}, {"height", doc.Height()}, {"dirty", doc.Dirty()}, {"path", doc.Path().generic_string()}};
		}
	} // namespace

	void AppendPixelArtMethods(std::vector<ControlMethod>& methods)
	{
		const auto withDoc = [](auto fn)
		{
			return [fn](const json& params, MethodContext& ctx) -> json
			{
				auto* doc = ctx.services.TryGet<PixelArtDocument>();
				if (doc == nullptr)
				{
					return json{{"ok", false}, {"error", "pixel-art document unavailable (editor build only)"}};
				}
				return fn(params, ctx, *doc);
			};
		};

		methods.push_back({"pixel.new",
		        "pixel_new",
		        "Create a fresh transparent pixel-art canvas of {width, height} (1-512). Clears undo history. Mirrors the Pixel Art panel's New.",
		        true,
		        Obj({{"width", IntProp()}, {"height", IntProp()}}, {"width", "height"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.New(Int(p, "width", 32), Int(p, "height", 32));
			                return json{{"ok", true}, {"info", DocInfo(doc)}};
		                })});

		methods.push_back({"pixel.info", "pixel_info", "Current pixel-art canvas dimensions, dirty flag, and file path.", false, Obj(),
		        withDoc([](const json&, MethodContext&, PixelArtDocument& doc) -> json { return json{{"ok", true}, {"info", DocInfo(doc)}}; })});

		methods.push_back({"pixel.set_color", "pixel_set_color", "Set the active drawing colour {color:[r,g,b,a]}.", true, Obj({{"color", ColorProp()}}, {"color"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.SetColor(ParseColor(p, "color", doc.Color()));
			                return json{{"ok", true}};
		                })});

		methods.push_back({"pixel.set",
		        "pixel_set",
		        "Set individual texels: {pixels:[{x,y,color?}]}. 'color' omitted uses the active colour; use [0,0,0,0] to erase. One undo step for the batch.",
		        true,
		        Obj({{"pixels", json{{"type", "array"}, {"items", Obj({{"x", IntProp()}, {"y", IntProp()}, {"color", ColorProp()}}, {"x", "y"})}}}}, {"pixels"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                if (!p.contains("pixels") || !p["pixels"].is_array())
			                {
				                return json{{"ok", false}, {"error", "missing pixels array"}};
			                }
			                doc.Snapshot();
			                int count = 0;
			                for (const auto& px: p["pixels"])
			                {
				                doc.SetPixel(Int(px, "x"), Int(px, "y"), ParseColor(px, "color", doc.Color()));
				                ++count;
			                }
			                return json{{"ok", true}, {"set", count}};
		                })});

		methods.push_back({"pixel.fill_rect", "pixel_fill_rect", "Fill an inclusive cell rectangle {x0,y0,x1,y1, color?}. One undo step.", true,
		        Obj({{"x0", IntProp()}, {"y0", IntProp()}, {"x1", IntProp()}, {"y1", IntProp()}, {"color", ColorProp()}}, {"x0", "y0", "x1", "y1"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.Snapshot();
			                doc.DrawRect(Int(p, "x0"), Int(p, "y0"), Int(p, "x1"), Int(p, "y1"), ParseColor(p, "color", doc.Color()), true);
			                return json{{"ok", true}};
		                })});

		methods.push_back({"pixel.line", "pixel_line", "Draw a line between two texels {x0,y0,x1,y1, color?}. One undo step.", true,
		        Obj({{"x0", IntProp()}, {"y0", IntProp()}, {"x1", IntProp()}, {"y1", IntProp()}, {"color", ColorProp()}}, {"x0", "y0", "x1", "y1"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.Snapshot();
			                doc.DrawLine(Int(p, "x0"), Int(p, "y0"), Int(p, "x1"), Int(p, "y1"), ParseColor(p, "color", doc.Color()));
			                return json{{"ok", true}};
		                })});

		methods.push_back({"pixel.bucket", "pixel_bucket", "Flood-fill the connected region at {x, y} with 'color' (or the active colour). One undo step.", true,
		        Obj({{"x", IntProp()}, {"y", IntProp()}, {"color", ColorProp()}}, {"x", "y"}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.Snapshot();
			                doc.FloodFill(Int(p, "x"), Int(p, "y"), ParseColor(p, "color", doc.Color()));
			                return json{{"ok", true}};
		                })});

		methods.push_back({"pixel.clear", "pixel_clear", "Clear the whole canvas to 'color' (default transparent). One undo step.", true, Obj({{"color", ColorProp()}}),
		        withDoc([](const json& p, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                doc.Snapshot();
			                doc.Clear(ParseColor(p, "color", 0u));
			                return json{{"ok", true}};
		                })});

		methods.push_back({"pixel.get",
		        "pixel_get",
		        "Read the canvas back: dimensions plus a row-major 'pixels' array of 0xAABBGGRR values (only when <= 4096 texels; otherwise omitted).",
		        false,
		        Obj(),
		        withDoc([](const json&, MethodContext&, PixelArtDocument& doc) -> json
		                {
			                json out{{"ok", true}, {"info", DocInfo(doc)}};
			                if (static_cast<std::size_t>(doc.Width()) * doc.Height() <= 4096)
			                {
				                out["pixels"] = doc.Pixels();
			                }
			                else
			                {
				                out["pixelsOmitted"] = true;
			                }
			                return out;
		                })});

		methods.push_back({"pixel.save", "pixel_save", "Save the canvas to a PNG at 'path' (project:// virtual path; default: the current file). Creates folders.", true,
		        Obj({{"path", json{{"type", "string"}}}}),
		        withDoc([](const json& p, MethodContext& ctx, PixelArtDocument& doc) -> json
		                {
			                // The default is the canvas' current file, a path this process put
			                // there; an explicit wire path is resolved against the project root
			                // and must stay inside it.
			                std::filesystem::path disk;
			                std::string echo;
			                if (p.contains("path") && p["path"].is_string())
			                {
				                const std::string vpath = p["path"].get<std::string>();
				                const auto local = ResolveProjectLocalPath(ctx.services.TryGet<app::EditorProjectContext>(), vpath);
				                if (!local.has_value())
				                {
					                AE_WARN(LogCategory::App, "pixel.save: refusing path '{}': it is not a project-relative path staying inside the project root.", vpath);
					                return json{{"ok", false}, {"error", "path must be a project-relative path staying inside the project root"}, {"path", vpath}};
				                }
				                disk = *local;
				                echo = vpath;
			                }
			                else
			                {
				                disk = doc.Path();
				                if (disk.empty())
				                {
					                return json{{"ok", false}, {"error", "no path given and no current file"}};
				                }
				                echo = disk.generic_string();
			                }
			                const bool ok = doc.Save(disk);
			                return json{{"ok", ok}, {"path", echo}};
		                })});

		methods.push_back({"pixel.open", "pixel_open", "Load a PNG at 'path' (project:// virtual path) into the canvas for editing.", true, Obj({{"path", json{{"type", "string"}}}}, {"path"}),
		        withDoc([](const json& p, MethodContext& ctx, PixelArtDocument& doc) -> json
		                {
			                const std::string vpath = p.contains("path") && p["path"].is_string() ? p["path"].get<std::string>() : std::string{};
			                const auto local = ResolveProjectLocalPath(ctx.services.TryGet<app::EditorProjectContext>(), vpath);
			                if (!local.has_value())
			                {
				                AE_WARN(LogCategory::App, "pixel.open: refusing path '{}': it is not a project-relative path staying inside the project root.", vpath);
				                return json{{"ok", false}, {"error", "path must be a project-relative path staying inside the project root"}};
			                }
			                const bool ok = doc.Load(*local);
			                return json{{"ok", ok}, {"info", DocInfo(doc)}};
		                })});
	}
} // namespace aether::editor
