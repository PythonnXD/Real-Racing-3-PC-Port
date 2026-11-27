#include "java_firemint.h"

#include <windows.h>
#include <dwrite.h>
#include <GLES2/gl2.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "rg_etc1.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>

namespace {

IDWriteFactory* GetDWriteFactory() {
    static IDWriteFactory* factory = []() {
        IDWriteFactory* f = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&f));
        return f;
    }();
    return factory;
}

int NextPOT(int v) {
    int p = 1;
    while (p < v && p < 8192) p <<= 1;
    return p;
}

// UTF-8 decode into Unicode codepoints (UTF-32)
std::vector<uint32_t> DecodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(s[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(s[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = '?'; ++i;
        }
        out.push_back(cp);
    }
    return out;
}

// Zero-width / bidi / format characters that Android's HarfBuzz/Skia ignores (never renders as .notdef tofu).
bool IsZeroWidthIgnorable(uint32_t cp) {
    if (cp < 0x20u && cp != '\t') return true;
    if (cp == 0x00ADu) return true; // soft hyphen
    if (cp >= 0x200Bu && cp <= 0x200Fu) return true; // ZWSP, ZWNJ, ZWJ, LRM, RLM
    if (cp >= 0x202Au && cp <= 0x202Eu) return true; // Bidi embedding/override
    if (cp >= 0x2060u && cp <= 0x206Fu) return true; // Word Joiner, invisible ops, Bidi isolates
    if (cp == 0xFEFFu) return true; // BOM / ZWNBSP
    if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true; // Variation selectors
    return false;
}

// Unicode whitespace characters missing from RR3's OTF/TTF fonts (e.g. U+2009 thin space,
// U+200A hair space, U+202F narrow no-break space) -> advance scale relative to ASCII space ' '.
float WhitespaceAdvanceScale(uint32_t cp) {
    switch (cp) {
        case '\t':    return 4.0f;
        case 0x00A0u: // NBSP
        case 0x2000u: // En quad
        case 0x2001u: // Em quad
        case 0x2002u: // En space
        case 0x2003u: // Em space
        case 0x2007u: // Figure space
        case 0x2008u: // Punctuation space
        case 0x205Fu: // Medium math space
        case 0x3000u: // Ideographic space
            return 1.0f;
        case 0x2004u: // Three-per-em
        case 0x2005u: // Four-per-em
        case 0x2006u: // Six-per-em
        case 0x2009u: // Thin space
        case 0x202Fu: // Narrow no-break space
            return 0.5f;
        case 0x200Au: // Hair space
            return 0.25f;
        default:
            return 0.0f;
    }
}

struct FallbackFaceEntry {
    IDWriteFontFace* face = nullptr;
    uint16_t design_upem = 1000;
};

const std::vector<FallbackFaceEntry>& GetSystemFallbackFaces(bool bold, bool italic) {
    static std::vector<FallbackFaceEntry> cache[4];
    static bool loaded[4] = {false, false, false, false};
    int idx = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (!loaded[idx]) {
        loaded[idx] = true;
        auto* factory = GetDWriteFactory();
        if (factory) {
            DWRITE_FONT_SIMULATIONS sims = DWRITE_FONT_SIMULATIONS_NONE;
            if (bold) sims = static_cast<DWRITE_FONT_SIMULATIONS>(sims | DWRITE_FONT_SIMULATIONS_BOLD);
            if (italic) sims = static_cast<DWRITE_FONT_SIMULATIONS>(sims | DWRITE_FONT_SIMULATIONS_OBLIQUE);
            static const wchar_t* kFallbacks[] = {
                L"C:\\Windows\\Fonts\\segoeui.ttf",
                L"C:\\Windows\\Fonts\\arial.ttf",
                L"C:\\Windows\\Fonts\\seguisym.ttf",
                L"C:\\Windows\\Fonts\\malgun.ttf",
                L"C:\\Windows\\Fonts\\msyh.ttc",
                L"C:\\Windows\\Fonts\\meiryo.ttc",
            };
            for (const wchar_t* fp : kFallbacks) {
                if (!std::filesystem::exists(fp)) continue;
                IDWriteFontFile* file = nullptr;
                if (FAILED(factory->CreateFontFileReference(fp, nullptr, &file)) || !file) continue;
                BOOL supported = FALSE;
                DWRITE_FONT_FILE_TYPE ftype = DWRITE_FONT_FILE_TYPE_UNKNOWN;
                DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
                UINT32 num_faces = 0;
                if (SUCCEEDED(file->Analyze(&supported, &ftype, &face_type, &num_faces)) && supported && num_faces > 0) {
                    IDWriteFontFace* f = nullptr;
                    if (SUCCEEDED(factory->CreateFontFace(face_type, 1, &file, 0, sims, &f)) && f) {
                        DWRITE_FONT_METRICS fm{};
                        f->GetMetrics(&fm);
                        cache[idx].push_back({f, fm.designUnitsPerEm ? fm.designUnitsPerEm : uint16_t(1000)});
                    }
                }
                file->Release();
            }
        }
    }
    return cache[idx];
}

struct ResolvedGlyph {
    IDWriteFontFace* face = nullptr; // nullptr for pure whitespace gap
    uint16_t upem = 1000;
    uint16_t gid = 0;
    float advance = 0.0f;
};

struct FontState {
    IDWriteFontFace* face = nullptr;
    bool is_bold = false;
    bool is_italic = false;
    float size = 16.0f;
    float width_scale = 1.0f;
    float ascent = 12.0f;
    float descent = 4.0f;
    float height = 16.0f;
    float leading = 0.0f;
    uint16_t design_upem = 1000;
    float space_advance = 4.0f;

    ~FontState() {
        if (face) { face->Release(); face = nullptr; }
    }

    float Scale() const { return design_upem ? (size / static_cast<float>(design_upem)) : 0.016f; }

    bool LoadFromFile(const std::filesystem::path& path, bool bold, bool italic, float sz, float wscale) {
        auto* factory = GetDWriteFactory();
        if (!factory) return false;
        std::filesystem::path p = path;
        if (!std::filesystem::exists(p)) {
            p = L"C:\\Windows\\Fonts\\segoeui.ttf";
            if (!std::filesystem::exists(p)) p = L"C:\\Windows\\Fonts\\arial.ttf";
        }
        IDWriteFontFile* file = nullptr;
        std::wstring wp = p.wstring();
        if (FAILED(factory->CreateFontFileReference(wp.c_str(), nullptr, &file)) || !file) {
            return false;
        }
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE ftype = DWRITE_FONT_FILE_TYPE_UNKNOWN;
        DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        UINT32 num_faces = 0;
        HRESULT hr = file->Analyze(&supported, &ftype, &face_type, &num_faces);
        if (FAILED(hr) || !supported || num_faces == 0) {
            file->Release();
            return false;
        }
        DWRITE_FONT_SIMULATIONS sims = DWRITE_FONT_SIMULATIONS_NONE;
        if (bold) sims = static_cast<DWRITE_FONT_SIMULATIONS>(sims | DWRITE_FONT_SIMULATIONS_BOLD);
        if (italic) sims = static_cast<DWRITE_FONT_SIMULATIONS>(sims | DWRITE_FONT_SIMULATIONS_OBLIQUE);

        if (face) { face->Release(); face = nullptr; }
        hr = factory->CreateFontFace(face_type, 1, &file, 0, sims, &face);
        file->Release();
        if (FAILED(hr) || !face) return false;

        is_bold = bold;
        is_italic = italic;
        size = sz > 0.0f ? sz : 16.0f;
        width_scale = wscale > 0.0f ? wscale : 1.0f;
        DWRITE_FONT_METRICS fm{};
        face->GetMetrics(&fm);
        design_upem = fm.designUnitsPerEm ? fm.designUnitsPerEm : 1000;
        float sc = Scale();
        ascent = fm.ascent * sc;
        descent = fm.descent * sc;
        leading = fm.lineGap * sc;
        height = ascent + descent;

        UINT32 sp_cp = 0x20u;
        UINT16 sp_gid = 0;
        face->GetGlyphIndices(&sp_cp, 1, &sp_gid);
        if (sp_gid != 0) {
            DWRITE_GLYPH_METRICS gm{};
            face->GetDesignGlyphMetrics(&sp_gid, 1, &gm, FALSE);
            space_advance = gm.advanceWidth * sc * width_scale;
        } else {
            space_advance = size * 0.25f * width_scale;
        }
        return true;
    }

    // Resolves every codepoint without ever emitting .notdef tofu [X] for invisible format
    // characters or missing Unicode whitespace; falls back to Windows system fonts for missing
    // visible glyphs (matching Android Skia/Minikin behavior).
    float ResolveGlyphs(const std::vector<uint32_t>& cps, std::vector<ResolvedGlyph>& out) const {
        out.clear();
        if (!face || cps.empty()) return 0.0f;

        std::vector<uint16_t> primary_gids(cps.size(), 0);
        face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), primary_gids.data());

        // Fast path: check if any codepoint needs stripping, whitespace synthesis, or fallback.
        bool needs_special = false;
        for (size_t i = 0; i < cps.size(); ++i) {
            if (IsZeroWidthIgnorable(cps[i]) || primary_gids[i] == 0) {
                needs_special = true;
                break;
            }
        }
        if (!needs_special) {
            std::vector<DWRITE_GLYPH_METRICS> gms(cps.size());
            face->GetDesignGlyphMetrics(primary_gids.data(), static_cast<UINT32>(cps.size()), gms.data(), FALSE);
            float sc = Scale() * width_scale;
            float total = 0.0f;
            out.reserve(cps.size());
            for (size_t i = 0; i < cps.size(); ++i) {
                float adv = gms[i].advanceWidth * sc;
                out.push_back({face, design_upem, primary_gids[i], adv});
                total += adv;
            }
            return total;
        }

        const auto& fallbacks = GetSystemFallbackFaces(is_bold, is_italic);
        float sc = Scale() * width_scale;
        float total = 0.0f;
        out.reserve(cps.size());
        for (size_t i = 0; i < cps.size(); ++i) {
            uint32_t cp = cps[i];
            if (IsZeroWidthIgnorable(cp)) {
                continue;
            }
            uint16_t gid = primary_gids[i];
            if (gid != 0) {
                DWRITE_GLYPH_METRICS gm{};
                face->GetDesignGlyphMetrics(&gid, 1, &gm, FALSE);
                float adv = gm.advanceWidth * sc;
                out.push_back({face, design_upem, gid, adv});
                total += adv;
                continue;
            }
            float ws_scale = WhitespaceAdvanceScale(cp);
            if (ws_scale > 0.0f) {
                float adv = space_advance * ws_scale;
                out.push_back({nullptr, design_upem, 0, adv});
                total += adv;
                continue;
            }
            // Visible character missing in game font: try system fallback fonts.
            bool resolved = false;
            for (const auto& fb : fallbacks) {
                UINT16 fb_gid = 0;
                fb.face->GetGlyphIndices(&cp, 1, &fb_gid);
                if (fb_gid != 0) {
                    DWRITE_GLYPH_METRICS gm{};
                    fb.face->GetDesignGlyphMetrics(&fb_gid, 1, &gm, FALSE);
                    float fb_sc = (size / static_cast<float>(fb.design_upem)) * width_scale;
                    float adv = gm.advanceWidth * fb_sc;
                    out.push_back({fb.face, fb.design_upem, fb_gid, adv});
                    total += adv;
                    resolved = true;
                    break;
                }
            }
            // If still missing anywhere on the system, skip instead of drawing an ugly [X] box.
            (void)resolved;
        }
        return total;
    }

    float MeasureLine(const std::vector<uint32_t>& cps, std::vector<ResolvedGlyph>& out) const {
        return ResolveGlyphs(cps, out);
    }

    // Fast tight ink bounding box (screen Y-down, baseline at y=0) via GetDesignGlyphMetrics — zero rasterization!
    bool MeasureTightBounds(const std::vector<uint32_t>& cps, std::vector<ResolvedGlyph>& out,
                            float& out_advance, RECT& out_bounds) const {
        out_advance = ResolveGlyphs(cps, out);
        if (out.empty()) return false;

        float pen_x = 0.0f;
        float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
        bool any_ink = false;

        for (const auto& g : out) {
            if (g.face && g.gid != 0) {
                DWRITE_GLYPH_METRICS gm{};
                g.face->GetDesignGlyphMetrics(&g.gid, 1, &gm, FALSE);
                const float sy = g.upem ? (size / static_cast<float>(g.upem)) : 0.016f;
                const float sx = sy * width_scale;
                int32_t bw = static_cast<int32_t>(gm.advanceWidth) - gm.leftSideBearing - gm.rightSideBearing;
                int32_t bh = static_cast<int32_t>(gm.advanceHeight) - gm.topSideBearing - gm.bottomSideBearing;
                if (bw > 0 && bh > 0) {
                    any_ink = true;
                    float gx0 = pen_x + static_cast<float>(gm.leftSideBearing) * sx;
                    float gx1 = gx0 + static_cast<float>(bw) * sx;
                    float y_max = static_cast<float>(gm.verticalOriginY - gm.topSideBearing) * sy;
                    float y_min = static_cast<float>(gm.verticalOriginY - static_cast<int32_t>(gm.advanceHeight) + gm.bottomSideBearing) * sy;
                    float gy0 = -y_max;
                    float gy1 = -y_min;
                    if (gy0 > gy1) std::swap(gy0, gy1);
                    min_x = std::min(min_x, gx0);
                    max_x = std::max(max_x, gx1);
                    min_y = std::min(min_y, gy0);
                    max_y = std::max(max_y, gy1);
                }
            }
            pen_x += g.advance;
        }
        if (!any_ink) return false;
        out_bounds.left = static_cast<LONG>(std::floor(min_x));
        out_bounds.top = static_cast<LONG>(std::floor(min_y));
        out_bounds.right = static_cast<LONG>(std::ceil(max_x));
        out_bounds.bottom = static_cast<LONG>(std::ceil(max_y));
        return (out_bounds.right > out_bounds.left && out_bounds.bottom > out_bounds.top);
    }

    // Rasterize a single-face glyph segment into an alpha-only bitmap.
    bool RasterizeSegment(IDWriteFontFace* seg_face, const std::vector<uint16_t>& gids,
                          const std::vector<float>& advs, float origin_x, float origin_y,
                          RECT& out_bounds, std::vector<uint8_t>& out_alpha,
                          float extra_scale = 1.0f) const {
        auto* factory = GetDWriteFactory();
        if (!factory || !seg_face || gids.empty()) return false;
        std::vector<float> scaled_advs;
        const float* adv_ptr = advs.data();
        if (std::fabs(extra_scale - 1.0f) > 1e-4f) {
            scaled_advs.resize(advs.size());
            for (size_t i = 0; i < advs.size(); ++i) scaled_advs[i] = advs[i] * extra_scale;
            adv_ptr = scaled_advs.data();
        }
        DWRITE_GLYPH_RUN run{};
        run.fontFace = seg_face;
        run.fontEmSize = size * extra_scale;
        run.glyphCount = static_cast<UINT32>(gids.size());
        run.glyphIndices = gids.data();
        run.glyphAdvances = adv_ptr;
        run.glyphOffsets = nullptr;
        run.isSideways = FALSE;
        run.bidiLevel = 0;

        DWRITE_MATRIX xform{width_scale, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        IDWriteGlyphRunAnalysis* analysis = nullptr;
        HRESULT hr = factory->CreateGlyphRunAnalysis(
            &run, 1.0f, &xform,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            DWRITE_MEASURING_MODE_NATURAL,
            origin_x, origin_y, &analysis);
        if (FAILED(hr) || !analysis) return false;

        hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &out_bounds);
        if (FAILED(hr) || out_bounds.right <= out_bounds.left || out_bounds.bottom <= out_bounds.top) {
            hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &out_bounds);
            if (FAILED(hr) || out_bounds.right <= out_bounds.left || out_bounds.bottom <= out_bounds.top) {
                analysis->Release();
                return false;
            }
            int w = out_bounds.right - out_bounds.left;
            int h = out_bounds.bottom - out_bounds.top;
            out_alpha.assign(static_cast<size_t>(w) * h, 0);
            hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &out_bounds,
                                              out_alpha.data(), static_cast<UINT32>(out_alpha.size()));
            analysis->Release();
            return SUCCEEDED(hr);
        }

        int w = out_bounds.right - out_bounds.left;
        int h = out_bounds.bottom - out_bounds.top;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &out_bounds,
                                          rgb.data(), static_cast<UINT32>(rgb.size()));
        analysis->Release();
        if (FAILED(hr)) return false;

        out_alpha.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < out_alpha.size(); ++i) {
            out_alpha[i] = static_cast<uint8_t>((static_cast<uint32_t>(rgb[i * 3]) +
                                                 rgb[i * 3 + 1] + rgb[i * 3 + 2]) / 3);
        }
        return true;
    }

    // Rasterize a resolved glyph run (supports mixed primary + fallback faces + synthetic whitespace).
    bool RasterizeRun(const std::vector<ResolvedGlyph>& glyphs,
                      float origin_x, float origin_y,
                      RECT& out_bounds, std::vector<uint8_t>& out_alpha,
                      float extra_scale = 1.0f) const {
        if (glyphs.empty()) return false;
        struct SegImg { RECT rb; std::vector<uint8_t> alpha; };
        std::vector<SegImg> segs;

        float pen_x = 0.0f;
        size_t i = 0;
        while (i < glyphs.size()) {
            if (!glyphs[i].face || glyphs[i].gid == 0) {
                pen_x += glyphs[i].advance * extra_scale;
                ++i;
                continue;
            }
            IDWriteFontFace* seg_face = glyphs[i].face;
            float seg_x = origin_x + pen_x;
            std::vector<uint16_t> gids;
            std::vector<float> advs;
            while (i < glyphs.size() && glyphs[i].face == seg_face && glyphs[i].gid != 0) {
                gids.push_back(glyphs[i].gid);
                advs.push_back(glyphs[i].advance);
                pen_x += glyphs[i].advance * extra_scale;
                ++i;
            }
            RECT rb{};
            std::vector<uint8_t> a;
            if (RasterizeSegment(seg_face, gids, advs, seg_x, origin_y, rb, a, extra_scale)) {
                segs.push_back({rb, std::move(a)});
            }
        }
        if (segs.empty()) return false;
        if (segs.size() == 1) {
            out_bounds = segs[0].rb;
            out_alpha = std::move(segs[0].alpha);
            return true;
        }
        out_bounds = segs[0].rb;
        for (size_t s = 1; s < segs.size(); ++s) {
            out_bounds.left = std::min(out_bounds.left, segs[s].rb.left);
            out_bounds.top = std::min(out_bounds.top, segs[s].rb.top);
            out_bounds.right = std::max(out_bounds.right, segs[s].rb.right);
            out_bounds.bottom = std::max(out_bounds.bottom, segs[s].rb.bottom);
        }
        int tw = out_bounds.right - out_bounds.left;
        int th = out_bounds.bottom - out_bounds.top;
        out_alpha.assign(static_cast<size_t>(tw) * th, 0);
        for (const auto& sg : segs) {
            int sw = sg.rb.right - sg.rb.left;
            int sh = sg.rb.bottom - sg.rb.top;
            int ox = sg.rb.left - out_bounds.left;
            int oy = sg.rb.top - out_bounds.top;
            for (int y = 0; y < sh; ++y) {
                for (int x = 0; x < sw; ++x) {
                    size_t dst = static_cast<size_t>(oy + y) * tw + (ox + x);
                    out_alpha[dst] = std::max(out_alpha[dst], sg.alpha[static_cast<size_t>(y) * sw + x]);
                }
            }
        }
        return true;
    }
};

struct GlyphVectorState {
    std::shared_ptr<FontState> font;
    std::vector<std::vector<uint32_t>> lines;
    std::vector<float> line_widths;
    float max_line_w = 0.0f;
    float total_h = 0.0f;
    int align = 0; // 0=left, 1=right, 2=center
    bool is_paragraph = false;
    RECT tight_bounds{0, 0, 1, 1}; // Single-line Paint.getTextBounds equivalent (baseline at y=0)
    float pixel_top_offset = 0.0f; // Paragraph line-0 ink top offset from layout top
};

} // namespace

FiremintRuntime::FiremintRuntime(CpuRuntime& cpu, ElfLoader& loader, GlesBridge& gles,
                                 FakeJvm& jvm, LayeredVfs& vfs, const PcConfig& cfg)
    : cpu_(cpu), loader_(loader), gles_(gles), jvm_(jvm), vfs_(vfs), cfg_(cfg) {
    static bool etc1_inited = []() { rg_etc1::pack_etc1_block_init(); return true; }();
    (void)etc1_inited;
}

FiremintRuntime::~FiremintRuntime() = default;

void FiremintRuntime::PostGlCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(cb_mtx_);
    gl_callbacks_.push_back(std::move(cb));
}

void FiremintRuntime::PumpGlCallbacks() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        batch.swap(gl_callbacks_);
    }
    for (auto& cb : batch) {
        if (cb) cb();
    }
}

void FiremintRuntime::RegisterClasses() {
    // AssetManager singleton
    asset_manager_handle_ = jvm_.NewObjectInstance("android/content/res/AssetManager");
    jvm_.PinHandle(asset_manager_handle_);

    // ---- MainActivity ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/MainActivity";
        d.super_name = "com/firemonkeys/cloudcellapi/CC_Activity";

        bool tv = cfg_.android_tv_mode;
        d.static_methods["ExactlyAPI"] = [](FakeJvm&, uint64_t, const auto& a) {
            return JniVal::Bool(!a.empty() && a[0].AsInt() == 31);
        };
        d.static_methods["IsAtLeastAPI"] = [](FakeJvm&, uint64_t, const auto& a) {
            return JniVal::Bool(!a.empty() && 31 >= a[0].AsInt());
        };
        d.static_methods["IsBelowAPI"] = [](FakeJvm&, uint64_t, const auto& a) {
            return JniVal::Bool(!a.empty() && 31 < a[0].AsInt());
        };
        d.static_methods["getIsAmazon"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["getIsAndroidTv"] = [tv](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(tv); };
        d.static_methods["getGameState"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(3); };
        d.static_methods["setGameState"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["logi"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (IsVerboseLogging() && !a.empty()) std::fprintf(stdout, "[RR3 Java INFO] %s\n", j.GetStringUtf(a[0].AsObj()).c_str());
            return JniVal::Void();
        };
        d.static_methods["logw"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (IsVerboseLogging() && !a.empty()) std::fprintf(stderr, "[RR3 Java WARN] %s\n", j.GetStringUtf(a[0].AsObj()).c_str());
            return JniVal::Void();
        };
        d.static_methods["loge"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (IsVerboseLogging() && !a.empty()) std::fprintf(stderr, "[RR3 Java ERR] %s\n", j.GetStringUtf(a[0].AsObj()).c_str());
            return JniVal::Void();
        };

        d.methods["getApiLevel"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(31); };
        d.methods["HasDisplayCutout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["ShouldUseDefaultDisplayCutout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["createControllerManager"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["GetCutoutLeft"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.methods["GetCutoutRight"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.methods["GetCutoutTop"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.methods["GetCutoutBottom"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.methods["getScreenOrientation"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(2); };
        d.methods["getScreenRotation"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.methods["getPaused"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["hasWindowFocus"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["checkStorage"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["setSkipOnDestroyJNI"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["finishActivity"] = [this](FakeJvm&, uint64_t, const auto&) { should_exit_ = true; return JniVal::Void(); };
        d.methods["showSplash"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["hideSplash"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["showLoadingSplash"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["hideLoadingSplash"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["showMessage"] = [this](FakeJvm& j, uint64_t, const auto& a) {
            std::string title = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "";
            std::string body = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "";
            std::string btn0 = a.size() > 2 ? j.GetStringUtf(a[2].AsObj()) : "";
            int64_t cb0 = a.size() > 5 ? a[5].AsLong() : 0;
            std::fprintf(stdout, "[RR3 Dialog] '%s': %s [btn0='%s', cb0=0x%llx]\n",
                         title.c_str(), body.c_str(), btn0.c_str(), static_cast<unsigned long long>(cb0));
            if (cb0 != 0) {
                uint64_t fn = loader_.FindExport("Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback");
                if (fn) {
                    PostGlCallback([this, fn, cb0]() {
                        cpu_.CallGuest(fn, {jvm_.GetJniEnvGuestPtr(), main_activity_handle_, static_cast<uint64_t>(cb0)});
                    });
                }
            }
            return JniVal::Void();
        };
        d.methods["dismissMessage"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["IsPermissionGranted"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["RequestPermission"] = [this](FakeJvm&, uint64_t, const auto&) {
            uint64_t fn = loader_.FindExport("Java_com_firemint_realracing_MainActivity_permissionRequestCompleted");
            if (fn) {
                PostGlCallback([this, fn]() {
                    cpu_.CallGuest(fn, {jvm_.GetJniEnvGuestPtr(), main_activity_handle_, 1});
                });
            }
            return JniVal::Void();
        };
        d.methods["OnLegalOptOutChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["showKeyboardInput"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["hideKeyboardInput"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["RunOnGlThread"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["runOnUiThread"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };

        jvm_.RegisterJniClass(std::move(d));
        main_activity_handle_ = jvm_.NewObjectInstance("com/firemint/realracing/MainActivity");
        jvm_.PinHandle(main_activity_handle_);
        jvm_.SetStaticField("com/firemint/realracing/MainActivity", "instance", JniVal::Obj(main_activity_handle_));
    }

    // ---- AppProxy ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/AppProxy";
        d.static_methods["GetContext"] = [this](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(main_activity_handle_); };
        d.static_methods["GetActivity"] = [this](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(main_activity_handle_); };
        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- TextureInfo ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/TextureInfo";
        d.default_fields["texId"] = JniVal::Int(0);
        d.default_fields["width"] = JniVal::Int(0);
        d.default_fields["height"] = JniVal::Int(0);
        d.default_fields["texWidth"] = JniVal::Int(0);
        d.default_fields["texHeight"] = JniVal::Int(0);
        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- Platform ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/Platform";
        d.static_methods["getApiLevel"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(31); };
        d.static_methods["IsInForeground"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["getModelID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("SM-S918B")); };
        d.static_methods["getModelName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("dm3qxxx")); };
        d.static_methods["getBuildDevice"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("dm3q")); };
        d.static_methods["getBuildManufacturer"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung")); };
        d.static_methods["getCpuName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("Snapdragon 8 Gen 2")); };
        d.static_methods["getCpuCount"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(8); };
        d.static_methods["getCpuSpeed"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(3200000); };
        d.static_methods["getTotalMemory"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(8192); };
        d.static_methods["getScreenWidth"] = [this](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(gles_.RenderWidth()); };
        d.static_methods["getScreenHeight"] = [this](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(gles_.RenderHeight()); };
        d.static_methods["getScreenDPI"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(320); };
        d.static_methods["getScreenDensityName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("xhigh")); };
        d.static_methods["getScreenSize"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(3); };
        d.static_methods["getScreenSizeName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("large")); };
        d.static_methods["getScreenRotation"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        d.static_methods["getSupportedAbis"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("arm64-v8a")); };
        d.static_methods["getPackageName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("com.ea.games.r3_row")); };
        d.static_methods["getAppName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("Real Racing 3 PC Port")); };
        d.static_methods["getAppVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("14.0.1")); };
        d.static_methods["getAppInstallTime"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Long(1700000000000LL); };
        d.static_methods["getAppPath"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("/storage/rr3/base.apk")); };
        d.static_methods["getOsVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("12")); };
        d.static_methods["GetDeviceHardware"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("qcom")); };
        d.static_methods["getDeviceUID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("rr3pc-offline-0001")); };
        d.static_methods["getExternalStorageState"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(2); };
        d.static_methods["getExternalStorageDir"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("/storage/rr3")); };
        d.static_methods["getLegacyExternalStorageDirPath"] = d.static_methods["getExternalStorageDir"];
        d.static_methods["ShouldUseLegacyExternalFilesDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            uint64_t b = j.NewObjectInstance("java/lang/Boolean");
            j.SetInstanceField(b, "value", JniVal::Bool(false));
            return JniVal::Obj(b);
        };
        d.static_methods["getAssetManager"] = [this](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(asset_manager_handle_); };
        d.static_methods["getLocale"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("en_US")); };
        d.static_methods["getCountry"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("US")); };
        d.static_methods["isNetworkAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isMobileDataAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["getMobileDataType"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetWifiSSID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["getCarrier"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["getCarrierName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["getCloudcellEnvironment"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("live")); };
        d.static_methods["getStarlightEnvironment"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("live")); };
        d.static_methods["getSystemMediaVolume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Float(1.0f); };
        d.static_methods["GetAudioSampleRate"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(48000); };
        d.static_methods["GetAudioFramesPerBuffer"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(256); };
        d.static_methods["getAppMemoryUsage"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Long(256LL * 1024 * 1024); };
        d.static_methods["getAppMemoryFree"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Long(4096LL * 1024 * 1024); };
        d.static_methods["getStringExtra"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        d.static_methods["toggleIdleMode"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["memoryProbe"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["exitApp"] = [this](FakeJvm&, uint64_t, const auto&) { should_exit_ = true; return JniVal::Void(); };
        d.static_methods["openURL"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (IsVerboseLogging() && !a.empty()) {
                std::fprintf(stdout, "[OFFLINE] Platform.openURL suppressed: %s\n", j.GetStringUtf(a[0].AsObj()).c_str());
            }
            return JniVal::Void();
        };
        d.static_methods["openNetworkSettings"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["isNetworkSettingsShown"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["hasCameraPermission"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isNotificationPermissionGranted"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["requestCameraPermission"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["requestPostNotificationsPermission"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["requestExactNotifications"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["extractRes"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["deleteDirectory"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["SetClipboardText"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["showMessage"] = [this](FakeJvm& j, uint64_t, const auto& a) {
            std::string title = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "";
            std::string body = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "";
            int64_t cb0 = a.size() > 5 ? a[5].AsLong() : 0;
            std::fprintf(stdout, "[RR3 Platform Dialog] '%s': %s\n", title.c_str(), body.c_str());
            if (cb0 != 0) {
                uint64_t fn = loader_.FindExport("Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback");
                if (fn) {
                    PostGlCallback([this, fn, cb0]() {
                        cpu_.CallGuest(fn, {jvm_.GetJniEnvGuestPtr(), main_activity_handle_, static_cast<uint64_t>(cb0)});
                    });
                }
            }
            return JniVal::Void();
        };
        d.static_methods["dismissMessage"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["GetFloatValueFromRemoteConfig"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Float(0.0f); };
        d.static_methods["GetStringValueFromRemoteConfig"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetBooleanValueFromRemoteConfig"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["setCrashlyticsInt"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["setCrashlyticsFloat"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["setCrashlyticsString"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["addCrashlyticsLogEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["addAnalyticsLogEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["addLevelAnalyticsLogEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["OnLegalOptOutChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["InitialiseSingular"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SetSingularCustomUserId"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["UnsetSingularCustomUserId"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SingularEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SingularRevenue"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SingularRevenueWithProductDetails"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SingularRevenueWithPurchaseReceipt"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SingularLimitDataSharing"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["saveToImageGallery"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["getAmazonAdvertisingId"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["testCrashlyticsCrash"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["RedirectToPlayStore"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };

        // Platform.loadTextureFromMemory(byte[] data, int len) -> TextureInfo (exact Platform.java:545-583 contract)
        d.static_methods["loadTextureFromMemory"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (a.size() < 2) return JniVal::Obj(0);
            auto arr = j.GetJniObject(a[0].AsObj());
            int len = a[1].AsInt();
            if (!arr || len <= 0 || static_cast<size_t>(len) > arr->bytes.size()) return JniVal::Obj(0);

            int w = 0, h = 0, comp = 0;
            unsigned char* pixels = stbi_load_from_memory(arr->bytes.data(), len, &w, &h, &comp, 4);
            if (!pixels || w <= 0 || h <= 0) {
                if (pixels) stbi_image_free(pixels);
                return JniVal::Obj(0);
            }
            int tex_w = NextPOT(w);
            int tex_h = NextPOT(h);

            // Flip Y and place at (0, tex_h - h) matching Platform.java:558-572
            std::vector<uint8_t> flipped(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(flipped.data() + static_cast<size_t>(h - 1 - y) * w * 4,
                            pixels + static_cast<size_t>(y) * w * 4,
                            static_cast<size_t>(w) * 4);
            }
            stbi_image_free(pixels);

            GLint prev_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, tex_h - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));

            uint64_t ti = j.NewObjectInstance("com/firemint/realracing/TextureInfo");
            j.SetInstanceField(ti, "texId", JniVal::Int(static_cast<int32_t>(tex)));
            j.SetInstanceField(ti, "width", JniVal::Int(w));
            j.SetInstanceField(ti, "height", JniVal::Int(h));
            j.SetInstanceField(ti, "texWidth", JniVal::Int(tex_w));
            j.SetInstanceField(ti, "texHeight", JniVal::Int(tex_h));
            return JniVal::Obj(ti);
        };

        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- Font (DirectWrite backed) ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/Font";
        d.create_native_state = []() -> std::any { return std::make_shared<FontState>(); };
        d.default_fields["ascent"] = JniVal::Float(0.0f);
        d.default_fields["descent"] = JniVal::Float(0.0f);
        d.default_fields["height"] = JniVal::Float(0.0f);
        d.default_fields["top"] = JniVal::Float(0.0f);
        d.default_fields["bottom"] = JniVal::Float(0.0f);
        d.default_fields["leading"] = JniVal::Float(0.0f);
        d.default_fields["glyphOffX"] = JniVal::Float(0.0f);
        d.default_fields["glyphOffY"] = JniVal::Float(0.0f);
        d.default_fields["glyphWidth"] = JniVal::Float(0.0f);
        d.default_fields["glyphHeight"] = JniVal::Float(0.0f);
        d.default_fields["glyphAdvance"] = JniVal::Float(0.0f);
        d.default_fields["bmpLeft"] = JniVal::Int(0);
        d.default_fields["bmpTop"] = JniVal::Int(0);
        d.default_fields["bmpWidth"] = JniVal::Int(0);
        d.default_fields["bmpHeight"] = JniVal::Int(0);
        d.default_fields["bmpPitch"] = JniVal::Int(0);
        d.default_fields["bmpData"] = JniVal::Obj(0);

        d.methods["init"] = [this](FakeJvm& j, uint64_t self, const auto& a) {
            auto obj = j.GetJniObject(self);
            if (!obj || a.size() < 5) return JniVal::Bool(false);
            if (!obj->native_state.has_value()) obj->native_state = std::make_shared<FontState>();
            auto fs = std::any_cast<std::shared_ptr<FontState>>(obj->native_state);
            std::string android_path = j.GetStringUtf(a[0].AsObj());
            auto host_path = vfs_.ResolveRead(android_path);
            bool ok = fs->LoadFromFile(host_path, a[1].AsBool(), a[2].AsBool(), a[3].AsFloat(), a[4].AsFloat());
            if (!ok) return JniVal::Bool(false);
            j.SetInstanceField(self, "ascent", JniVal::Float(fs->ascent));
            j.SetInstanceField(self, "descent", JniVal::Float(fs->descent));
            j.SetInstanceField(self, "height", JniVal::Float(fs->height));
            j.SetInstanceField(self, "top", JniVal::Float(fs->ascent));
            j.SetInstanceField(self, "bottom", JniVal::Float(fs->descent));
            j.SetInstanceField(self, "leading", JniVal::Float(fs->leading));
            return JniVal::Bool(true);
        };
        d.methods["loadGlyph"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value() || a.empty()) return JniVal::Bool(false);
            auto fs = std::any_cast<std::shared_ptr<FontState>>(obj->native_state);
            if (!fs->face) return JniVal::Bool(false);
            UINT32 cp = static_cast<UINT32>(a[0].AsInt());
            std::vector<ResolvedGlyph> glyphs;
            float adv = 0.0f;
            RECT rb{};
            if (fs->MeasureTightBounds({cp}, glyphs, adv, rb)) {
                j.SetInstanceField(self, "glyphOffX", JniVal::Float(static_cast<float>(rb.left)));
                j.SetInstanceField(self, "glyphOffY", JniVal::Float(static_cast<float>(-rb.top)));
                j.SetInstanceField(self, "glyphWidth", JniVal::Float(static_cast<float>(rb.right - rb.left)));
                j.SetInstanceField(self, "glyphHeight", JniVal::Float(static_cast<float>(rb.bottom - rb.top)));
                j.SetInstanceField(self, "glyphAdvance", JniVal::Float(adv));
            } else {
                j.SetInstanceField(self, "glyphOffX", JniVal::Float(0.0f));
                j.SetInstanceField(self, "glyphOffY", JniVal::Float(0.0f));
                j.SetInstanceField(self, "glyphWidth", JniVal::Float(0.0f));
                j.SetInstanceField(self, "glyphHeight", JniVal::Float(0.0f));
                j.SetInstanceField(self, "glyphAdvance", JniVal::Float(adv));
            }
            return JniVal::Bool(true);
        };
        auto load_bmp = [](FakeJvm& j, uint64_t self, const std::vector<JniVal>& a) -> JniVal {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value() || a.size() < 5) return JniVal::Bool(false);
            auto fs = std::any_cast<std::shared_ptr<FontState>>(obj->native_state);
            if (!fs->face) return JniVal::Bool(false);
            UINT32 cp = static_cast<UINT32>(a[0].AsInt());
            float fx = a[1].AsFloat();
            float fy = -a[2].AsFloat();
            std::vector<ResolvedGlyph> glyphs;
            fs->MeasureLine({cp}, glyphs);
            RECT b{};
            std::vector<uint8_t> alpha;
            if (!fs->RasterizeRun(glyphs, fx, fy, b, alpha)) {
                return JniVal::Bool(false);
            }
            int w = b.right - b.left;
            int h = b.bottom - b.top;
            j.SetInstanceField(self, "bmpLeft", JniVal::Int(b.left));
            j.SetInstanceField(self, "bmpTop", JniVal::Int(-b.top));
            j.SetInstanceField(self, "bmpWidth", JniVal::Int(w));
            j.SetInstanceField(self, "bmpHeight", JniVal::Int(h));
            j.SetInstanceField(self, "bmpPitch", JniVal::Int(w));
            j.SetInstanceField(self, "bmpData", JniVal::Obj(j.NewByteArray(alpha.data(), alpha.size())));
            return JniVal::Bool(true);
        };
        d.methods["loadBitmap"] = load_bmp;
        d.methods["loadBitmapDynamic"] = load_bmp;
        d.methods["getSize"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value()) return JniVal::Float(16.0f);
            return JniVal::Float(std::any_cast<std::shared_ptr<FontState>>(obj->native_state)->size);
        };
        d.methods["getWidthScale"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value()) return JniVal::Float(1.0f);
            return JniVal::Float(std::any_cast<std::shared_ptr<FontState>>(obj->native_state)->width_scale);
        };
        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- GlyphVector (DirectWrite layout + GL texture upload) ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/GlyphVector";
        d.create_native_state = []() -> std::any { return std::make_shared<GlyphVectorState>(); };
        d.default_fields["offsetX"] = JniVal::Float(0.0f);
        d.default_fields["offsetY"] = JniVal::Float(0.0f);
        d.default_fields["boundsW"] = JniVal::Float(0.0f);
        d.default_fields["boundsH"] = JniVal::Float(0.0f);
        d.default_fields["pixelBoundsW"] = JniVal::Float(0.0f);
        d.default_fields["pixelBoundsH"] = JniVal::Float(0.0f);
        d.default_fields["pixelTopOffset"] = JniVal::Float(0.0f);
        d.default_fields["numLines"] = JniVal::Int(0);
        d.default_fields["texId"] = JniVal::Int(-1);
        d.default_fields["texWidth"] = JniVal::Int(0);
        d.default_fields["texHeight"] = JniVal::Int(0);

        // Exact GlyphVector.java:54-78 contract: single-line tight ink bounds (critical for r3_symbols.ttf
        // where hhea.ascent=5628 vs glyph height ~2000!).
        d.methods["init"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (a.size() < 2) return JniVal::Void();
            auto obj = j.GetJniObject(self);
            auto fobj = j.GetJniObject(a[0].AsObj());
            if (!obj || !fobj || !fobj->native_state.has_value()) return JniVal::Void();
            if (!obj->native_state.has_value()) obj->native_state = std::make_shared<GlyphVectorState>();
            auto gv = std::any_cast<std::shared_ptr<GlyphVectorState>>(obj->native_state);
            gv->font = std::any_cast<std::shared_ptr<FontState>>(fobj->native_state);
            gv->lines.clear();
            gv->line_widths.clear();
            gv->align = 0;
            gv->is_paragraph = false;
            gv->pixel_top_offset = 0.0f;

            auto cps = DecodeUtf8(j.GetStringUtf(a[1].AsObj()));
            std::vector<ResolvedGlyph> glyphs;
            float lw = 0.0f;
            RECT rb{};
            bool has_ink = gv->font->MeasureTightBounds(cps, glyphs, lw, rb);
            gv->lines.push_back(std::move(cps));
            gv->line_widths.push_back(lw);

            if (has_ink) {
                rb.top -= 1;
                rb.bottom += 1;
            } else {
                rb.left = 0;
                rb.top = -std::max(1, static_cast<int>(std::ceil(gv->font->ascent)));
                rb.right = std::max(1, static_cast<int>(std::ceil(lw)));
                rb.bottom = std::max(1, static_cast<int>(std::ceil(gv->font->descent)));
            }
            gv->tight_bounds = rb;
            float bw = static_cast<float>(std::max<LONG>(1, rb.right - rb.left));
            float bh = static_cast<float>(std::max<LONG>(1, rb.bottom - rb.top));
            gv->max_line_w = bw;
            gv->total_h = bh;

            j.SetInstanceField(self, "offsetX", JniVal::Float(static_cast<float>(-rb.left)));
            j.SetInstanceField(self, "offsetY", JniVal::Float(static_cast<float>(-rb.bottom)));
            j.SetInstanceField(self, "boundsW", JniVal::Float(bw));
            j.SetInstanceField(self, "boundsH", JniVal::Float(bh));
            j.SetInstanceField(self, "pixelBoundsW", JniVal::Float(bw));
            j.SetInstanceField(self, "pixelBoundsH", JniVal::Float(bh));
            j.SetInstanceField(self, "pixelTopOffset", JniVal::Float(0.0f));
            j.SetInstanceField(self, "numLines", JniVal::Int(1));
            return JniVal::Void();
        };

        // Exact GlyphVector.java:81-286 contract: wrapped paragraph with tight pixelBounds + pixelTopOffset.
        d.methods["initWithParagraph"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (a.size() < 6) return JniVal::Void();
            auto obj = j.GetJniObject(self);
            auto fobj = j.GetJniObject(a[0].AsObj());
            if (!obj || !fobj || !fobj->native_state.has_value()) return JniVal::Void();
            if (!obj->native_state.has_value()) obj->native_state = std::make_shared<GlyphVectorState>();
            auto gv = std::any_cast<std::shared_ptr<GlyphVectorState>>(obj->native_state);
            gv->font = std::any_cast<std::shared_ptr<FontState>>(fobj->native_state);
            gv->lines.clear();
            gv->line_widths.clear();
            gv->align = a[5].AsInt();
            gv->is_paragraph = true;
            gv->pixel_top_offset = 0.0f;

            std::string text = j.GetStringUtf(a[1].AsObj());
            float max_w = a[2].AsFloat();
            float max_h = a[3].AsFloat();
            int break_mode = a[4].AsInt();

            auto all_cps = DecodeUtf8(text);
            std::vector<ResolvedGlyph> tmp_g;

            std::vector<uint32_t> cur_line;
            auto flush_line = [&]() {
                float w = gv->font->MeasureLine(cur_line, tmp_g);
                gv->lines.push_back(cur_line);
                gv->line_widths.push_back(w);
                cur_line.clear();
            };

            size_t pos = 0;
            while (pos < all_cps.size()) {
                if (all_cps[pos] == '\n') {
                    flush_line();
                    ++pos;
                    continue;
                }
                if (max_w <= 0.0f) {
                    cur_line.push_back(all_cps[pos++]);
                } else {
                    size_t wend = pos;
                    while (wend < all_cps.size() && all_cps[wend] != ' ' && all_cps[wend] != '\n') ++wend;
                    while (wend < all_cps.size() && all_cps[wend] == ' ') ++wend;
                    std::vector<uint32_t> cand = cur_line;
                    cand.insert(cand.end(), all_cps.begin() + pos, all_cps.begin() + wend);
                    float cw = gv->font->MeasureLine(cand, tmp_g);
                    if (cw > max_w && !cur_line.empty()) {
                        flush_line();
                    }
                    cur_line.insert(cur_line.end(), all_cps.begin() + pos, all_cps.begin() + wend);
                    pos = wend;
                }
            }
            if (!cur_line.empty() || gv->lines.empty()) {
                flush_line();
            }

            // Android StaticLayout(includepad=false, spacingadd=0) uses ascent + descent (no extra lineGap)
            // and only truncates lines to max_h for fmParagraphClip (2) / fmParagraphWordWrapTruncate (6) (GlyphVector.java:166,186).
            float line_h = std::max(1.0f, gv->font->height);
            if ((break_mode == 2 || break_mode == 6) && max_h > 0.0f && gv->lines.size() > 1) {
                size_t max_lines = std::max<size_t>(1, static_cast<size_t>(std::floor(max_h / line_h)));
                if (gv->lines.size() > max_lines) {
                    gv->lines.resize(max_lines);
                    gv->line_widths.resize(max_lines);
                }
            }

            gv->max_line_w = 1.0f;
            for (float lw : gv->line_widths) gv->max_line_w = std::max(gv->max_line_w, lw);
            if (max_w > 0.0f) gv->max_line_w = std::min(gv->max_line_w, max_w);
            gv->total_h = std::max(1.0f, line_h * static_cast<float>(gv->lines.size()));

            // Compute tight pixel ink union + line-0 pixelTopOffset (GlyphVector.java:237-286)
            int ink_l = 10000000, ink_r = -10000000, ink_t = 10000000, ink_b = -10000000;
            bool any_ink = false;
            float last_bottom = 0.0f;
            for (size_t li = 0; li < gv->lines.size(); ++li) {
                if (gv->lines[li].empty()) continue;
                std::vector<ResolvedGlyph> glyphs;
                float lw_unused = 0.0f;
                RECT rb{};
                if (gv->font->MeasureTightBounds(gv->lines[li], glyphs, lw_unused, rb)) {
                    any_ink = true;
                    float line_top = static_cast<float>(li) * line_h;
                    float baseline = line_top + gv->font->ascent;
                    if (li == 0) {
                        gv->pixel_top_offset = (static_cast<float>(rb.top) + baseline) - line_top;
                    }
                    if (li + 1 == gv->lines.size()) {
                        last_bottom = std::max(0.0f, static_cast<float>(rb.bottom));
                    }
                    int lt = static_cast<int>(std::floor(static_cast<float>(rb.top) + baseline));
                    int lb = static_cast<int>(std::ceil(static_cast<float>(rb.bottom) + baseline));
                    ink_l = std::min(ink_l, static_cast<int>(rb.left));
                    ink_r = std::max(ink_r, static_cast<int>(rb.right));
                    ink_t = std::min(ink_t, lt);
                    ink_b = std::max(ink_b, lb);
                }
            }

            float bw = std::ceil(gv->max_line_w);
            float bh = std::ceil(gv->total_h);
            float pbw = any_ink ? static_cast<float>(std::max(1, ink_r - ink_l)) : bw;
            float pbh = any_ink ? static_cast<float>(std::max(1, ink_b - ink_t)) : bh;
            gv->tight_bounds = any_ink ? RECT{ink_l, ink_t, ink_r, ink_b}
                                       : RECT{0, 0, static_cast<LONG>(bw), static_cast<LONG>(bh)};

            j.SetInstanceField(self, "offsetX", JniVal::Float(0.0f));
            j.SetInstanceField(self, "offsetY", JniVal::Float(last_bottom));
            j.SetInstanceField(self, "boundsW", JniVal::Float(bw));
            j.SetInstanceField(self, "boundsH", JniVal::Float(bh));
            j.SetInstanceField(self, "pixelBoundsW", JniVal::Float(pbw));
            j.SetInstanceField(self, "pixelBoundsH", JniVal::Float(pbh));
            j.SetInstanceField(self, "pixelTopOffset", JniVal::Float(gv->pixel_top_offset));
            j.SetInstanceField(self, "numLines", JniVal::Int(static_cast<int32_t>(gv->lines.size())));
            return JniVal::Void();
        };

        d.methods["createTexture"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value()) return JniVal::Bool(false);
            auto gv = std::any_cast<std::shared_ptr<GlyphVectorState>>(obj->native_state);
            if (!gv->font || !gv->font->face) return JniVal::Bool(false);

            int bw = std::max(1, static_cast<int>(std::ceil(gv->max_line_w)));
            int bh = std::max(1, static_cast<int>(std::ceil(gv->total_h)));
            int tw = NextPOT(bw);
            int th = NextPOT(bh);

            std::vector<uint8_t> rgba(static_cast<size_t>(tw) * th * 4, 0);
            std::vector<ResolvedGlyph> glyphs;

            auto blit_alpha = [&](const RECT& rb, const std::vector<uint8_t>& alpha) {
                int rw = rb.right - rb.left;
                int rh = rb.bottom - rb.top;
                for (int y = 0; y < rh; ++y) {
                    int dy = rb.top + y;
                    if (dy < 0 || dy >= th) continue;
                    for (int x = 0; x < rw; ++x) {
                        int dx = rb.left + x;
                        if (dx < 0 || dx >= tw) continue;
                        uint8_t a = alpha[static_cast<size_t>(y) * rw + x];
                        if (a) {
                            size_t idx = (static_cast<size_t>(dy) * tw + dx) * 4;
                            rgba[idx + 0] = 255;
                            rgba[idx + 1] = 255;
                            rgba[idx + 2] = 255;
                            rgba[idx + 3] = std::max(rgba[idx + 3], a);
                        }
                    }
                }
            };

            if (!gv->is_paragraph) {
                // Single-line: draw at (-m_bounds.left, -m_bounds.top) matching GlyphVector.java:338
                if (!gv->lines.empty() && !gv->lines[0].empty()) {
                    gv->font->MeasureLine(gv->lines[0], glyphs);
                    float ox = static_cast<float>(-gv->tight_bounds.left);
                    float oy = static_cast<float>(-gv->tight_bounds.top);
                    RECT rb{};
                    std::vector<uint8_t> alpha;
                    if (gv->font->RasterizeRun(glyphs, ox, oy, rb, alpha)) {
                        blit_alpha(rb, alpha);
                    }
                }
            } else {
                // Paragraph: shift Y by -pixelTopOffset matching GlyphVector.java:340
                float line_h = std::max(1.0f, gv->font->height);
                for (size_t li = 0; li < gv->lines.size(); ++li) {
                    if (gv->lines[li].empty()) continue;
                    float lw = gv->font->MeasureLine(gv->lines[li], glyphs);
                    float ox = 0.0f;
                    if (gv->align == 2) ox = std::max(0.0f, (bw - lw) * 0.5f);
                    else if (gv->align == 1) ox = std::max(0.0f, bw - lw);
                    float oy = static_cast<float>(li) * line_h + gv->font->ascent - gv->pixel_top_offset;
                    RECT rb{};
                    std::vector<uint8_t> alpha;
                    if (gv->font->RasterizeRun(glyphs, ox, oy, rb, alpha)) {
                        blit_alpha(rb, alpha);
                    }
                }
            }

            GLint prev_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));

            j.SetInstanceField(self, "texId", JniVal::Int(static_cast<int32_t>(tex)));
            j.SetInstanceField(self, "texWidth", JniVal::Int(tw));
            j.SetInstanceField(self, "texHeight", JniVal::Int(th));
            return JniVal::Bool(true);
        };

        // Exact GlyphVector.java:367-387 contract: respects scale parameter f = a[3].AsFloat()!
        d.methods["renderToTexture"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            auto obj = j.GetJniObject(self);
            if (!obj || !obj->native_state.has_value() || a.size() < 4) return JniVal::Bool(false);
            auto gv = std::any_cast<std::shared_ptr<GlyphVectorState>>(obj->native_state);
            if (!gv->font || !gv->font->face || gv->lines.empty()) return JniVal::Bool(false);

            GLuint tex = static_cast<GLuint>(a[0].AsInt());
            int w = std::max(1, a[1].AsInt());
            int h = std::max(1, a[2].AsInt());
            float f = a[3].AsFloat();
            if (!(f > 0.0f)) f = 1.0f;
            std::vector<uint8_t> lum(static_cast<size_t>(w) * h, 0);

            std::vector<ResolvedGlyph> glyphs;
            gv->font->MeasureLine(gv->lines[0], glyphs);
            float bw = static_cast<float>(std::max<LONG>(1, gv->tight_bounds.right - gv->tight_bounds.left));
            float bh = static_cast<float>(std::max<LONG>(1, gv->tight_bounds.bottom - gv->tight_bounds.top));
            float ox = (static_cast<float>(w) - bw * f) * 0.5f - static_cast<float>(gv->tight_bounds.left) * f;
            float oy = (static_cast<float>(h) - bh * f) * 0.5f - static_cast<float>(gv->tight_bounds.top) * f;
            RECT rb{};
            std::vector<uint8_t> alpha;
            if (gv->font->RasterizeRun(glyphs, ox, oy, rb, alpha, f)) {
                int rw = rb.right - rb.left;
                int rh = rb.bottom - rb.top;
                for (int y = 0; y < rh; ++y) {
                    int dy = h - 1 - (rb.top + y); // flip Y per GlyphVector.java:375
                    if (dy < 0 || dy >= h) continue;
                    for (int x = 0; x < rw; ++x) {
                        int dx = rb.left + x;
                        if (dx < 0 || dx >= w) continue;
                        lum[static_cast<size_t>(dy) * w + dx] = alpha[static_cast<size_t>(y) * rw + x];
                    }
                }
            }
            GLint prev_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum.data());
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
            return JniVal::Bool(true);
        };
        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- CarLiveryBaker (rg_etc1 compression returning DirectByteBuffer) ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/CarLiveryBaker";
        d.static_methods["compressTextureToETC1"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (a.size() < 5) return JniVal::Obj(0);
            auto buf_obj = j.GetJniObject(a[0].AsObj());
            int w = a[1].AsInt(), h = a[2].AsInt(), pixel_sz = a[3].AsInt(), stride = a[4].AsInt();
            if (!buf_obj || w <= 0 || h <= 0) return JniVal::Obj(0);
            const auto* src = static_cast<const uint8_t*>(
                buf_obj->external_direct_ptr ? buf_obj->external_direct_ptr : buf_obj->bytes.data());
            if (!src) return JniVal::Obj(0);
            if (stride <= 0) stride = w * std::max(3, pixel_sz);

            int bw = (w + 3) / 4;
            int bh = (h + 3) / 4;
            std::vector<uint8_t> etc_out(static_cast<size_t>(bw) * bh * 8, 0);
            rg_etc1::etc1_pack_params params;
            params.m_quality = rg_etc1::cLowQuality; // fast interactive livery baking
            params.m_dithering = false;

            uint32_t block[16]{};
            for (int by = 0; by < bh; ++by) {
                for (int bx = 0; bx < bw; ++bx) {
                    for (int py = 0; py < 4; ++py) {
                        int sy = std::min(h - 1, by * 4 + py);
                        for (int px = 0; px < 4; ++px) {
                            int sx = std::min(w - 1, bx * 4 + px);
                            const uint8_t* p = src + static_cast<size_t>(sy) * stride + sx * pixel_sz;
                            uint8_t r = p[0];
                            uint8_t g = pixel_sz >= 2 ? p[1] : r;
                            uint8_t b = pixel_sz >= 3 ? p[2] : r;
                            block[py * 4 + px] = static_cast<uint32_t>(r) |
                                                 (static_cast<uint32_t>(g) << 8) |
                                                 (static_cast<uint32_t>(b) << 16) |
                                                 0xFF000000u;
                        }
                    }
                    rg_etc1::pack_etc1_block(etc_out.data() + (static_cast<size_t>(by) * bw + bx) * 8, block, params);
                }
            }
            return JniVal::Obj(j.NewDirectByteBufferOwned(std::move(etc_out)));
        };
        jvm_.RegisterJniClass(std::move(d));
    }

    // ---- ControllerManager / AudioStreamManager / Input / MoviePlayer / TextField ----
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/ControllerManager";
        d.static_methods["isControllerConnected"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        jvm_.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/AudioStreamManager";
        d.static_methods["staticIsUserMusicPlaying"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["staticRequestMusicFocus"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["isUserMusicPlaying"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["requestAudioFocus"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["abandonAudioFocus"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm_.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/Input";
        d.static_methods["isAccelerometerAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isGyroscopeAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["enableAccelerometer"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["enableGyroscope"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["isAccelerometerAvailable"] = d.static_methods["isAccelerometerAvailable"];
        d.methods["isGyroscopeAvailable"] = d.static_methods["isGyroscopeAvailable"];
        d.methods["enableAccelerometer"] = d.static_methods["enableAccelerometer"];
        d.methods["enableGyroscope"] = d.static_methods["enableGyroscope"];
        jvm_.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/MoviePlayer";
        d.methods["play"] = [this](FakeJvm& j, uint64_t self, const auto& a) {
            std::string url = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "";
            int64_t that = a.size() > 1 ? a[1].AsLong() : 0;
            std::fprintf(stdout, "[RR3] MoviePlayer.play('%s') -> skipping to OnCompletionNative\n", url.c_str());
            uint64_t fn = loader_.FindExport("Java_com_firemint_realracing_MoviePlayer_OnCompletionNative");
            if (fn && that) {
                j.PinHandle(self);
                PostGlCallback([this, fn, self, that]() {
                    cpu_.CallGuest(fn, {jvm_.GetJniEnvGuestPtr(), self, static_cast<uint64_t>(that)});
                });
            }
            return JniVal::Void();
        };
        d.methods["stop"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["pause"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["resume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm_.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemint/realracing/TextField";
        d.methods["show"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["hide"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm_.RegisterJniClass(std::move(d));
    }
}
