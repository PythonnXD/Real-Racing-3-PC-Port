#include "gles_bridge.h"

#include <windows.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "rg_etc1.h"

namespace {

constexpr GLenum kGlTextureMaxAnisotropyExt = 0x84FE;
constexpr GLenum kGlMaxTextureMaxAnisotropyExt = 0x84FF;
constexpr GLenum kGlRgbS3tcDxt1Ext = 0x83F0;
constexpr GLenum kGlRgbaS3tcDxt1Ext = 0x83F1;
constexpr GLenum kGlRgbaS3tcDxt3Ext = 0x83F2;
constexpr GLenum kGlRgbaS3tcDxt5Ext = 0x83F3;
constexpr GLenum kGlRgbPvrtc4BppImg = 0x8C00;
constexpr GLenum kGlRgbPvrtc2BppImg = 0x8C01;
constexpr GLenum kGlRgbaPvrtc4BppImg = 0x8C02;
constexpr GLenum kGlRgbaPvrtc2BppImg = 0x8C03;
constexpr GLenum kGlAtcRgbAmd = 0x8C92;
constexpr GLenum kGlAtcRgbaExplicitAmd = 0x8C93;
constexpr GLenum kGlAtcRgbaInterpolatedAmd = 0x87EE;
constexpr GLenum kGlEtc1Rgb8Oes = 0x8D64;

inline bool IsAtcFormat(GLenum fmt) {
    return fmt == kGlAtcRgbAmd || fmt == kGlAtcRgbaExplicitAmd || fmt == kGlAtcRgbaInterpolatedAmd;
}
inline bool IsDxtFormat(GLenum fmt) {
    return fmt == kGlRgbS3tcDxt1Ext || fmt == kGlRgbaS3tcDxt1Ext ||
           fmt == kGlRgbaS3tcDxt3Ext || fmt == kGlRgbaS3tcDxt5Ext;
}
inline bool IsPvrtcFormat(GLenum fmt) {
    return fmt == kGlRgbPvrtc4BppImg || fmt == kGlRgbPvrtc2BppImg ||
           fmt == kGlRgbaPvrtc4BppImg || fmt == kGlRgbaPvrtc2BppImg;
}
inline uint32_t ReadU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint8_t Expand5To8(int v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
inline uint8_t Expand6To8(int v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
inline uint8_t Expand4To5(int v) { return static_cast<uint8_t>((v << 1) | (v >> 3)); }

inline uint8_t ClampU8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void DecodeDxtColorBlock(const uint8_t* blk, bool four_color_only, bool punch_through_alpha, uint8_t out_rgba[16][4]) {
    const uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    const uint32_t idx = ReadU32Le(blk + 4);

    const int r0 = Expand5To8((c0 >> 11) & 0x1F);
    const int g0 = Expand6To8((c0 >> 5) & 0x3F);
    const int b0 = Expand5To8(c0 & 0x1F);
    const int r1 = Expand5To8((c1 >> 11) & 0x1F);
    const int g1 = Expand6To8((c1 >> 5) & 0x3F);
    const int b1 = Expand5To8(c1 & 0x1F);

    uint8_t pal[4][4];
    pal[0][0] = static_cast<uint8_t>(r0); pal[0][1] = static_cast<uint8_t>(g0); pal[0][2] = static_cast<uint8_t>(b0); pal[0][3] = 255;
    pal[1][0] = static_cast<uint8_t>(r1); pal[1][1] = static_cast<uint8_t>(g1); pal[1][2] = static_cast<uint8_t>(b1); pal[1][3] = 255;
    if (four_color_only || c0 > c1) {
        pal[2][0] = static_cast<uint8_t>((2 * r0 + r1 + 1) / 3);
        pal[2][1] = static_cast<uint8_t>((2 * g0 + g1 + 1) / 3);
        pal[2][2] = static_cast<uint8_t>((2 * b0 + b1 + 1) / 3);
        pal[2][3] = 255;
        pal[3][0] = static_cast<uint8_t>((r0 + 2 * r1 + 1) / 3);
        pal[3][1] = static_cast<uint8_t>((g0 + 2 * g1 + 1) / 3);
        pal[3][2] = static_cast<uint8_t>((b0 + 2 * b1 + 1) / 3);
        pal[3][3] = 255;
    } else {
        pal[2][0] = static_cast<uint8_t>((r0 + r1) / 2);
        pal[2][1] = static_cast<uint8_t>((g0 + g1) / 2);
        pal[2][2] = static_cast<uint8_t>((b0 + b1) / 2);
        pal[2][3] = 255;
        pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0;
        pal[3][3] = punch_through_alpha ? 0 : 255;
    }

    for (int i = 0; i < 16; ++i) {
        const uint32_t sel = (idx >> (2 * i)) & 3u;
        out_rgba[i][0] = pal[sel][0];
        out_rgba[i][1] = pal[sel][1];
        out_rgba[i][2] = pal[sel][2];
        out_rgba[i][3] = pal[sel][3];
    }
}

struct PvrtcColor {
    uint8_t r, g, b, a;
};

uint32_t PvrtcTwiddleUV(uint32_t x_size, uint32_t y_size, uint32_t x_pos, uint32_t y_pos) {
    uint32_t min_dim = x_size;
    uint32_t max_val = y_pos;
    if (y_size < x_size) {
        min_dim = y_size;
        max_val = x_pos;
    }
    uint32_t twiddled = 0;
    uint32_t src_bit = 1;
    uint32_t dst_bit = 1;
    uint32_t shift_count = 0;
    while (src_bit < min_dim) {
        if (y_pos & src_bit) twiddled |= dst_bit;
        if (x_pos & src_bit) twiddled |= (dst_bit << 1);
        src_bit <<= 1;
        dst_bit <<= 2;
        ++shift_count;
    }
    max_val >>= shift_count;
    twiddled |= (max_val << (2u * shift_count));
    return twiddled;
}

PvrtcColor PvrtcUnpackColorA(uint32_t cd) {
    if (cd & 0x8000u) {
        int r5 = static_cast<int>((cd >> 10) & 0x1Fu);
        int g5 = static_cast<int>((cd >> 5) & 0x1Fu);
        int b4 = static_cast<int>((cd >> 1) & 0x0Fu);
        int b5 = Expand4To5(b4);
        return {Expand5To8(r5), Expand5To8(g5), Expand5To8(b5), 255};
    }
    int a3 = static_cast<int>((cd >> 12) & 0x07u);
    int r4 = static_cast<int>((cd >> 8) & 0x0Fu);
    int g4 = static_cast<int>((cd >> 4) & 0x0Fu);
    int b3 = static_cast<int>((cd >> 1) & 0x07u);
    int a4 = a3 << 1;
    int b5 = (b3 << 2) | (b3 >> 1);
    return {
        Expand5To8(Expand4To5(r4)),
        Expand5To8(Expand4To5(g4)),
        Expand5To8(b5),
        static_cast<uint8_t>((a4 << 4) | a4)
    };
}

PvrtcColor PvrtcUnpackColorB(uint32_t cd) {
    if (cd & 0x80000000u) {
        int r5 = static_cast<int>((cd >> 26) & 0x1Fu);
        int g5 = static_cast<int>((cd >> 21) & 0x1Fu);
        int b5 = static_cast<int>((cd >> 16) & 0x1Fu);
        return {Expand5To8(r5), Expand5To8(g5), Expand5To8(b5), 255};
    }
    int a3 = static_cast<int>((cd >> 28) & 0x07u);
    int r4 = static_cast<int>((cd >> 24) & 0x0Fu);
    int g4 = static_cast<int>((cd >> 20) & 0x0Fu);
    int b4 = static_cast<int>((cd >> 16) & 0x0Fu);
    int a4 = a3 << 1;
    return {
        Expand5To8(Expand4To5(r4)),
        Expand5To8(Expand4To5(g4)),
        Expand5To8(Expand4To5(b4)),
        static_cast<uint8_t>((a4 << 4) | a4)
    };
}

bool DecodePvrtc1ToRgba(GLenum fmt, int w, int h, const uint8_t* src, int image_size, std::vector<uint8_t>& out_rgba) {
    if (!src || w <= 0 || h <= 0) return false;
    const bool is_2bpp = (fmt == kGlRgbPvrtc2BppImg || fmt == kGlRgbaPvrtc2BppImg);
    const bool force_opaque = (fmt == kGlRgbPvrtc4BppImg || fmt == kGlRgbPvrtc2BppImg);
    const int blk_w = is_2bpp ? 8 : 4;
    const int blk_h = 4;
    const int min_w = is_2bpp ? 16 : 8;
    const int min_h = 8;

    const int eff_w = std::max(w, min_w);
    const int eff_h = std::max(h, min_h);
    const int num_x = std::max(2, (eff_w + blk_w - 1) / blk_w);
    const int num_y = std::max(2, (eff_h + blk_h - 1) / blk_h);
    const size_t total_blocks = static_cast<size_t>(num_x) * static_cast<size_t>(num_y);
    if (image_size < static_cast<int>(total_blocks * 8)) return false;

    const int grid_w = num_x * blk_w;
    const int grid_h = num_y * blk_h;

    std::vector<PvrtcColor> col_a(total_blocks);
    std::vector<PvrtcColor> col_b(total_blocks);
    std::vector<uint8_t> mod_w(static_cast<size_t>(grid_w) * grid_h, 0);
    std::vector<uint8_t> mod_flag(static_cast<size_t>(grid_w) * grid_h, 0);
    // mod_flag: 0=direct, 1=punch-through(alpha=0), 2=avg4(2bpp checker), 3=avgH(2bpp), 4=avgV(2bpp)

    static constexpr uint8_t kStd4[4] = {0, 3, 5, 8};
    static constexpr uint8_t kPunch4[4] = {0, 4, 4, 8};

    for (int by = 0; by < num_y; ++by) {
        for (int bx = 0; bx < num_x; ++bx) {
            const uint32_t tw = PvrtcTwiddleUV(static_cast<uint32_t>(num_x), static_cast<uint32_t>(num_y),
                                               static_cast<uint32_t>(bx), static_cast<uint32_t>(by));
            if (tw >= total_blocks) return false;
            const uint8_t* pkt = src + static_cast<size_t>(tw) * 8;
            const uint32_t mod_bits = ReadU32Le(pkt);
            const uint32_t col_bits = ReadU32Le(pkt + 4);
            const size_t bidx = static_cast<size_t>(by) * num_x + bx;
            col_a[bidx] = PvrtcUnpackColorA(col_bits);
            col_b[bidx] = PvrtcUnpackColorB(col_bits);
            const uint32_t mode = col_bits & 1u;

            if (!is_2bpp) {
                uint32_t mb = mod_bits;
                for (int py = 0; py < 4; ++py) {
                    const size_t row_off = static_cast<size_t>(by * 4 + py) * grid_w + bx * 4;
                    for (int px = 0; px < 4; ++px) {
                        const uint32_t code = mb & 3u;
                        mb >>= 2;
                        if (mode == 0) {
                            mod_w[row_off + px] = kStd4[code];
                        } else {
                            mod_w[row_off + px] = kPunch4[code];
                            if (code == 2) mod_flag[row_off + px] = 1;
                        }
                    }
                }
            } else if (mode == 0) {
                uint32_t mb = mod_bits;
                for (int py = 0; py < 4; ++py) {
                    const size_t row_off = static_cast<size_t>(by * 4 + py) * grid_w + bx * 8;
                    for (int px = 0; px < 8; ++px) {
                        mod_w[row_off + px] = (mb & 1u) ? 8 : 0;
                        mb >>= 1;
                    }
                }
            } else {
                const bool hv_mode = (mod_bits & 1u) != 0;
                const bool vert_mode = (mod_bits & (1u << 20)) != 0;
                const uint8_t odd_flag = hv_mode ? (vert_mode ? 4 : 3) : 2;
                uint32_t mb = mod_bits;
                for (int py = 0; py < 4; ++py) {
                    const size_t row_off = static_cast<size_t>(by * 4 + py) * grid_w + bx * 8;
                    for (int px = 0; px < 8; ++px) {
                        if (((px ^ py) & 1) == 0) {
                            const uint32_t code = mb & 3u;
                            mb >>= 2;
                            if (px == 0 && py == 0) {
                                mod_w[row_off + px] = (code & 2u) ? 8 : 0;
                            } else if (hv_mode && px == 4 && py == 2) {
                                mod_w[row_off + px] = (code & 2u) ? 8 : 0;
                            } else {
                                mod_w[row_off + px] = kStd4[code];
                            }
                        } else {
                            mod_flag[row_off + px] = odd_flag;
                        }
                    }
                }
            }
        }
    }

    auto floor_div = [](int a, int b) -> int {
        int q = a / b;
        int r = a % b;
        if (r < 0) --q;
        return q;
    };
    auto wrap = [](int v, int m) -> int {
        int r = v % m;
        return r < 0 ? r + m : r;
    };

    const int half_w = blk_w / 2;
    const int half_h = blk_h / 2;
    const int area_shift = is_2bpp ? 5 : 4;

    out_rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const int y_adj = y - half_h;
        const int by0 = wrap(floor_div(y_adj, blk_h), num_y);
        const int by1 = (by0 + 1) % num_y;
        const int wy = y_adj - floor_div(y_adj, blk_h) * blk_h;
        const int inv_wy = blk_h - wy;

        for (int x = 0; x < w; ++x) {
            const int x_adj = x - half_w;
            const int bx0 = wrap(floor_div(x_adj, blk_w), num_x);
            const int bx1 = (bx0 + 1) % num_x;
            const int wx = x_adj - floor_div(x_adj, blk_w) * blk_w;
            const int inv_wx = blk_w - wx;

            const int w00 = inv_wx * inv_wy;
            const int w10 = wx * inv_wy;
            const int w01 = inv_wx * wy;
            const int w11 = wx * wy;

            const size_t i00 = static_cast<size_t>(by0) * num_x + bx0;
            const size_t i10 = static_cast<size_t>(by0) * num_x + bx1;
            const size_t i01 = static_cast<size_t>(by1) * num_x + bx0;
            const size_t i11 = static_cast<size_t>(by1) * num_x + bx1;

            auto interp = [&](const std::vector<PvrtcColor>& tbl, int& r, int& g, int& b, int& a) {
                r = (tbl[i00].r * w00 + tbl[i10].r * w10 + tbl[i01].r * w01 + tbl[i11].r * w11) >> area_shift;
                g = (tbl[i00].g * w00 + tbl[i10].g * w10 + tbl[i01].g * w01 + tbl[i11].g * w11) >> area_shift;
                b = (tbl[i00].b * w00 + tbl[i10].b * w10 + tbl[i01].b * w01 + tbl[i11].b * w11) >> area_shift;
                a = (tbl[i00].a * w00 + tbl[i10].a * w10 + tbl[i01].a * w01 + tbl[i11].a * w11) >> area_shift;
            };

            int ar, ag, ab, aa, br, bg, bb, ba;
            interp(col_a, ar, ag, ab, aa);
            interp(col_b, br, bg, bb, ba);

            const size_t pidx = static_cast<size_t>(y) * grid_w + x;
            const uint8_t fl = mod_flag[pidx];
            int mw = mod_w[pidx];
            if (fl >= 2) {
                const int xl = wrap(x - 1, grid_w);
                const int xr = wrap(x + 1, grid_w);
                const int yu = wrap(y - 1, grid_h);
                const int yd = wrap(y + 1, grid_h);
                const int wl = mod_w[static_cast<size_t>(y) * grid_w + xl];
                const int wr = mod_w[static_cast<size_t>(y) * grid_w + xr];
                const int wu = mod_w[static_cast<size_t>(yu) * grid_w + x];
                const int wd = mod_w[static_cast<size_t>(yd) * grid_w + x];
                if (fl == 2)      mw = (wl + wr + wu + wd + 2) / 4;
                else if (fl == 3) mw = (wl + wr + 1) / 2;
                else              mw = (wu + wd + 1) / 2;
            }

            const int inv_m = 8 - mw;
            uint8_t* dst = out_rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            dst[0] = static_cast<uint8_t>((ar * inv_m + br * mw) / 8);
            dst[1] = static_cast<uint8_t>((ag * inv_m + bg * mw) / 8);
            dst[2] = static_cast<uint8_t>((ab * inv_m + bb * mw) / 8);
            if (force_opaque) {
                dst[3] = 255;
            } else if (fl == 1) {
                dst[3] = 0;
            } else {
                dst[3] = static_cast<uint8_t>((aa * inv_m + ba * mw) / 8);
            }
        }
    }
    return true;
}

void DecodeAtcColorBlock(const uint8_t* blk, uint8_t out_rgb[16][3]) {
    uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    uint32_t idx = static_cast<uint32_t>(blk[4]) | (static_cast<uint32_t>(blk[5]) << 8) |
                   (static_cast<uint32_t>(blk[6]) << 16) | (static_cast<uint32_t>(blk[7]) << 24);

    int r0 = ((c0 >> 10) & 0x1F); r0 = (r0 << 3) | (r0 >> 2);
    int g0 = ((c0 >> 5)  & 0x1F); g0 = (g0 << 3) | (g0 >> 2);
    int b0 = (c0 & 0x1F);         b0 = (b0 << 3) | (b0 >> 2);

    int r1 = ((c1 >> 11) & 0x1F); r1 = (r1 << 3) | (r1 >> 2);
    int g1 = ((c1 >> 5)  & 0x3F); g1 = (g1 << 2) | (g1 >> 4);
    int b1 = (c1 & 0x1F);         b1 = (b1 << 3) | (b1 >> 2);

    uint8_t pal[4][3];
    if ((c0 & 0x8000u) == 0) {
        pal[0][0] = static_cast<uint8_t>(r0); pal[0][1] = static_cast<uint8_t>(g0); pal[0][2] = static_cast<uint8_t>(b0);
        pal[3][0] = static_cast<uint8_t>(r1); pal[3][1] = static_cast<uint8_t>(g1); pal[3][2] = static_cast<uint8_t>(b1);
        pal[1][0] = static_cast<uint8_t>((5 * r0 + 3 * r1) / 8);
        pal[1][1] = static_cast<uint8_t>((5 * g0 + 3 * g1) / 8);
        pal[1][2] = static_cast<uint8_t>((5 * b0 + 3 * b1) / 8);
        pal[2][0] = static_cast<uint8_t>((3 * r0 + 5 * r1) / 8);
        pal[2][1] = static_cast<uint8_t>((3 * g0 + 5 * g1) / 8);
        pal[2][2] = static_cast<uint8_t>((3 * b0 + 5 * b1) / 8);
    } else {
        pal[0][0] = 0; pal[0][1] = 0; pal[0][2] = 0;
        pal[1][0] = ClampU8(r0 - (r1 >> 2));
        pal[1][1] = ClampU8(g0 - (g1 >> 2));
        pal[1][2] = ClampU8(b0 - (b1 >> 2));
        pal[2][0] = static_cast<uint8_t>(r0); pal[2][1] = static_cast<uint8_t>(g0); pal[2][2] = static_cast<uint8_t>(b0);
        pal[3][0] = static_cast<uint8_t>(r1); pal[3][1] = static_cast<uint8_t>(g1); pal[3][2] = static_cast<uint8_t>(b1);
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t sel = (idx >> (2 * i)) & 3u;
        out_rgb[i][0] = pal[sel][0];
        out_rgb[i][1] = pal[sel][1];
        out_rgb[i][2] = pal[sel][2];
    }
}

void DecodeAtcExplicitAlpha(const uint8_t* blk, uint8_t out_a[16]) {
    uint64_t bits = 0;
    std::memcpy(&bits, blk, 8);
    for (int i = 0; i < 16; ++i) {
        uint8_t a4 = static_cast<uint8_t>((bits >> (4 * i)) & 0xFu);
        out_a[i] = static_cast<uint8_t>((a4 << 4) | a4);
    }
}

void DecodeAtcInterpolatedAlpha(const uint8_t* blk, uint8_t out_a[16]) {
    uint8_t a[8];
    a[0] = blk[0];
    a[1] = blk[1];
    if (a[0] > a[1]) {
        for (int k = 1; k <= 6; ++k) a[k + 1] = static_cast<uint8_t>(((7 - k) * a[0] + k * a[1]) / 7);
    } else {
        for (int k = 1; k <= 4; ++k) a[k + 1] = static_cast<uint8_t>(((5 - k) * a[0] + k * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int b = 0; b < 6; ++b) bits |= static_cast<uint64_t>(blk[2 + b]) << (8 * b);
    for (int i = 0; i < 16; ++i) {
        out_a[i] = a[(bits >> (3 * i)) & 7u];
    }
}

bool DecodeCompressedToRgba(GLenum fmt, int w, int h, const uint8_t* src, int image_size, std::vector<uint8_t>& out_rgba) {
    if (!src || w <= 0 || h <= 0) return false;
    if (IsPvrtcFormat(fmt)) {
        return DecodePvrtc1ToRgba(fmt, w, h, src, image_size, out_rgba);
    }
    if (!IsAtcFormat(fmt) && !IsDxtFormat(fmt) && fmt != kGlEtc1Rgb8Oes) {
        return false;
    }

    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    const size_t blk_bytes = (fmt == kGlAtcRgbAmd || fmt == kGlEtc1Rgb8Oes ||
                              fmt == kGlRgbS3tcDxt1Ext || fmt == kGlRgbaS3tcDxt1Ext) ? 8 : 16;
    if (image_size < static_cast<int>(static_cast<size_t>(bw) * bh * blk_bytes)) return false;

    out_rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * blk_bytes;
            if (fmt == kGlEtc1Rgb8Oes) {
                uint32_t pixels[16];
                rg_etc1::unpack_etc1_block(blk, pixels, false);
                for (int py = 0; py < 4; ++py) {
                    int y = by * 4 + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; ++px) {
                        int x = bx * 4 + px;
                        if (x >= w) continue;
                        uint32_t c = pixels[py * 4 + px];
                        uint8_t* dst = out_rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
                        dst[0] = static_cast<uint8_t>(c & 0xFF);
                        dst[1] = static_cast<uint8_t>((c >> 8) & 0xFF);
                        dst[2] = static_cast<uint8_t>((c >> 16) & 0xFF);
                        dst[3] = 255;
                    }
                }
            } else if (IsDxtFormat(fmt)) {
                uint8_t rgba[16][4];
                if (fmt == kGlRgbS3tcDxt1Ext || fmt == kGlRgbaS3tcDxt1Ext) {
                    DecodeDxtColorBlock(blk, false, fmt == kGlRgbaS3tcDxt1Ext, rgba);
                } else {
                    uint8_t alpha[16];
                    if (fmt == kGlRgbaS3tcDxt3Ext) {
                        DecodeAtcExplicitAlpha(blk, alpha);
                    } else {
                        DecodeAtcInterpolatedAlpha(blk, alpha);
                    }
                    DecodeDxtColorBlock(blk + 8, true, false, rgba);
                    for (int i = 0; i < 16; ++i) rgba[i][3] = alpha[i];
                }
                for (int py = 0; py < 4; ++py) {
                    int y = by * 4 + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; ++px) {
                        int x = bx * 4 + px;
                        if (x >= w) continue;
                        int pi = py * 4 + px;
                        uint8_t* dst = out_rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
                        dst[0] = rgba[pi][0];
                        dst[1] = rgba[pi][1];
                        dst[2] = rgba[pi][2];
                        dst[3] = rgba[pi][3];
                    }
                }
            } else {
                uint8_t rgb[16][3];
                uint8_t alpha[16];
                if (fmt == kGlAtcRgbAmd) {
                    DecodeAtcColorBlock(blk, rgb);
                    std::memset(alpha, 255, 16);
                } else if (fmt == kGlAtcRgbaExplicitAmd) {
                    DecodeAtcExplicitAlpha(blk, alpha);
                    DecodeAtcColorBlock(blk + 8, rgb);
                } else {
                    DecodeAtcInterpolatedAlpha(blk, alpha);
                    DecodeAtcColorBlock(blk + 8, rgb);
                }
                for (int py = 0; py < 4; ++py) {
                    int y = by * 4 + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; ++px) {
                        int x = bx * 4 + px;
                        if (x >= w) continue;
                        int pi = py * 4 + px;
                        uint8_t* dst = out_rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
                        dst[0] = rgb[pi][0];
                        dst[1] = rgb[pi][1];
                        dst[2] = rgb[pi][2];
                        dst[3] = alpha[pi];
                    }
                }
            }
        }
    }
    return true;
}

template <typename T>
T* Ptr(uint64_t v) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(v));
}

template <typename T>
const T* CPtr(uint64_t v) {
    return reinterpret_cast<const T*>(static_cast<uintptr_t>(v));
}

} // namespace

GlesBridge::GlesBridge(CpuRuntime& cpu) : cpu_(cpu) {}

bool GlesBridge::DecodeCompressedTextureToRgba(uint32_t gl_format, int width, int height,
                                               const uint8_t* src, int image_size,
                                               std::vector<uint8_t>& out_rgba) {
    return DecodeCompressedToRgba(static_cast<GLenum>(gl_format), width, height, src, image_size, out_rgba);
}

GlesBridge::~GlesBridge() {
    Shutdown();
}

bool GlesBridge::Init(HWND hwnd, int width, int height, const PcConfig& cfg, int fixed_render_h) {
    hwnd_ = hwnd;
    cfg_ = cfg;
    window_w_ = std::max(1, width);
    window_h_ = std::max(1, height);
    float scale = std::clamp(cfg_.render_scale, 0.25f, 4.0f);
    if (fixed_render_h > 0) {
        fixed_render_h_ = std::max(360, static_cast<int>(std::lround(fixed_render_h * scale)) & ~1);
        render_h_ = fixed_render_h_;
        double aspect = static_cast<double>(window_w_) / std::max(1, window_h_);
        render_w_ = std::clamp(static_cast<int>(std::lround(render_h_ * aspect)) & ~1,
                               render_h_ + 16, 4096);
    } else {
        fixed_render_h_ = 0;
        render_w_ = std::max(2, static_cast<int>(std::lround(window_w_ * scale)) & ~1);
        render_h_ = std::max(2, static_cast<int>(std::lround(window_h_ * scale)) & ~1);
    }

    hdc_ = GetDC(hwnd_);
    if (!hdc_) {
        std::fprintf(stderr, "[GLES] GetDC failed\n");
        return false;
    }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform_dpy = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_dpy) {
        const EGLint dpy_attribs[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, EGL_DONT_CARE,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, EGL_DONT_CARE,
            EGL_NONE
        };
        dpy = get_platform_dpy(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(hdc_), dpy_attribs);
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(hdc_);
    }
    if (dpy == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "[GLES] eglGetDisplay failed (err=0x%x)\n", eglGetError());
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        std::fprintf(stderr, "[GLES] eglInitialize failed (err=0x%x)\n", eglGetError());
        return false;
    }

    auto choose_cfg = [&](int samples, EGLConfig& out_cfg) -> bool {
        std::vector<EGLint> attrs = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
        };
        if (samples > 1) {
            attrs.push_back(EGL_SAMPLE_BUFFERS);
            attrs.push_back(1);
            attrs.push_back(EGL_SAMPLES);
            attrs.push_back(samples);
        }
        attrs.push_back(EGL_NONE);
        EGLint num_cfg = 0;
        return eglChooseConfig(dpy, attrs.data(), &out_cfg, 1, &num_cfg) && num_cfg > 0;
    };

    EGLConfig egl_cfg = nullptr;
    int req_msaa = (cfg_.msaa_samples >= 2 && fixed_render_h_ == 0 && std::fabs(scale - 1.0f) < 0.01f)
                       ? cfg_.msaa_samples
                       : 0;
    if (!choose_cfg(req_msaa, egl_cfg)) {
        if (!choose_cfg(0, egl_cfg)) {
            std::fprintf(stderr, "[GLES] eglChooseConfig failed (err=0x%x)\n", eglGetError());
            return false;
        }
    }

    const EGLint surf_attribs[] = { EGL_NONE };
    EGLSurface surf = eglCreateWindowSurface(dpy, egl_cfg, hwnd_, surf_attribs);
    if (surf == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[GLES] eglCreateWindowSurface failed (err=0x%x)\n", eglGetError());
        return false;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[GLES] eglCreateContext failed (err=0x%x)\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        std::fprintf(stderr, "[GLES] eglMakeCurrent failed (err=0x%x)\n", eglGetError());
        return false;
    }

    display_ = dpy;
    surface_ = surf;
    context_ = ctx;

    eglSwapInterval(dpy, cfg_.vsync ? 1 : 0);

    auto safe_str = [](const GLubyte* s) { return s ? std::string(reinterpret_cast<const char*>(s)) : std::string(); };
    gl_vendor_ = safe_str(glGetString(GL_VENDOR));
    gl_renderer_ = safe_str(glGetString(GL_RENDERER));
    gl_version_ = safe_str(glGetString(GL_VERSION));
    gl_extensions_ = safe_str(glGetString(GL_EXTENSIONS));

    // Curate guest GL_EXTENSIONS so RR3's IsTextureFormatForThisGpu (priority: PVRTC > ATC > DXT > ETC1)
    // selects the exact compressed texture pack present in the user's .depot!
    host_supports_s3tc_ =
        (gl_extensions_.find("GL_EXT_texture_compression_s3tc") != std::string::npos) ||
        (gl_extensions_.find("GL_EXT_texture_compression_dxt1") != std::string::npos &&
         gl_extensions_.find("GL_ANGLE_texture_compression_dxt3") != std::string::npos &&
         gl_extensions_.find("GL_ANGLE_texture_compression_dxt5") != std::string::npos);
    cpu_decoded_dxt_tex_.clear();
    {
        const bool keep_dxt = (target_gpu_format_ == DepotGpuFormat::Dxt);
        std::istringstream iss(gl_extensions_);
        std::string tok;
        std::string curated;
        while (iss >> tok) {
            if (tok.find("texture_compression_bptc") != std::string::npos ||
                tok.find("texture_compression_rgtc") != std::string::npos ||
                tok.find("texture_compression_astc") != std::string::npos ||
                tok.find("texture_compression_pvrtc") != std::string::npos ||
                tok.find("compressed_ATC") != std::string::npos ||
                tok.find("compressed_ETC") != std::string::npos) {
                continue;
            }
            if (!keep_dxt &&
                (tok.find("texture_compression_dxt") != std::string::npos ||
                 tok.find("texture_compression_s3tc") != std::string::npos)) {
                continue;
            }
            if (!curated.empty()) curated.push_back(' ');
            curated += tok;
        }
        switch (target_gpu_format_) {
            case DepotGpuFormat::Pvrtc:
                curated += " GL_IMG_texture_compression_pvrtc GL_OES_compressed_ETC1_RGB8_texture";
                break;
            case DepotGpuFormat::Atc:
                curated += " GL_AMD_compressed_ATC_texture GL_OES_compressed_ETC1_RGB8_texture";
                break;
            case DepotGpuFormat::Dxt:
                if (curated.find("GL_EXT_texture_compression_s3tc") == std::string::npos) {
                    curated += " GL_EXT_texture_compression_s3tc";
                }
                if (curated.find("GL_EXT_texture_compression_dxt1") == std::string::npos) {
                    curated += " GL_EXT_texture_compression_dxt1";
                }
                if (curated.find("GL_ANGLE_texture_compression_dxt3") == std::string::npos) {
                    curated += " GL_ANGLE_texture_compression_dxt3";
                }
                if (curated.find("GL_ANGLE_texture_compression_dxt5") == std::string::npos) {
                    curated += " GL_ANGLE_texture_compression_dxt5";
                }
                curated += " GL_OES_compressed_ETC1_RGB8_texture";
                break;
            case DepotGpuFormat::Etc:
                // Expose ONLY ETC1 (no PVRTC, ATC, or S3TC/DXT) so RR3 selects Mali .etc.dds.z assets.
                curated += " GL_OES_compressed_ETC1_RGB8_texture";
                break;
        }
        guest_extensions_ = std::move(curated);
    }

    if (gl_extensions_.find("GL_EXT_texture_filter_anisotropic") != std::string::npos) {
        glGetFloatv(kGlMaxTextureMaxAnisotropyExt, &max_anisotropy_);
        if (cfg_.anisotropy > 1) {
            applied_anisotropy_ = std::min(static_cast<float>(cfg_.anisotropy), max_anisotropy_);
        }
    }

    if (fixed_render_h_ > 0 || std::fabs(scale - 1.0f) >= 0.01f) {
        RebuildScaleFbo();
    }

    std::fprintf(stdout, "[GLES] ANGLE EGL %d.%d ready: %s | %s | %s (aniso=%.0fx/%.0fx, scale=%.2f -> %dx%d)\n",
                 major, minor, gl_vendor_.c_str(), gl_renderer_.c_str(), gl_version_.c_str(),
                 applied_anisotropy_, max_anisotropy_, scale, render_w_, render_h_);
    return true;
}

void GlesBridge::Shutdown() {
    if (display_) {
        DestroyScaleFbo();
        EGLDisplay dpy = static_cast<EGLDisplay>(display_);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_) eglDestroyContext(dpy, static_cast<EGLContext>(context_));
        if (surface_) eglDestroySurface(dpy, static_cast<EGLSurface>(surface_));
        eglTerminate(dpy);
        display_ = nullptr;
        surface_ = nullptr;
        context_ = nullptr;
    }
    if (hdc_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
    }
}

void GlesBridge::DestroyScaleFbo() {
    if (scale_fbo_) { glDeleteFramebuffers(1, &scale_fbo_); scale_fbo_ = 0; }
    if (scale_color_tex_) { glDeleteTextures(1, &scale_color_tex_); scale_color_tex_ = 0; }
    if (scale_depth_rb_) { glDeleteRenderbuffers(1, &scale_depth_rb_); scale_depth_rb_ = 0; }
    if (blit_prog_) { glDeleteProgram(blit_prog_); blit_prog_ = 0; }
    if (blit_vbo_) { glDeleteBuffers(1, &blit_vbo_); blit_vbo_ = 0; }
}

void GlesBridge::RebuildScaleFbo() {
    if (scale_fbo_) {
        glDeleteFramebuffers(1, &scale_fbo_); scale_fbo_ = 0;
        glDeleteTextures(1, &scale_color_tex_); scale_color_tex_ = 0;
        glDeleteRenderbuffers(1, &scale_depth_rb_); scale_depth_rb_ = 0;
    }
    GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);

    glGenTextures(1, &scale_color_tex_);
    glBindTexture(GL_TEXTURE_2D, scale_color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, render_w_, render_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &scale_depth_rb_);
    glBindRenderbuffer(GL_RENDERBUFFER, scale_depth_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, render_w_, render_h_);

    glGenFramebuffers(1, &scale_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scale_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scale_color_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scale_depth_rb_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scale_depth_rb_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GLES] Supersample FBO incomplete (0x%x); falling back to 1.0x\n", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyScaleFbo();
        render_w_ = window_w_;
        render_h_ = window_h_;
    } else if (!blit_prog_) {
        static const char* kVs =
            "attribute vec2 aPos;\n"
            "varying vec2 vUv;\n"
            "void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        static const char* kFs =
            "precision mediump float;\n"
            "varying vec2 vUv;\n"
            "uniform sampler2D uTex;\n"
            "void main() { gl_FragColor = texture2D(uTex, vUv); }\n";
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &kVs, nullptr);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &kFs, nullptr);
        glCompileShader(fs);
        blit_prog_ = glCreateProgram();
        glAttachShader(blit_prog_, vs);
        glAttachShader(blit_prog_, fs);
        glBindAttribLocation(blit_prog_, 0, "aPos");
        glLinkProgram(blit_prog_);
        glDeleteShader(vs);
        glDeleteShader(fs);

        static const float kQuad[8] = { -1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f };
        GLint prev_buf = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
        glGenBuffers(1, &blit_vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, blit_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_buf));
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prev_rb));
    glBindFramebuffer(GL_FRAMEBUFFER, scale_fbo_ ? scale_fbo_ : static_cast<GLuint>(prev_fbo));
}

void GlesBridge::GetPresentRect(int& out_x, int& out_y, int& out_w, int& out_h) const {
    int ww = std::max(1, window_w_);
    int wh = std::max(1, window_h_);
    int rw = std::max(1, render_w_);
    int rh = std::max(1, render_h_);
    double win_aspect = static_cast<double>(ww) / wh;
    double ren_aspect = static_cast<double>(rw) / rh;
    if (std::fabs(win_aspect - ren_aspect) < 0.015) {
        out_x = 0; out_y = 0; out_w = ww; out_h = wh;
        return;
    }
    if (win_aspect > ren_aspect) {
        out_h = wh;
        out_w = std::clamp(static_cast<int>(std::lround(wh * ren_aspect)), 1, ww);
        out_x = (ww - out_w) / 2;
        out_y = 0;
    } else {
        out_w = ww;
        out_h = std::clamp(static_cast<int>(std::lround(ww / ren_aspect)), 1, wh);
        out_x = 0;
        out_y = (wh - out_h) / 2;
    }
}

void GlesBridge::BlitScaleFboToBackbuffer() {
    if (!scale_fbo_ || !blit_prog_) return;

    // Keep backbuffer dimensions synced to live Win32 client area on resize/fullscreen switches.
    RECT client_rc{};
    if (hwnd_ && GetClientRect(hwnd_, &client_rc)) {
        int cw = client_rc.right - client_rc.left;
        int ch = client_rc.bottom - client_rc.top;
        if (cw > 0 && ch > 0) {
            window_w_ = cw;
            window_h_ = ch;
        }
    }

    // Save state touched by fullscreen blit (including OES VAO and Vertex Attrib 0!)
    static auto fn_bind_vao = reinterpret_cast<PFNGLBINDVERTEXARRAYOESPROC>(eglGetProcAddress("glBindVertexArrayOES"));
    GLint prev_vao = 0;
    if (fn_bind_vao) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &prev_vao);
        if (prev_vao != 0) fn_bind_vao(0);
    }

    GLint prev_fbo = 0, prev_prog = 0, prev_tex = 0, prev_act = 0, prev_abuf = 0;
    GLint prev_vp[4] = {};
    GLfloat prev_clear[4] = {};
    GLint prev_va0_enabled = 0, prev_va0_size = 4, prev_va0_type = GL_FLOAT;
    GLint prev_va0_norm = 0, prev_va0_stride = 0, prev_va0_buf = 0;
    void* prev_va0_ptr = nullptr;
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean prev_cmask[4] = {1, 1, 1, 1};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_act);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abuf);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_cmask);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prev_va0_enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &prev_va0_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &prev_va0_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &prev_va0_norm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &prev_va0_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &prev_va0_buf);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &prev_va0_ptr);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    int vx = 0, vy = 0, vw = window_w_, vh = window_h_;
    GetPresentRect(vx, vy, vw, vh);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (vx > 0 || vy > 0 || vw < window_w_ || vh < window_h_) {
        glViewport(0, 0, window_w_, window_h_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
    }
    glViewport(vx, vy, vw, vh);

    glUseProgram(blit_prog_);
    glBindTexture(GL_TEXTURE_2D, scale_color_tex_);
    glUniform1i(glGetUniformLocation(blit_prog_, "uTex"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, blit_vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Restore Vertex Attrib 0, VAO, and GL state
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_va0_buf));
    glVertexAttribPointer(0, prev_va0_size, static_cast<GLenum>(prev_va0_type),
                          static_cast<GLboolean>(prev_va0_norm), prev_va0_stride, prev_va0_ptr);
    if (!prev_va0_enabled) glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_abuf));
    if (fn_bind_vao && prev_vao != 0) fn_bind_vao(static_cast<GLuint>(prev_vao));

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glActiveTexture(static_cast<GLenum>(prev_act));
    glUseProgram(static_cast<GLuint>(prev_prog));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glColorMask(prev_cmask[0], prev_cmask[1], prev_cmask[2], prev_cmask[3]);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_blend) glEnable(GL_BLEND);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

bool GlesBridge::Resize(int window_w, int window_h) {
    if (window_w <= 0 || window_h <= 0) return false;
    window_w_ = window_w;
    window_h_ = window_h;
    if (fixed_render_h_ > 0) {
        // Keep UI/render height fixed at fixed_render_h_, and adapt render_w to match the window's
        // exact aspect ratio so there are ZERO black bars on any landscape resize/maximize/fullscreen!
        double aspect = static_cast<double>(window_w_) / std::max(1, window_h_);
        int new_rw = std::clamp(static_cast<int>(std::lround(fixed_render_h_ * aspect)) & ~1,
                                fixed_render_h_ + 16, 4096);
        int new_rh = fixed_render_h_;
        if (std::abs(new_rw - render_w_) < 4 && new_rh == render_h_) {
            return false;
        }
        render_w_ = new_rw;
        render_h_ = new_rh;
        if (context_) {
            RebuildScaleFbo();
        }
        return true;
    }
    float scale = std::clamp(cfg_.render_scale, 0.25f, 4.0f);
    int new_rw = std::max(2, static_cast<int>(std::lround(window_w_ * scale)) & ~1);
    int new_rh = std::max(2, static_cast<int>(std::lround(window_h_ * scale)) & ~1);
    bool changed = (new_rw != render_w_ || new_rh != render_h_);
    render_w_ = new_rw;
    render_h_ = new_rh;
    if (std::fabs(scale - 1.0f) >= 0.01f && context_) {
        RebuildScaleFbo();
    }
    return changed;
}

void GlesBridge::SetVsync(bool enabled) {
    cfg_.vsync = enabled;
    if (display_) {
        eglSwapInterval(static_cast<EGLDisplay>(display_), enabled ? 1 : 0);
    }
}

void GlesBridge::SwapBuffers() {
    if (!display_ || !surface_) return;
    if (scale_fbo_) {
        BlitScaleFboToBackbuffer();
    }
    eglSwapBuffers(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_));
    ++swap_count_;
    if (scale_fbo_) {
        glBindFramebuffer(GL_FRAMEBUFFER, scale_fbo_);
    }
}

bool GlesBridge::ReadPixelsRgba(std::vector<uint8_t>& out_rgba, int& out_w, int& out_h, bool from_backbuffer) {
    if (!context_) return false;
    GLint prev_fbo = 0, prev_pack = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
    if (from_backbuffer && scale_fbo_) {
        BlitScaleFboToBackbuffer();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        out_w = window_w_;
        out_h = window_h_;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, scale_fbo_ ? scale_fbo_ : 0);
        out_w = scale_fbo_ ? render_w_ : window_w_;
        out_h = scale_fbo_ ? render_h_ : window_h_;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(out_w) * out_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, out_w, out_h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));

    // Flip vertically from GL bottom-left to standard top-left image
    out_rgba.resize(raw.size());
    const size_t stride = static_cast<size_t>(out_w) * 4;
    for (int y = 0; y < out_h; ++y) {
        std::memcpy(out_rgba.data() + static_cast<size_t>(y) * stride,
                    raw.data() + static_cast<size_t>(out_h - 1 - y) * stride,
                    stride);
    }
    return true;
}

uint32_t GlesBridge::UploadRgbaTexture(const void* pixels, int img_w, int img_h, int tex_w, int tex_h, bool flip_y) {
    if (!context_ || !pixels || img_w <= 0 || img_h <= 0) return 0;
    if (tex_w < img_w) tex_w = img_w;
    if (tex_h < img_h) tex_h = img_h;

    std::vector<uint8_t> buf(static_cast<size_t>(tex_w) * tex_h * 4, 0);
    const auto* src = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < img_h; ++y) {
        int dst_y = flip_y ? (img_h - 1 - y) : y;
        std::memcpy(buf.data() + static_cast<size_t>(dst_y) * tex_w * 4,
                    src + static_cast<size_t>(y) * img_w * 4,
                    static_cast<size_t>(img_w) * 4);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    return tex;
}

uint64_t GlesBridge::WrapDynamicProc(const char* name) {
    if (!name || !*name) return 0;
    std::lock_guard<std::mutex> lk(proc_mtx_);

    if (auto it = static_stubs_.find(name); it != static_stubs_.end()) {
        return it->second;
    }
    if (auto it = dynamic_stubs_.find(name); it != dynamic_stubs_.end()) {
        return it->second;
    }

    auto raw = eglGetProcAddress(name);
    if (!raw) {
        dynamic_stubs_[name] = 0;
        return 0;
    }

    auto reg = [&](SvcHandlerFn fn) -> uint64_t {
        uint64_t stub = cpu_.AllocateSvcStub(std::string("dyn:") + name, std::move(fn));
        dynamic_stubs_[name] = stub;
        return stub;
    };

    std::string nm(name);

    // ---- OES Vertex Array Object ----
    if (nm == "glBindVertexArrayOES" || nm == "glBindVertexArray") {
        auto f = reinterpret_cast<PFNGLBINDVERTEXARRAYOESPROC>(raw);
        return reg([f](CpuContext& c) { f(c.GetW(0)); });
    }
    if (nm == "glGenVertexArraysOES" || nm == "glGenVertexArrays") {
        auto f = reinterpret_cast<PFNGLGENVERTEXARRAYSOESPROC>(raw);
        return reg([f](CpuContext& c) { f(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    }
    if (nm == "glDeleteVertexArraysOES" || nm == "glDeleteVertexArrays") {
        auto f = reinterpret_cast<PFNGLDELETEVERTEXARRAYSOESPROC>(raw);
        return reg([f](CpuContext& c) { f(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1))); });
    }
    if (nm == "glIsVertexArrayOES" || nm == "glIsVertexArray") {
        auto f = reinterpret_cast<PFNGLISVERTEXARRAYOESPROC>(raw);
        return reg([f](CpuContext& c) { c.SetW(0, f(c.GetW(0))); });
    }

    // ---- MapBuffer / MapBufferRange ----
    if (nm == "glMapBufferOES" || nm == "glMapBufferARB") {
        auto f = reinterpret_cast<PFNGLMAPBUFFEROESPROC>(raw);
        return reg([f](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(f(c.GetW(0), c.GetW(1)))); });
    }
    if (nm == "glUnmapBufferOES" || nm == "glUnmapBuffer" || nm == "glUnmapBufferARB") {
        auto f = reinterpret_cast<PFNGLUNMAPBUFFEROESPROC>(raw);
        return reg([f](CpuContext& c) { c.SetW(0, f(c.GetW(0))); });
    }
    if (nm == "glMapBufferRangeEXT" || nm == "glMapBufferRange") {
        auto f = reinterpret_cast<PFNGLMAPBUFFERRANGEEXTPROC>(raw);
        return reg([f](CpuContext& c) {
            void* p = f(c.GetW(0), static_cast<GLintptr>(c.GetX(1)),
                        static_cast<GLsizeiptr>(c.GetX(2)), c.GetW(3));
            c.SetX(0, reinterpret_cast<uint64_t>(p));
        });
    }
    if (nm == "glFlushMappedBufferRangeEXT" || nm == "glFlushMappedBufferRange") {
        auto f = reinterpret_cast<PFNGLFLUSHMAPPEDBUFFERRANGEEXTPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), static_cast<GLintptr>(c.GetX(1)), static_cast<GLsizeiptr>(c.GetX(2)));
        });
    }

    // ---- BlitFramebuffer / Multisample / Discard / Invalidate ----
    if (nm == "glBlitFramebuffer" || nm == "glBlitFramebufferANGLE" ||
        nm == "glBlitFramebufferNV" || nm == "glBlitFramebufferEXT") {
        auto f = reinterpret_cast<PFNGLBLITFRAMEBUFFERANGLEPROC>(raw);
        return reg([f](CpuContext& c) {
            f(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)),
              static_cast<GLint>(c.GetW(2)), static_cast<GLint>(c.GetW(3)),
              static_cast<GLint>(c.GetW(4)), static_cast<GLint>(c.GetW(5)),
              static_cast<GLint>(c.GetW(6)), static_cast<GLint>(c.GetW(7)),
              static_cast<GLbitfield>(c.ReadStackU64(0)),
              static_cast<GLenum>(c.ReadStackU64(1)));
        });
    }
    if (nm == "glRenderbufferStorageMultisample" || nm == "glRenderbufferStorageMultisampleANGLE" ||
        nm == "glRenderbufferStorageMultisampleEXT" || nm == "glRenderbufferStorageMultisampleIMG" ||
        nm == "glRenderbufferStorageMultisampleNV" || nm == "glRenderbufferStorageMultisampleAPPLE") {
        auto f = reinterpret_cast<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEANGLEPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), c.GetW(2),
              static_cast<GLsizei>(c.GetW(3)), static_cast<GLsizei>(c.GetW(4)));
        });
    }
    if (nm == "glFramebufferTexture2DMultisampleEXT" || nm == "glFramebufferTexture2DMultisampleIMG") {
        auto f = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), c.GetW(1), c.GetW(2), c.GetW(3),
              static_cast<GLint>(c.GetW(4)), static_cast<GLsizei>(c.GetW(5)));
        });
    }
    if (nm == "glDiscardFramebufferEXT" || nm == "glInvalidateFramebuffer") {
        auto f = reinterpret_cast<PFNGLDISCARDFRAMEBUFFEREXTPROC>(raw);
        return reg([this, f](CpuContext& c) {
            GLenum target = c.GetW(0);
            GLsizei n = static_cast<GLsizei>(c.GetW(1));
            const GLenum* att = CPtr<GLenum>(c.GetX(2));
            if (scale_fbo_ != 0 && att && n > 0 && n <= 8) {
                GLint cur_fb = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur_fb);
                if (static_cast<GLuint>(cur_fb) == scale_fbo_) {
                    GLenum mapped[8]{};
                    for (GLsizei i = 0; i < n; ++i) {
                        if (att[i] == GL_COLOR_EXT) mapped[i] = GL_COLOR_ATTACHMENT0;
                        else if (att[i] == GL_DEPTH_EXT) mapped[i] = GL_DEPTH_ATTACHMENT;
                        else if (att[i] == GL_STENCIL_EXT) mapped[i] = GL_STENCIL_ATTACHMENT;
                        else mapped[i] = att[i];
                    }
                    f(target, n, mapped);
                    return;
                }
            }
            f(target, n, att);
        });
    }
    if (nm == "glInvalidateSubFramebuffer") {
        auto f = reinterpret_cast<PFNGLINVALIDATESUBFRAMEBUFFERPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), CPtr<GLenum>(c.GetX(2)),
              static_cast<GLint>(c.GetW(3)), static_cast<GLint>(c.GetW(4)),
              static_cast<GLsizei>(c.GetW(5)), static_cast<GLsizei>(c.GetW(6)));
        });
    }

    // ---- Instanced drawing ----
    if (nm == "glDrawArraysInstanced" || nm == "glDrawArraysInstancedANGLE" ||
        nm == "glDrawArraysInstancedEXT" || nm == "glDrawArraysInstancedNV") {
        auto f = reinterpret_cast<PFNGLDRAWARRAYSINSTANCEDANGLEPROC>(raw);
        return reg([this, f](CpuContext& c) {
            ++draw_calls_;
            f(c.GetW(0), static_cast<GLint>(c.GetW(1)), static_cast<GLsizei>(c.GetW(2)),
              static_cast<GLsizei>(c.GetW(3)));
        });
    }
    if (nm == "glDrawElementsInstanced" || nm == "glDrawElementsInstancedANGLE" ||
        nm == "glDrawElementsInstancedEXT" || nm == "glDrawElementsInstancedNV") {
        auto f = reinterpret_cast<PFNGLDRAWELEMENTSINSTANCEDANGLEPROC>(raw);
        return reg([this, f](CpuContext& c) {
            ++draw_calls_;
            f(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), c.GetW(2),
              CPtr<void>(c.GetX(3)), static_cast<GLsizei>(c.GetW(4)));
        });
    }
    if (nm == "glVertexAttribDivisor" || nm == "glVertexAttribDivisorANGLE" ||
        nm == "glVertexAttribDivisorEXT" || nm == "glVertexAttribDivisorNV") {
        auto f = reinterpret_cast<PFNGLVERTEXATTRIBDIVISORANGLEPROC>(raw);
        return reg([f](CpuContext& c) { f(c.GetW(0), c.GetW(1)); });
    }

    // ---- TexStorage / DrawBuffers / ProgramBinary ----
    if (nm == "glTexStorage2DEXT" || nm == "glTexStorage2D") {
        auto f = reinterpret_cast<PFNGLTEXSTORAGE2DEXTPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), c.GetW(2),
              static_cast<GLsizei>(c.GetW(3)), static_cast<GLsizei>(c.GetW(4)));
        });
    }
    if (nm == "glDrawBuffersEXT" || nm == "glDrawBuffers" || nm == "glDrawBuffersNV") {
        auto f = reinterpret_cast<PFNGLDRAWBUFFERSEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(static_cast<GLsizei>(c.GetW(0)), CPtr<GLenum>(c.GetX(1))); });
    }
    if (nm == "glGetProgramBinaryOES" || nm == "glGetProgramBinary") {
        auto f = reinterpret_cast<PFNGLGETPROGRAMBINARYOESPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), Ptr<GLsizei>(c.GetX(2)),
              Ptr<GLenum>(c.GetX(3)), Ptr<void>(c.GetX(4)));
        });
    }
    if (nm == "glProgramBinaryOES" || nm == "glProgramBinary") {
        auto f = reinterpret_cast<PFNGLPROGRAMBINARYOESPROC>(raw);
        return reg([f](CpuContext& c) {
            f(c.GetW(0), c.GetW(1), CPtr<void>(c.GetX(2)), static_cast<GLint>(c.GetW(3)));
        });
    }

    // ---- Sync objects (GL & EGL) ----
    if (nm == "glFenceSync" || nm == "glFenceSyncAPPLE") {
        auto f = reinterpret_cast<PFNGLFENCESYNCPROC>(raw);
        return reg([f](CpuContext& c) {
            c.SetX(0, reinterpret_cast<uint64_t>(f(c.GetW(0), c.GetW(1))));
        });
    }
    if (nm == "glDeleteSync" || nm == "glDeleteSyncAPPLE") {
        auto f = reinterpret_cast<PFNGLDELETESYNCPROC>(raw);
        return reg([f](CpuContext& c) { f(reinterpret_cast<GLsync>(c.GetX(0))); });
    }
    if (nm == "glClientWaitSync" || nm == "glClientWaitSyncAPPLE") {
        auto f = reinterpret_cast<PFNGLCLIENTWAITSYNCPROC>(raw);
        return reg([f](CpuContext& c) {
            c.SetW(0, f(reinterpret_cast<GLsync>(c.GetX(0)), c.GetW(1), c.GetX(2)));
        });
    }
    if (nm == "glWaitSync" || nm == "glWaitSyncAPPLE") {
        auto f = reinterpret_cast<PFNGLWAITSYNCPROC>(raw);
        return reg([f](CpuContext& c) { f(reinterpret_cast<GLsync>(c.GetX(0)), c.GetW(1), c.GetX(2)); });
    }
    if (nm == "eglCreateSyncKHR") {
        auto f = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(raw);
        return reg([this, f](CpuContext& c) {
            EGLDisplay dpy = c.GetX(0) ? reinterpret_cast<EGLDisplay>(c.GetX(0)) : static_cast<EGLDisplay>(display_);
            c.SetX(0, reinterpret_cast<uint64_t>(f(dpy, c.GetW(1), CPtr<EGLint>(c.GetX(2)))));
        });
    }
    if (nm == "eglDestroySyncKHR") {
        auto f = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(raw);
        return reg([this, f](CpuContext& c) {
            EGLDisplay dpy = c.GetX(0) ? reinterpret_cast<EGLDisplay>(c.GetX(0)) : static_cast<EGLDisplay>(display_);
            c.SetW(0, f(dpy, reinterpret_cast<EGLSyncKHR>(c.GetX(1))));
        });
    }
    if (nm == "eglClientWaitSyncKHR") {
        auto f = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(raw);
        return reg([this, f](CpuContext& c) {
            EGLDisplay dpy = c.GetX(0) ? reinterpret_cast<EGLDisplay>(c.GetX(0)) : static_cast<EGLDisplay>(display_);
            c.SetW(0, static_cast<uint32_t>(f(dpy, reinterpret_cast<EGLSyncKHR>(c.GetX(1)), c.GetW(2), c.GetX(3))));
        });
    }

    // ---- Debug markers (safe no-ops or forward) ----
    if (nm == "glInsertEventMarkerEXT" || nm == "glPushGroupMarkerEXT" || nm == "glPopGroupMarkerEXT") {
        return reg([](CpuContext&) {});
    }

    // ---- Queries ----
    if (nm == "glGenQueriesEXT" || nm == "glGenQueries") {
        auto f = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    }
    if (nm == "glDeleteQueriesEXT" || nm == "glDeleteQueries") {
        auto f = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1))); });
    }
    if (nm == "glBeginQueryEXT" || nm == "glBeginQuery") {
        auto f = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(c.GetW(0), c.GetW(1)); });
    }
    if (nm == "glEndQueryEXT" || nm == "glEndQuery") {
        auto f = reinterpret_cast<PFNGLENDQUERYEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(c.GetW(0)); });
    }
    if (nm == "glGetQueryObjectuivEXT" || nm == "glGetQueryObjectuiv") {
        auto f = reinterpret_cast<PFNGLGETQUERYOBJECTUIVEXTPROC>(raw);
        return reg([f](CpuContext& c) { f(c.GetW(0), c.GetW(1), Ptr<GLuint>(c.GetX(2))); });
    }
    if (nm == "glGetStringi") {
        auto f = reinterpret_cast<PFNGLGETSTRINGIPROC>(raw);
        return reg([f](CpuContext& c) {
            c.SetX(0, reinterpret_cast<uint64_t>(f(c.GetW(0), c.GetW(1))));
        });
    }

    if (IsVerboseLogging()) {
        std::fprintf(stderr, "[GLES] Unwrapped eglGetProcAddress('%s') -> returning NULL\n", name);
    }
    dynamic_stubs_[name] = 0;
    return 0;
}

void GlesBridge::RegisterSymbols(ElfLoader& loader) {
    auto fn = [&](const std::string& name, SvcHandlerFn handler) {
        uint64_t addr = loader.RegisterHostFunction(name, std::move(handler));
        static_stubs_[name] = addr;
    };

    // ---- 3 EGL symbols ----
    fn("eglGetDisplay", [this](CpuContext& c) {
        c.SetX(0, reinterpret_cast<uint64_t>(display_));
    });
    fn("eglQueryString", [this](CpuContext& c) {
        EGLDisplay dpy = c.GetX(0) ? reinterpret_cast<EGLDisplay>(c.GetX(0)) : static_cast<EGLDisplay>(display_);
        const char* s = eglQueryString(dpy, static_cast<EGLint>(c.GetW(1)));
        c.SetX(0, reinterpret_cast<uint64_t>(s));
    });
    fn("eglGetProcAddress", [this](CpuContext& c) {
        const char* name = CPtr<char>(c.GetX(0));
        c.SetX(0, WrapDynamicProc(name));
    });

    // ---- All 142 GLES 2.0 core symbols ----
    fn("glActiveTexture", [](CpuContext& c) { glActiveTexture(c.GetW(0)); });
    fn("glAttachShader", [](CpuContext& c) { glAttachShader(c.GetW(0), c.GetW(1)); });
    fn("glBindAttribLocation", [](CpuContext& c) { glBindAttribLocation(c.GetW(0), c.GetW(1), CPtr<GLchar>(c.GetX(2))); });
    fn("glBindBuffer", [](CpuContext& c) { glBindBuffer(c.GetW(0), c.GetW(1)); });
    fn("glBindFramebuffer", [this](CpuContext& c) {
        GLuint fb = c.GetW(1);
        if (fb == 0 && scale_fbo_ != 0) fb = scale_fbo_;
        glBindFramebuffer(c.GetW(0), fb);
    });
    fn("glBindRenderbuffer", [](CpuContext& c) { glBindRenderbuffer(c.GetW(0), c.GetW(1)); });
    fn("glBindTexture", [](CpuContext& c) { glBindTexture(c.GetW(0), c.GetW(1)); });
    fn("glBlendColor", [](CpuContext& c) { glBlendColor(c.GetS(0), c.GetS(1), c.GetS(2), c.GetS(3)); });
    fn("glBlendEquation", [](CpuContext& c) { glBlendEquation(c.GetW(0)); });
    fn("glBlendEquationSeparate", [](CpuContext& c) { glBlendEquationSeparate(c.GetW(0), c.GetW(1)); });
    fn("glBlendFunc", [](CpuContext& c) { glBlendFunc(c.GetW(0), c.GetW(1)); });
    fn("glBlendFuncSeparate", [](CpuContext& c) { glBlendFuncSeparate(c.GetW(0), c.GetW(1), c.GetW(2), c.GetW(3)); });
    fn("glBufferData", [](CpuContext& c) {
        glBufferData(c.GetW(0), static_cast<GLsizeiptr>(c.GetX(1)), CPtr<void>(c.GetX(2)), c.GetW(3));
    });
    fn("glBufferSubData", [](CpuContext& c) {
        glBufferSubData(c.GetW(0), static_cast<GLintptr>(c.GetX(1)), static_cast<GLsizeiptr>(c.GetX(2)), CPtr<void>(c.GetX(3)));
    });
    fn("glCheckFramebufferStatus", [](CpuContext& c) { c.SetW(0, glCheckFramebufferStatus(c.GetW(0))); });
    fn("glClear", [](CpuContext& c) { glClear(c.GetW(0)); });
    fn("glClearColor", [](CpuContext& c) { glClearColor(c.GetS(0), c.GetS(1), c.GetS(2), c.GetS(3)); });
    fn("glClearDepthf", [](CpuContext& c) { glClearDepthf(c.GetS(0)); });
    fn("glClearStencil", [](CpuContext& c) { glClearStencil(static_cast<GLint>(c.GetW(0))); });
    fn("glColorMask", [](CpuContext& c) {
        glColorMask(static_cast<GLboolean>(c.GetW(0)), static_cast<GLboolean>(c.GetW(1)),
                    static_cast<GLboolean>(c.GetW(2)), static_cast<GLboolean>(c.GetW(3)));
    });
    fn("glCompileShader", [](CpuContext& c) { glCompileShader(c.GetW(0)); });
    fn("glCompressedTexImage2D", [this](CpuContext& c) {
        GLenum target = c.GetW(0);
        GLint level = static_cast<GLint>(c.GetW(1));
        GLenum fmt = c.GetW(2);
        GLsizei w = static_cast<GLsizei>(c.GetW(3));
        GLsizei h = static_cast<GLsizei>(c.GetW(4));
        GLint border = static_cast<GLint>(c.GetW(5));
        GLsizei img_sz = static_cast<GLsizei>(c.GetW(6));
        const void* data = CPtr<void>(c.GetX(7));

        bool decode_cpu = IsAtcFormat(fmt) || fmt == kGlEtc1Rgb8Oes || IsPvrtcFormat(fmt);
        if (IsDxtFormat(fmt)) {
            GLint bound = 0;
            glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &bound);
            const uint32_t tex_id = static_cast<uint32_t>(bound);
            if (!host_supports_s3tc_ || (level == 0 && ((w & 3) != 0 || (h & 3) != 0)) ||
                (level > 0 && cpu_decoded_dxt_tex_.count(tex_id) > 0)) {
                decode_cpu = true;
                if (tex_id != 0) cpu_decoded_dxt_tex_.insert(tex_id);
            } else if (level == 0 && tex_id != 0) {
                cpu_decoded_dxt_tex_.erase(tex_id);
            }
        }
        if (decode_cpu) {
            std::vector<uint8_t> rgba;
            if (DecodeCompressedToRgba(fmt, w, h, static_cast<const uint8_t*>(data), img_sz, rgba)) {
                GLint prev_align = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
                return;
            }
        }
        glCompressedTexImage2D(target, level, fmt, w, h, border, img_sz, data);
    });
    fn("glCompressedTexSubImage2D", [this](CpuContext& c) {
        GLenum target = c.GetW(0);
        GLint level = static_cast<GLint>(c.GetW(1));
        GLint xoff = static_cast<GLint>(c.GetW(2));
        GLint yoff = static_cast<GLint>(c.GetW(3));
        GLsizei w = static_cast<GLsizei>(c.GetW(4));
        GLsizei h = static_cast<GLsizei>(c.GetW(5));
        GLenum fmt = c.GetW(6);
        GLsizei img_sz = static_cast<GLsizei>(c.GetW(7));
        const void* data = CPtr<void>(c.ReadStackU64(0));

        bool decode_cpu = IsAtcFormat(fmt) || fmt == kGlEtc1Rgb8Oes || IsPvrtcFormat(fmt);
        if (IsDxtFormat(fmt)) {
            GLint bound = 0;
            glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &bound);
            if (!host_supports_s3tc_ || cpu_decoded_dxt_tex_.count(static_cast<uint32_t>(bound)) > 0) {
                decode_cpu = true;
            }
        }
        if (decode_cpu) {
            std::vector<uint8_t> rgba;
            if (DecodeCompressedToRgba(fmt, w, h, static_cast<const uint8_t*>(data), img_sz, rgba)) {
                GLint prev_align = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(target, level, xoff, yoff, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
                return;
            }
        }
        glCompressedTexSubImage2D(target, level, xoff, yoff, w, h, fmt, img_sz, data);
    });
    fn("glCopyTexImage2D", [](CpuContext& c) {
        glCopyTexImage2D(c.GetW(0), static_cast<GLint>(c.GetW(1)), c.GetW(2),
                         static_cast<GLint>(c.GetW(3)), static_cast<GLint>(c.GetW(4)),
                         static_cast<GLsizei>(c.GetW(5)), static_cast<GLsizei>(c.GetW(6)),
                         static_cast<GLint>(c.GetW(7)));
    });
    fn("glCopyTexSubImage2D", [](CpuContext& c) {
        glCopyTexSubImage2D(c.GetW(0), static_cast<GLint>(c.GetW(1)),
                            static_cast<GLint>(c.GetW(2)), static_cast<GLint>(c.GetW(3)),
                            static_cast<GLint>(c.GetW(4)), static_cast<GLint>(c.GetW(5)),
                            static_cast<GLsizei>(c.GetW(6)), static_cast<GLsizei>(c.GetW(7)));
    });
    fn("glCreateProgram", [](CpuContext& c) { c.SetW(0, glCreateProgram()); });
    fn("glCreateShader", [](CpuContext& c) { c.SetW(0, glCreateShader(c.GetW(0))); });
    fn("glCullFace", [](CpuContext& c) { glCullFace(c.GetW(0)); });
    fn("glDeleteBuffers", [](CpuContext& c) { glDeleteBuffers(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1))); });
    fn("glDeleteFramebuffers", [](CpuContext& c) { glDeleteFramebuffers(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1))); });
    fn("glDeleteProgram", [](CpuContext& c) { glDeleteProgram(c.GetW(0)); });
    fn("glDeleteRenderbuffers", [](CpuContext& c) { glDeleteRenderbuffers(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1))); });
    fn("glDeleteShader", [](CpuContext& c) { glDeleteShader(c.GetW(0)); });
    fn("glDeleteTextures", [this](CpuContext& c) {
        GLsizei n = static_cast<GLsizei>(c.GetW(0));
        const GLuint* ids = CPtr<GLuint>(c.GetX(1));
        if (ids && n > 0 && !cpu_decoded_dxt_tex_.empty()) {
            for (GLsizei i = 0; i < n; ++i) cpu_decoded_dxt_tex_.erase(ids[i]);
        }
        glDeleteTextures(n, ids);
    });
    fn("glDepthFunc", [](CpuContext& c) { glDepthFunc(c.GetW(0)); });
    fn("glDepthMask", [](CpuContext& c) { glDepthMask(static_cast<GLboolean>(c.GetW(0))); });
    fn("glDepthRangef", [](CpuContext& c) { glDepthRangef(c.GetS(0), c.GetS(1)); });
    fn("glDetachShader", [](CpuContext& c) { glDetachShader(c.GetW(0), c.GetW(1)); });
    fn("glDisable", [](CpuContext& c) { glDisable(c.GetW(0)); });
    fn("glDisableVertexAttribArray", [](CpuContext& c) { glDisableVertexAttribArray(c.GetW(0)); });
    fn("glDrawArrays", [this](CpuContext& c) {
        ++draw_calls_;
        glDrawArrays(c.GetW(0), static_cast<GLint>(c.GetW(1)), static_cast<GLsizei>(c.GetW(2)));
    });
    fn("glDrawElements", [this](CpuContext& c) {
        ++draw_calls_;
        glDrawElements(c.GetW(0), static_cast<GLsizei>(c.GetW(1)), c.GetW(2), CPtr<void>(c.GetX(3)));
    });
    fn("glEnable", [](CpuContext& c) { glEnable(c.GetW(0)); });
    fn("glEnableVertexAttribArray", [](CpuContext& c) { glEnableVertexAttribArray(c.GetW(0)); });
    fn("glFinish", [](CpuContext&) { glFinish(); });
    fn("glFlush", [](CpuContext&) { glFlush(); });
    fn("glFramebufferRenderbuffer", [](CpuContext& c) {
        glFramebufferRenderbuffer(c.GetW(0), c.GetW(1), c.GetW(2), c.GetW(3));
    });
    fn("glFramebufferTexture2D", [](CpuContext& c) {
        glFramebufferTexture2D(c.GetW(0), c.GetW(1), c.GetW(2), c.GetW(3), static_cast<GLint>(c.GetW(4)));
    });
    fn("glFrontFace", [](CpuContext& c) { glFrontFace(c.GetW(0)); });
    fn("glGenBuffers", [](CpuContext& c) { glGenBuffers(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    fn("glGenFramebuffers", [](CpuContext& c) { glGenFramebuffers(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    fn("glGenRenderbuffers", [](CpuContext& c) { glGenRenderbuffers(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    fn("glGenTextures", [](CpuContext& c) { glGenTextures(static_cast<GLsizei>(c.GetW(0)), Ptr<GLuint>(c.GetX(1))); });
    fn("glGenerateMipmap", [](CpuContext& c) { glGenerateMipmap(c.GetW(0)); });
    fn("glGetActiveAttrib", [](CpuContext& c) {
        glGetActiveAttrib(c.GetW(0), c.GetW(1), static_cast<GLsizei>(c.GetW(2)),
                          Ptr<GLsizei>(c.GetX(3)), Ptr<GLint>(c.GetX(4)),
                          Ptr<GLenum>(c.GetX(5)), Ptr<GLchar>(c.GetX(6)));
    });
    fn("glGetActiveUniform", [](CpuContext& c) {
        glGetActiveUniform(c.GetW(0), c.GetW(1), static_cast<GLsizei>(c.GetW(2)),
                           Ptr<GLsizei>(c.GetX(3)), Ptr<GLint>(c.GetX(4)),
                           Ptr<GLenum>(c.GetX(5)), Ptr<GLchar>(c.GetX(6)));
    });
    fn("glGetAttachedShaders", [](CpuContext& c) {
        glGetAttachedShaders(c.GetW(0), static_cast<GLsizei>(c.GetW(1)),
                             Ptr<GLsizei>(c.GetX(2)), Ptr<GLuint>(c.GetX(3)));
    });
    fn("glGetAttribLocation", [](CpuContext& c) {
        c.SetW(0, static_cast<uint32_t>(glGetAttribLocation(c.GetW(0), CPtr<GLchar>(c.GetX(1)))));
    });
    fn("glGetBooleanv", [](CpuContext& c) { glGetBooleanv(c.GetW(0), Ptr<GLboolean>(c.GetX(1))); });
    fn("glGetBufferParameteriv", [](CpuContext& c) { glGetBufferParameteriv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2))); });
    fn("glGetError", [](CpuContext& c) { c.SetW(0, glGetError()); });
    fn("glGetFloatv", [](CpuContext& c) { glGetFloatv(c.GetW(0), Ptr<GLfloat>(c.GetX(1))); });
    fn("glGetFramebufferAttachmentParameteriv", [](CpuContext& c) {
        glGetFramebufferAttachmentParameteriv(c.GetW(0), c.GetW(1), c.GetW(2), Ptr<GLint>(c.GetX(3)));
    });
    fn("glGetIntegerv", [this](CpuContext& c) {
        GLenum pname = c.GetW(0);
        GLint* params = Ptr<GLint>(c.GetX(1));
        glGetIntegerv(pname, params);
        if (pname == GL_FRAMEBUFFER_BINDING && scale_fbo_ != 0 && params && static_cast<GLuint>(*params) == scale_fbo_) {
            *params = 0; // hide internal supersample FBO from guest
        }
    });
    fn("glGetProgramInfoLog", [](CpuContext& c) {
        glGetProgramInfoLog(c.GetW(0), static_cast<GLsizei>(c.GetW(1)),
                            Ptr<GLsizei>(c.GetX(2)), Ptr<GLchar>(c.GetX(3)));
    });
    fn("glGetProgramiv", [](CpuContext& c) { glGetProgramiv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2))); });
    fn("glGetRenderbufferParameteriv", [](CpuContext& c) {
        glGetRenderbufferParameteriv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2)));
    });
    fn("glGetShaderInfoLog", [](CpuContext& c) {
        glGetShaderInfoLog(c.GetW(0), static_cast<GLsizei>(c.GetW(1)),
                           Ptr<GLsizei>(c.GetX(2)), Ptr<GLchar>(c.GetX(3)));
    });
    fn("glGetShaderPrecisionFormat", [](CpuContext& c) {
        glGetShaderPrecisionFormat(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2)), Ptr<GLint>(c.GetX(3)));
    });
    fn("glGetShaderSource", [](CpuContext& c) {
        glGetShaderSource(c.GetW(0), static_cast<GLsizei>(c.GetW(1)),
                          Ptr<GLsizei>(c.GetX(2)), Ptr<GLchar>(c.GetX(3)));
    });
    fn("glGetShaderiv", [](CpuContext& c) { glGetShaderiv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2))); });
    fn("glGetString", [this](CpuContext& c) {
        GLenum name = c.GetW(0);
        if (name == GL_RENDERER && !renderer_override_.empty()) {
            c.SetX(0, reinterpret_cast<uint64_t>(renderer_override_.c_str()));
            return;
        }
        if (name == GL_VERSION) {
            static const char* kEs2Ver = "OpenGL ES 2.0 (ANGLE D3D11)";
            c.SetX(0, reinterpret_cast<uint64_t>(kEs2Ver));
            return;
        }
        if (name == GL_EXTENSIONS && !guest_extensions_.empty()) {
            c.SetX(0, reinterpret_cast<uint64_t>(guest_extensions_.c_str()));
            return;
        }
        c.SetX(0, reinterpret_cast<uint64_t>(glGetString(name)));
    });
    fn("glGetTexParameterfv", [](CpuContext& c) { glGetTexParameterfv(c.GetW(0), c.GetW(1), Ptr<GLfloat>(c.GetX(2))); });
    fn("glGetTexParameteriv", [](CpuContext& c) { glGetTexParameteriv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2))); });
    fn("glGetUniformLocation", [](CpuContext& c) {
        c.SetW(0, static_cast<uint32_t>(glGetUniformLocation(c.GetW(0), CPtr<GLchar>(c.GetX(1)))));
    });
    fn("glGetUniformfv", [](CpuContext& c) { glGetUniformfv(c.GetW(0), static_cast<GLint>(c.GetW(1)), Ptr<GLfloat>(c.GetX(2))); });
    fn("glGetUniformiv", [](CpuContext& c) { glGetUniformiv(c.GetW(0), static_cast<GLint>(c.GetW(1)), Ptr<GLint>(c.GetX(2))); });
    fn("glGetVertexAttribPointerv", [](CpuContext& c) {
        glGetVertexAttribPointerv(c.GetW(0), c.GetW(1), Ptr<void*>(c.GetX(2)));
    });
    fn("glGetVertexAttribfv", [](CpuContext& c) { glGetVertexAttribfv(c.GetW(0), c.GetW(1), Ptr<GLfloat>(c.GetX(2))); });
    fn("glGetVertexAttribiv", [](CpuContext& c) { glGetVertexAttribiv(c.GetW(0), c.GetW(1), Ptr<GLint>(c.GetX(2))); });
    fn("glHint", [](CpuContext& c) { glHint(c.GetW(0), c.GetW(1)); });
    fn("glIsBuffer", [](CpuContext& c) { c.SetW(0, glIsBuffer(c.GetW(0))); });
    fn("glIsEnabled", [](CpuContext& c) { c.SetW(0, glIsEnabled(c.GetW(0))); });
    fn("glIsFramebuffer", [](CpuContext& c) { c.SetW(0, glIsFramebuffer(c.GetW(0))); });
    fn("glIsProgram", [](CpuContext& c) { c.SetW(0, glIsProgram(c.GetW(0))); });
    fn("glIsRenderbuffer", [](CpuContext& c) { c.SetW(0, glIsRenderbuffer(c.GetW(0))); });
    fn("glIsShader", [](CpuContext& c) { c.SetW(0, glIsShader(c.GetW(0))); });
    fn("glIsTexture", [](CpuContext& c) { c.SetW(0, glIsTexture(c.GetW(0))); });
    fn("glLineWidth", [](CpuContext& c) { glLineWidth(c.GetS(0)); });
    fn("glLinkProgram", [](CpuContext& c) { glLinkProgram(c.GetW(0)); });
    fn("glPixelStorei", [](CpuContext& c) { glPixelStorei(c.GetW(0), static_cast<GLint>(c.GetW(1))); });
    fn("glPolygonOffset", [](CpuContext& c) { glPolygonOffset(c.GetS(0), c.GetS(1)); });
    fn("glReadPixels", [](CpuContext& c) {
        glReadPixels(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)),
                     static_cast<GLsizei>(c.GetW(2)), static_cast<GLsizei>(c.GetW(3)),
                     c.GetW(4), c.GetW(5), Ptr<void>(c.GetX(6)));
    });
    fn("glReleaseShaderCompiler", [](CpuContext&) { glReleaseShaderCompiler(); });
    fn("glRenderbufferStorage", [](CpuContext& c) {
        glRenderbufferStorage(c.GetW(0), c.GetW(1), static_cast<GLsizei>(c.GetW(2)), static_cast<GLsizei>(c.GetW(3)));
    });
    fn("glSampleCoverage", [](CpuContext& c) {
        // AAPCS64: float in S0, GLboolean (first int arg) in W0!
        glSampleCoverage(c.GetS(0), static_cast<GLboolean>(c.GetW(0)));
    });
    fn("glScissor", [](CpuContext& c) {
        glScissor(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)),
                  static_cast<GLsizei>(c.GetW(2)), static_cast<GLsizei>(c.GetW(3)));
    });
    fn("glShaderBinary", [](CpuContext& c) {
        glShaderBinary(static_cast<GLsizei>(c.GetW(0)), CPtr<GLuint>(c.GetX(1)),
                       c.GetW(2), CPtr<void>(c.GetX(3)), static_cast<GLsizei>(c.GetW(4)));
    });
    fn("glShaderSource", [](CpuContext& c) {
        glShaderSource(c.GetW(0), static_cast<GLsizei>(c.GetW(1)),
                       CPtr<const GLchar*>(c.GetX(2)), CPtr<GLint>(c.GetX(3)));
    });
    fn("glStencilFunc", [](CpuContext& c) {
        glStencilFunc(c.GetW(0), static_cast<GLint>(c.GetW(1)), c.GetW(2));
    });
    fn("glStencilFuncSeparate", [](CpuContext& c) {
        glStencilFuncSeparate(c.GetW(0), c.GetW(1), static_cast<GLint>(c.GetW(2)), c.GetW(3));
    });
    fn("glStencilMask", [](CpuContext& c) { glStencilMask(c.GetW(0)); });
    fn("glStencilMaskSeparate", [](CpuContext& c) { glStencilMaskSeparate(c.GetW(0), c.GetW(1)); });
    fn("glStencilOp", [](CpuContext& c) { glStencilOp(c.GetW(0), c.GetW(1), c.GetW(2)); });
    fn("glStencilOpSeparate", [](CpuContext& c) { glStencilOpSeparate(c.GetW(0), c.GetW(1), c.GetW(2), c.GetW(3)); });
    fn("glTexImage2D", [](CpuContext& c) {
        glTexImage2D(c.GetW(0), static_cast<GLint>(c.GetW(1)), static_cast<GLint>(c.GetW(2)),
                     static_cast<GLsizei>(c.GetW(3)), static_cast<GLsizei>(c.GetW(4)),
                     static_cast<GLint>(c.GetW(5)), c.GetW(6), c.GetW(7),
                     CPtr<void>(c.ReadStackU64(0)));
    });
    fn("glTexParameterf", [this](CpuContext& c) {
        GLenum target = c.GetW(0), pname = c.GetW(1);
        GLfloat param = c.GetS(0);
        if (pname == kGlTextureMaxAnisotropyExt && applied_anisotropy_ > param) {
            param = applied_anisotropy_;
        }
        glTexParameterf(target, pname, param);
    });
    fn("glTexParameterfv", [](CpuContext& c) { glTexParameterfv(c.GetW(0), c.GetW(1), CPtr<GLfloat>(c.GetX(2))); });
    fn("glTexParameteri", [this](CpuContext& c) {
        GLenum target = c.GetW(0), pname = c.GetW(1);
        GLint param = static_cast<GLint>(c.GetW(2));
        glTexParameteri(target, pname, param);
        if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER && applied_anisotropy_ > 1.0f &&
            (param == GL_LINEAR_MIPMAP_LINEAR || param == GL_LINEAR_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR)) {
            glTexParameterf(GL_TEXTURE_2D, kGlTextureMaxAnisotropyExt, applied_anisotropy_);
        }
    });
    fn("glTexParameteriv", [](CpuContext& c) { glTexParameteriv(c.GetW(0), c.GetW(1), CPtr<GLint>(c.GetX(2))); });
    fn("glTexSubImage2D", [](CpuContext& c) {
        glTexSubImage2D(c.GetW(0), static_cast<GLint>(c.GetW(1)),
                        static_cast<GLint>(c.GetW(2)), static_cast<GLint>(c.GetW(3)),
                        static_cast<GLsizei>(c.GetW(4)), static_cast<GLsizei>(c.GetW(5)),
                        c.GetW(6), c.GetW(7), CPtr<void>(c.ReadStackU64(0)));
    });

    // ---- Uniforms ----
    fn("glUniform1f", [](CpuContext& c) { glUniform1f(static_cast<GLint>(c.GetW(0)), c.GetS(0)); });
    fn("glUniform1fv", [](CpuContext& c) { glUniform1fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLfloat>(c.GetX(2))); });
    fn("glUniform1i", [](CpuContext& c) { glUniform1i(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1))); });
    fn("glUniform1iv", [](CpuContext& c) { glUniform1iv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLint>(c.GetX(2))); });
    fn("glUniform2f", [](CpuContext& c) { glUniform2f(static_cast<GLint>(c.GetW(0)), c.GetS(0), c.GetS(1)); });
    fn("glUniform2fv", [](CpuContext& c) { glUniform2fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLfloat>(c.GetX(2))); });
    fn("glUniform2i", [](CpuContext& c) { glUniform2i(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)), static_cast<GLint>(c.GetW(2))); });
    fn("glUniform2iv", [](CpuContext& c) { glUniform2iv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLint>(c.GetX(2))); });
    fn("glUniform3f", [](CpuContext& c) { glUniform3f(static_cast<GLint>(c.GetW(0)), c.GetS(0), c.GetS(1), c.GetS(2)); });
    fn("glUniform3fv", [](CpuContext& c) { glUniform3fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLfloat>(c.GetX(2))); });
    fn("glUniform3i", [](CpuContext& c) { glUniform3i(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)), static_cast<GLint>(c.GetW(2)), static_cast<GLint>(c.GetW(3))); });
    fn("glUniform3iv", [](CpuContext& c) { glUniform3iv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLint>(c.GetX(2))); });
    fn("glUniform4f", [](CpuContext& c) { glUniform4f(static_cast<GLint>(c.GetW(0)), c.GetS(0), c.GetS(1), c.GetS(2), c.GetS(3)); });
    fn("glUniform4fv", [](CpuContext& c) { glUniform4fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLfloat>(c.GetX(2))); });
    fn("glUniform4i", [](CpuContext& c) {
        glUniform4i(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)),
                    static_cast<GLint>(c.GetW(2)), static_cast<GLint>(c.GetW(3)), static_cast<GLint>(c.GetW(4)));
    });
    fn("glUniform4iv", [](CpuContext& c) { glUniform4iv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)), CPtr<GLint>(c.GetX(2))); });
    fn("glUniformMatrix2fv", [](CpuContext& c) {
        glUniformMatrix2fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)),
                           static_cast<GLboolean>(c.GetW(2)), CPtr<GLfloat>(c.GetX(3)));
    });
    fn("glUniformMatrix3fv", [](CpuContext& c) {
        glUniformMatrix3fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)),
                           static_cast<GLboolean>(c.GetW(2)), CPtr<GLfloat>(c.GetX(3)));
    });
    fn("glUniformMatrix4fv", [](CpuContext& c) {
        glUniformMatrix4fv(static_cast<GLint>(c.GetW(0)), static_cast<GLsizei>(c.GetW(1)),
                           static_cast<GLboolean>(c.GetW(2)), CPtr<GLfloat>(c.GetX(3)));
    });

    fn("glUseProgram", [](CpuContext& c) { glUseProgram(c.GetW(0)); });
    fn("glValidateProgram", [](CpuContext& c) { glValidateProgram(c.GetW(0)); });
    fn("glVertexAttrib1f", [](CpuContext& c) { glVertexAttrib1f(c.GetW(0), c.GetS(0)); });
    fn("glVertexAttrib1fv", [](CpuContext& c) { glVertexAttrib1fv(c.GetW(0), CPtr<GLfloat>(c.GetX(1))); });
    fn("glVertexAttrib2f", [](CpuContext& c) { glVertexAttrib2f(c.GetW(0), c.GetS(0), c.GetS(1)); });
    fn("glVertexAttrib2fv", [](CpuContext& c) { glVertexAttrib2fv(c.GetW(0), CPtr<GLfloat>(c.GetX(1))); });
    fn("glVertexAttrib3f", [](CpuContext& c) { glVertexAttrib3f(c.GetW(0), c.GetS(0), c.GetS(1), c.GetS(2)); });
    fn("glVertexAttrib3fv", [](CpuContext& c) { glVertexAttrib3fv(c.GetW(0), CPtr<GLfloat>(c.GetX(1))); });
    fn("glVertexAttrib4f", [](CpuContext& c) { glVertexAttrib4f(c.GetW(0), c.GetS(0), c.GetS(1), c.GetS(2), c.GetS(3)); });
    fn("glVertexAttrib4fv", [](CpuContext& c) { glVertexAttrib4fv(c.GetW(0), CPtr<GLfloat>(c.GetX(1))); });
    fn("glVertexAttribPointer", [](CpuContext& c) {
        glVertexAttribPointer(c.GetW(0), static_cast<GLint>(c.GetW(1)), c.GetW(2),
                              static_cast<GLboolean>(c.GetW(3)), static_cast<GLsizei>(c.GetW(4)),
                              CPtr<void>(c.GetX(5)));
    });
    fn("glViewport", [](CpuContext& c) {
        glViewport(static_cast<GLint>(c.GetW(0)), static_cast<GLint>(c.GetW(1)),
                   static_cast<GLsizei>(c.GetW(2)), static_cast<GLsizei>(c.GetW(3)));
    });
}
