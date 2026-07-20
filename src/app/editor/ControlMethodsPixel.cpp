#include "editor/ControlMethods.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "debug/PixelArtDocument.hpp"
#include "editor/EditorProjectContext.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		json Obj(json properties = json::object(), const std::vector<std::string>& required = {})
		{
			json schema{{"type", "object"}, {"properties", std::move(properties)}};
			if (!required.empty())
			{
				schema["required"] = required;
			}
			return schema;
		}

		json IntProp()
		{
			return json{{"type", "integer"}};
		}

		json ColorProp()
		{
			return json{{"type", "array"}, {"items", json{{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}}, {"minItems", 3}, {"maxItems", 4}, {"description", "[r, g, b] or [r, g, b, a], 0-255; omit to use the current colour"}};
		}

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

		std::filesystem::path ResolveProjectPath(MethodContext& ctx, std::string_view vpath)
		{
			const auto* project = ctx.services.TryGet<app::EditorProjectContext>();
			if (project == nullptr || project->root.empty())
			{
				return {};
			}
			constexpr std::string_view kPrefix = "project://";
			if (vpath.starts_with(kPrefix))
			{
				vpath.remove_prefix(kPrefix.size());
			}
			return project->root / std::filesystem::path(vpath);
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
			                std::string vpath = p.contains("path") && p["path"].is_string() ? p["path"].get<std::string>() : doc.Path().generic_string();
			                if (vpath.empty())
			                {
				                return json{{"ok", false}, {"error", "no path given and no current file"}};
			                }
			                const std::filesystem::path disk = ResolveProjectPath(ctx, vpath);
			                const bool ok = !disk.empty() && doc.Save(disk);
			                return json{{"ok", ok}, {"path", vpath}};
		                })});

		methods.push_back({"pixel.open", "pixel_open", "Load a PNG at 'path' (project:// virtual path) into the canvas for editing.", true, Obj({{"path", json{{"type", "string"}}}}, {"path"}),
		        withDoc([](const json& p, MethodContext& ctx, PixelArtDocument& doc) -> json
		                {
			                const std::string vpath = p.contains("path") && p["path"].is_string() ? p["path"].get<std::string>() : std::string{};
			                const std::filesystem::path disk = ResolveProjectPath(ctx, vpath);
			                const bool ok = !disk.empty() && doc.Load(disk);
			                return json{{"ok", ok}, {"info", DocInfo(doc)}};
		                })});
	}
} // namespace aether::editor
