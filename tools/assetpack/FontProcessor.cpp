#include "FontProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	namespace FontProcessor
	{
		namespace
		{
			// Glyphs are loaded unscaled and divided by the face's em, so everything on disk
			// is in em units and the asset carries no resolution of any kind.
			constexpr float kBandTargetCurves = 4.0f; // curves per band the splitter aims for
			constexpr std::uint32_t kMaxBands = 16;
			constexpr int kCubicSplits = 4; // sub-curves a cubic is broken into before approximating

			struct Vec2
			{
				float x = 0.0f;
				float y = 0.0f;
			};

			inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
			inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
			inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

			// One quadratic Bezier of a contour, in em units and in the font's own
			// orientation (y up). Direction is preserved exactly as authored, because the
			// shader's coverage sum reads winding from it.
			struct Curve
			{
				Vec2 p0, p1, p2;

				[[nodiscard]] float MinX() const { return std::min({p0.x, p1.x, p2.x}); }
				[[nodiscard]] float MaxX() const { return std::max({p0.x, p1.x, p2.x}); }
				[[nodiscard]] float MinY() const { return std::min({p0.y, p1.y, p2.y}); }
				[[nodiscard]] float MaxY() const { return std::max({p0.y, p1.y, p2.y}); }
			};

			struct RawGlyph
			{
				std::uint32_t codepoint = 0;
				std::vector<Curve> curves;
				float advance = 0.0f;
				Vec2 boxMin{0.0f, 0.0f};
				Vec2 boxMax{0.0f, 0.0f};
			};

			// ── FreeType RAII ────────────────────────────────────────────────────────────

			struct FaceHandle
			{
				FT_Face face = nullptr;
				FaceHandle() = default;
				FaceHandle(const FaceHandle&) = delete;
				FaceHandle& operator=(const FaceHandle&) = delete;
				FaceHandle(FaceHandle&&) = delete;
				FaceHandle& operator=(FaceHandle&&) = delete;

				~FaceHandle()
				{
					if (face)
					{
						FT_Done_Face(face);
					}
				}
			};

			struct LibraryHandle
			{
				FT_Library library = nullptr;
				LibraryHandle() = default;
				LibraryHandle(const LibraryHandle&) = delete;
				LibraryHandle& operator=(const LibraryHandle&) = delete;
				LibraryHandle(LibraryHandle&&) = delete;
				LibraryHandle& operator=(LibraryHandle&&) = delete;

				~LibraryHandle()
				{
					if (library)
					{
						FT_Done_FreeType(library);
					}
				}
			};

			// ── Outline decomposition ────────────────────────────────────────────────────

			// Walks one glyph's contours and lands every segment in `curves` as a quadratic.
			// Contours are closed explicitly: FreeType reports the closing segment only
			// implicitly, and an unclosed contour leaves a gap the ray-casting coverage reads
			// straight through, which shows up as a glyph bleeding into its own counter.
			struct OutlineSink
			{
				std::vector<Curve> curves;
				Vec2 start{};
				Vec2 cursor{};
				bool open = false;
				float invEm = 1.0f;

				void CloseContour()
				{
					if (open && (cursor.x != start.x || cursor.y != start.y))
					{
						EmitLine(start);
					}
					open = false;
				}

				void EmitLine(Vec2 to)
				{
					// A straight segment is a quadratic whose control point sits on the line,
					// so the shader needs exactly one curve primitive and no special case.
					curves.push_back(Curve{cursor, (cursor + to) * 0.5f, to});
					cursor = to;
				}

				void EmitQuadratic(Vec2 control, Vec2 to)
				{
					curves.push_back(Curve{cursor, control, to});
					cursor = to;
				}

				// Cubics only appear in CFF/Type2 outlines. Split first, then approximate each
				// piece: a single quadratic across a whole cubic visibly cuts corners on the
				// bowls of letters at large sizes, which is exactly where this renderer is
				// supposed to win.
				void EmitCubic(Vec2 c1, Vec2 c2, Vec2 to)
				{
					const Vec2 p0 = cursor;
					Vec2 previous = p0;
					for (int i = 1; i <= kCubicSplits; ++i)
					{
						const float t1 = static_cast<float>(i) / static_cast<float>(kCubicSplits);
						const Vec2 point = CubicAt(p0, c1, c2, to, t1);
						const Vec2 tangentStart = CubicTangent(p0, c1, c2, to, static_cast<float>(i - 1) / static_cast<float>(kCubicSplits));
						const Vec2 tangentEnd = CubicTangent(p0, c1, c2, to, t1);
						curves.push_back(Curve{previous, QuadraticControl(previous, point, tangentStart, tangentEnd), point});
						previous = point;
					}
					cursor = to;
				}

				static Vec2 CubicAt(Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, float t)
				{
					const float u = 1.0f - t;
					return p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + p3 * (t * t * t);
				}

				static Vec2 CubicTangent(Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, float t)
				{
					const float u = 1.0f - t;
					return (c1 - p0) * (3.0f * u * u) + (c2 - c1) * (6.0f * u * t) + (p3 - c2) * (3.0f * t * t);
				}

				// The quadratic through two points with the given end tangents: intersect the
				// tangent lines. Parallel tangents mean the piece is effectively straight, so
				// the midpoint control degenerates it to a line, which is correct.
				static Vec2 QuadraticControl(Vec2 a, Vec2 b, Vec2 tangentA, Vec2 tangentB)
				{
					const float denominator = tangentA.x * tangentB.y - tangentA.y * tangentB.x;
					if (std::fabs(denominator) < 1e-9f)
					{
						return (a + b) * 0.5f;
					}
					const Vec2 delta = b - a;
					const float s = (delta.x * tangentB.y - delta.y * tangentB.x) / denominator;
					return a + tangentA * s;
				}
			};

			int MoveTo(const FT_Vector* to, void* user)
			{
				auto* sink = static_cast<OutlineSink*>(user);
				sink->CloseContour();
				sink->start = {static_cast<float>(to->x) * sink->invEm, static_cast<float>(to->y) * sink->invEm};
				sink->cursor = sink->start;
				sink->open = true;
				return 0;
			}

			int LineTo(const FT_Vector* to, void* user)
			{
				auto* sink = static_cast<OutlineSink*>(user);
				sink->EmitLine({static_cast<float>(to->x) * sink->invEm, static_cast<float>(to->y) * sink->invEm});
				return 0;
			}

			int ConicTo(const FT_Vector* control, const FT_Vector* to, void* user)
			{
				auto* sink = static_cast<OutlineSink*>(user);
				sink->EmitQuadratic(
				        {static_cast<float>(control->x) * sink->invEm, static_cast<float>(control->y) * sink->invEm},
				        {static_cast<float>(to->x) * sink->invEm, static_cast<float>(to->y) * sink->invEm});
				return 0;
			}

			int CubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
			{
				auto* sink = static_cast<OutlineSink*>(user);
				sink->EmitCubic(
				        {static_cast<float>(c1->x) * sink->invEm, static_cast<float>(c1->y) * sink->invEm},
				        {static_cast<float>(c2->x) * sink->invEm, static_cast<float>(c2->y) * sink->invEm},
				        {static_cast<float>(to->x) * sink->invEm, static_cast<float>(to->y) * sink->invEm});
				return 0;
			}

			bool LoadGlyph(FT_Face face, std::uint32_t codepoint, float invEm, RawGlyph& out)
			{
				const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
				if (glyphIndex == 0 && codepoint != 0x20)
				{
					return false;
				}

				// NO_SCALE keeps the outline in font units, which is the only way to bake a
				// resolution-independent asset; everything is divided by the em below.
				if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != FT_Err_Ok)
				{
					return false;
				}

				const FT_GlyphSlot slot = face->glyph;
				out.codepoint = codepoint;
				out.advance = static_cast<float>(slot->advance.x) * invEm;

				if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_points == 0)
				{
					// A blank glyph (space) is legitimate: it advances the pen and draws nothing.
					out.curves.clear();
					out.boxMin = {0.0f, 0.0f};
					out.boxMax = {0.0f, 0.0f};
					return true;
				}

				OutlineSink sink;
				sink.invEm = invEm;
				FT_Outline_Funcs funcs{};
				funcs.move_to = MoveTo;
				funcs.line_to = LineTo;
				funcs.conic_to = ConicTo;
				funcs.cubic_to = CubicTo;
				if (FT_Outline_Decompose(&slot->outline, &funcs, &sink) != FT_Err_Ok)
				{
					return false;
				}
				sink.CloseContour();

				out.curves = std::move(sink.curves);
				if (out.curves.empty())
				{
					out.boxMin = {0.0f, 0.0f};
					out.boxMax = {0.0f, 0.0f};
					return true;
				}

				// The box is taken from the CURVES, control points included, not from
				// FreeType's reported bbox: the quad drawn on screen is this box, and a
				// control point outside it would put part of a stroke outside the quad.
				out.boxMin = {out.curves[0].MinX(), out.curves[0].MinY()};
				out.boxMax = {out.curves[0].MaxX(), out.curves[0].MaxY()};
				for (const Curve& curve: out.curves)
				{
					out.boxMin.x = std::min(out.boxMin.x, curve.MinX());
					out.boxMin.y = std::min(out.boxMin.y, curve.MinY());
					out.boxMax.x = std::max(out.boxMax.x, curve.MaxX());
					out.boxMax.y = std::max(out.boxMax.y, curve.MaxY());
				}
				return true;
			}

			// ── Bands ────────────────────────────────────────────────────────────────────

			// How many slices this glyph's curves are worth. One band per few curves keeps the
			// shader's inner loop short without exploding the duplication a curve spanning
			// many bands costs.
			std::uint32_t BandCountFor(std::size_t curveCount)
			{
				if (curveCount == 0)
				{
					return 0;
				}
				const auto wanted = static_cast<std::uint32_t>(std::lround(static_cast<float>(curveCount) / kBandTargetCurves));
				return std::clamp<std::uint32_t>(wanted, 1u, kMaxBands);
			}

			struct Texel
			{
				float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
			};

			// Appends one glyph's band tables and curve runs, and reports where the band table
			// landed. Curves are duplicated into every band they span - that duplication is the
			// whole point of the structure: it turns the shader's per-pixel work into a flat
			// run over one band with no indirection.
			void AppendGlyph(const RawGlyph& glyph, std::vector<Texel>& texels, GlyphCurve& meta)
			{
				const std::uint32_t bandsX = BandCountFor(glyph.curves.size());
				const std::uint32_t bandsY = bandsX;
				meta.bandCountX = static_cast<std::uint16_t>(bandsX);
				meta.bandCountY = static_cast<std::uint16_t>(bandsY);
				meta.bandTexel = static_cast<std::uint32_t>(texels.size());

				if (bandsX == 0)
				{
					return;
				}

				const float spanX = std::max(glyph.boxMax.x - glyph.boxMin.x, 1e-8f);
				const float spanY = std::max(glyph.boxMax.y - glyph.boxMin.y, 1e-8f);

				// Curves are stored normalised into the box, so the shader maps its own
				// interpolated local coordinate straight onto them with no per-glyph constants.
				const auto normalise = [&](Vec2 p) -> Vec2
				{ return {(p.x - glyph.boxMin.x) / spanX, (p.y - glyph.boxMin.y) / spanY}; };

				std::vector<Curve> unit;
				unit.reserve(glyph.curves.size());
				for (const Curve& curve: glyph.curves)
				{
					unit.push_back(Curve{normalise(curve.p0), normalise(curve.p1), normalise(curve.p2)});
				}

				// Reserve the band table; the runs are appended after it and the entries are
				// filled in as each run's position becomes known.
				const std::size_t tableStart = texels.size();
				texels.resize(tableStart + bandsX + bandsY);

				const auto buildAxis = [&](std::uint32_t bandCount, std::size_t entryBase, bool horizontal)
				{
					for (std::uint32_t band = 0; band < bandCount; ++band)
					{
						const float lo = static_cast<float>(band) / static_cast<float>(bandCount);
						const float hi = static_cast<float>(band + 1) / static_cast<float>(bandCount);

						const auto runStart = static_cast<std::uint32_t>(texels.size());
						std::uint32_t count = 0;
						for (const Curve& curve: unit)
						{
							// A horizontal ray only ever meets curves whose Y range covers its
							// band; a vertical ray, curves whose X range does.
							const float min = horizontal ? curve.MinY() : curve.MinX();
							const float max = horizontal ? curve.MaxY() : curve.MaxX();
							if (max < lo || min > hi)
							{
								continue;
							}
							texels.push_back(Texel{curve.p0.x, curve.p0.y, curve.p1.x, curve.p1.y});
							texels.push_back(Texel{curve.p2.x, curve.p2.y, 0.0f, 0.0f});
							++count;
						}
						texels[entryBase + band] = Texel{static_cast<float>(runStart), static_cast<float>(count), 0.0f, 0.0f};
					}
				};

				buildAxis(bandsX, tableStart, true);
				buildAxis(bandsY, tableStart + bandsX, false);
			}

			bool WriteCurves(const fs::path& path, const FontCurveHeader& header,
			        const std::vector<GlyphCurve>& glyphs, const std::vector<Texel>& texels)
			{
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
				{
					return false;
				}
				out.write(reinterpret_cast<const char*>(&header), sizeof(header));
				if (!glyphs.empty())
				{
					out.write(reinterpret_cast<const char*>(glyphs.data()), static_cast<std::streamsize>(glyphs.size() * sizeof(GlyphCurve)));
				}
				if (!texels.empty())
				{
					out.write(reinterpret_cast<const char*>(texels.data()), static_cast<std::streamsize>(texels.size() * sizeof(Texel)));
				}
				return static_cast<bool>(out);
			}

		} // namespace

		BakeResult BakeFont(const fs::path& ttfPath, const fs::path& outDir)
		{
			BakeResult result;

			if (!fs::exists(ttfPath))
			{
				result.error = "font file not found: " + ttfPath.string();
				return result;
			}

			LibraryHandle libraryHandle;
			if (FT_Init_FreeType(&libraryHandle.library) != FT_Err_Ok)
			{
				result.error = "FT_Init_FreeType failed";
				return result;
			}

			FaceHandle faceHandle;
			const std::string ttfPathStr = ttfPath.string();
			if (FT_New_Face(libraryHandle.library, ttfPathStr.c_str(), 0, &faceHandle.face) != FT_Err_Ok)
			{
				result.error = "FT_New_Face failed for '" + ttfPathStr + "'";
				return result;
			}

			FT_Face face = faceHandle.face;
			if (face->units_per_EM == 0)
			{
				result.error = "'" + ttfPathStr + "' reports no em size - not a scalable outline font";
				return result;
			}
			const float invEm = 1.0f / static_cast<float>(face->units_per_EM);

			std::vector<RawGlyph> glyphs;
			glyphs.reserve(96 + 96);

			const auto collect = [&](std::uint32_t first, std::uint32_t last)
			{
				for (std::uint32_t cp = first; cp <= last; ++cp)
				{
					RawGlyph glyph;
					if (LoadGlyph(face, cp, invEm, glyph))
					{
						glyphs.push_back(std::move(glyph));
					}
				}
			};
			collect(0x20, 0x7E);
			collect(0xA0, 0xFF);

			if (glyphs.empty())
			{
				result.error = "no glyph outlines read from '" + ttfPathStr + "'";
				return result;
			}

			std::vector<GlyphCurve> metas(glyphs.size());
			std::vector<Texel> texels;
			texels.reserve(glyphs.size() * 64);

			std::uint32_t curveTotal = 0;
			for (std::size_t i = 0; i < glyphs.size(); ++i)
			{
				const RawGlyph& glyph = glyphs[i];
				GlyphCurve& meta = metas[i];
				meta.codepoint = glyph.codepoint;
				meta.minX = glyph.boxMin.x;
				meta.minY = glyph.boxMin.y;
				meta.maxX = glyph.boxMax.x;
				meta.maxY = glyph.boxMax.y;
				meta.advance = glyph.advance;
				meta.bearingX = glyph.boxMin.x;
				meta.bearingY = glyph.boxMax.y;
				AppendGlyph(glyph, texels, meta);
				curveTotal += static_cast<std::uint32_t>(glyph.curves.size());
			}

			const auto texelCount = static_cast<std::uint32_t>(texels.size());
			const std::uint32_t height = std::max<std::uint32_t>(1, (texelCount + kCurveTextureWidth - 1) / kCurveTextureWidth);
			// Pad to a whole rectangle so the runtime uploads one contiguous block.
			texels.resize(static_cast<std::size_t>(kCurveTextureWidth) * height);

			FontCurveHeader header{};
			header.magic = kFontCurveMagic;
			header.version = kFontCurveVersion;
			header.glyphCount = static_cast<std::uint32_t>(metas.size());
			header.texelCount = static_cast<std::uint32_t>(texels.size());
			header.textureWidth = kCurveTextureWidth;
			header.textureHeight = height;
			header.ascent = static_cast<float>(face->ascender) * invEm;
			header.descent = -static_cast<float>(face->descender) * invEm; // FreeType reports it negative
			header.lineHeight = static_cast<float>(face->height) * invEm;

			std::error_code ec;
			fs::create_directories(outDir, ec);

			const fs::path curvesPath = outDir / (ttfPath.stem().string() + ".fontcurves");
			if (!WriteCurves(curvesPath, header, metas, texels))
			{
				result.error = "failed to write '" + curvesPath.string() + "'";
				return result;
			}

			result.success = true;
			result.glyphCount = header.glyphCount;
			result.curveCount = curveTotal;
			result.textureWidth = header.textureWidth;
			result.textureHeight = header.textureHeight;
			return result;
		}

	} // namespace FontProcessor
} // namespace aether::assetpipeline
