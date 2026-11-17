#include "imageops.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <thread>

// CPU implementations of ShareX's editor image operations and the most used ImageEffectsLib effects.
// Formulas follow ShareX (ColorMatrixManager, ImageHelpers); GDI+ drawing is replaced by plain loops,
// WIC (resampling) and Direct2D on a WIC bitmap (text watermark).

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;

inline uint32_t Ch(uint32_t p, int s) { return p >> s & 255; }
inline uint32_t Pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
inline uint32_t Clamp255(float v) { return v <= 0 ? 0u : v >= 255 ? 255u : (uint32_t)(v + .5f); }

uint32_t Premul(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 255) return p;
    if (a == 0) return 0;
    return Pack(a, (Ch(p, 16) * a + 127) / 255, (Ch(p, 8) * a + 127) / 255, (Ch(p, 0) * a + 127) / 255);
}
uint32_t Unpremul(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 255 || a == 0) return a ? p : 0;
    return Pack(a, std::min(255u, (Ch(p, 16) * 255 + a / 2) / a), std::min(255u, (Ch(p, 8) * 255 + a / 2) / a),
                std::min(255u, (Ch(p, 0) * 255 + a / 2) / a));
}
// Straight-alpha "source over".
uint32_t Over(uint32_t d, uint32_t s) {
    uint32_t sa = s >> 24;
    if (sa == 255) return s;
    if (sa == 0) return d;
    uint32_t da = d >> 24;
    float fs = sa / 255.f, fd = da / 255.f * (1 - fs), oa = fs + fd;
    if (oa <= 0) return 0;
    auto mix = [&](int sh) { return Clamp255((Ch(s, sh) * fs + Ch(d, sh) * fd) / oa); };
    return Pack(Clamp255(oa * 255), mix(16), mix(8), mix(0));
}

// Splits rows over the cores for big images; small ones stay on the calling thread.
template <class Fn>
void ParallelRows(int h, size_t pixels, Fn fn) {
    unsigned n = std::min(16u, std::thread::hardware_concurrency());
    if (pixels < 300000 || n < 2 || h < 32) {
        fn(0, h);
        return;
    }
    std::vector<std::thread> th;
    int chunk = (h + (int)n - 1) / (int)n;
    for (unsigned i = 0; i < n; ++i) {
        int a = (int)i * chunk, b = std::min(h, a + chunk);
        if (a >= b) break;
        th.emplace_back([=, &fn] { fn(a, b); });
    }
    for (auto& t : th) t.join();
}

Image Blank(int w, int h, uint32_t argb = 0) {
    Image o;
    if (w < 1 || h < 1) return o;
    o.w = w;
    o.h = h;
    o.px.assign((size_t)w * h, argb);
    return o;
}

// A working copy with real alpha bytes (opaque documents may carry junk in the alpha byte).
Image Normalized(const Image& src) {
    Image o = src;
    if (!src.alpha)
        for (auto& p : o.px) p |= 0xFF000000u;
    return o;
}

void Finish(Image& im) {
    im.alpha = false;
    for (uint32_t p : im.px)
        if ((p >> 24) != 255) {
            im.alpha = true;
            break;
        }
}

// Running-sum box blur over premultiplied pixels (4 channels), edges clamped; r = half width.
void BoxPass(const uint32_t* src, uint32_t* dst, int w, int h, int r, bool horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w;
    size_t step = horiz ? 1 : (size_t)w;
    ParallelRows(lines, (size_t)w * h, [&](int l0, int l1) {
        for (int l = l0; l < l1; ++l) {
            size_t base = horiz ? (size_t)l * w : (size_t)l;
            uint32_t s[4] = {};
            auto at = [&](int i) { return src[base + step * std::clamp(i, 0, n - 1)]; };
            for (int i = -r; i <= r; ++i) {
                uint32_t p = at(i);
                for (int c = 0; c < 4; ++c) s[c] += p >> (c * 8) & 255;
            }
            uint32_t div = 2 * r + 1;
            for (int i = 0; i < n; ++i) {
                uint32_t o = 0;
                for (int c = 0; c < 4; ++c) o |= ((s[c] + div / 2) / div) << (c * 8);
                dst[base + step * i] = o;
                uint32_t a = at(i + r + 1), b = at(i - r);
                for (int c = 0; c < 4; ++c) s[c] += (a >> (c * 8) & 255) - (b >> (c * 8) & 255);
            }
        }
    });
}

void BoxBlur(std::vector<uint32_t>& pm, int w, int h, int r, int passes) {
    if (r < 1 || w < 1 || h < 1) return;
    std::vector<uint32_t> tmp(pm.size());
    for (int i = 0; i < passes; ++i) {
        BoxPass(pm.data(), tmp.data(), w, h, r, true);
        BoxPass(tmp.data(), pm.data(), w, h, r, false);
    }
}

// GDI+ ColorMatrix (row-vector convention: [r g b a 1] x M), applied to straight colours.
struct Matrix {
    float m[5][5]{};
};
Image ApplyMatrix(const Image& src, const Matrix& M) {
    Image o = Normalized(src);
    ParallelRows(o.h, o.px.size(), [&](int y0, int y1) {
        for (size_t i = (size_t)y0 * o.w; i < (size_t)y1 * o.w; ++i) {
            uint32_t p = o.px[i];
            float r = Ch(p, 16) / 255.f, g = Ch(p, 8) / 255.f, b = Ch(p, 0) / 255.f, a = Ch(p, 24) / 255.f;
            float v[4];
            for (int c = 0; c < 4; ++c) v[c] = r * M.m[0][c] + g * M.m[1][c] + b * M.m[2][c] + a * M.m[3][c] + M.m[4][c];
            o.px[i] = Pack(Clamp255(v[3] * 255), Clamp255(v[0] * 255), Clamp255(v[1] * 255), Clamp255(v[2] * 255));
        }
    });
    Finish(o);
    return o;
}
Matrix Identity() {
    Matrix M;
    for (int i = 0; i < 5; ++i) M.m[i][i] = 1;
    return M;
}
constexpr float rw = 0.212671f, gw = 0.715160f, bw = 0.072169f;  // ShareX luminance weights

Image Gamma(const Image& src, float gamma) {
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint8_t)Clamp255(std::pow(i / 255.f, gamma) * 255);
    Image o = Normalized(src);
    for (auto& p : o.px) p = (p & 0xFF000000u) | (uint32_t)lut[Ch(p, 16)] << 16 | (uint32_t)lut[Ch(p, 8)] << 8 | lut[Ch(p, 0)];
    o.alpha = src.alpha;
    return o;
}

Image Blur(const Image& src, int radius) {
    // ShareX Blur: odd radius >= 3, one box pass each way over `radius` pixels.
    radius = std::max(3, radius | 1);
    Image o = Normalized(src);
    for (auto& p : o.px) p = Premul(p);
    BoxBlur(o.px, o.w, o.h, radius / 2, 1);
    for (auto& p : o.px) p = Unpremul(p);
    Finish(o);
    return o;
}

Image Sharpen(const Image& src) {
    // ConvolutionMatrixManager.Sharpen(11): centre 11/3, cross -2/3.
    Image in = Normalized(src), o = in;
    const float k = 11.f / 3, e = -2.f / 3;
    int w = in.w, h = in.h;
    ParallelRows(h, in.px.size(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                auto P = [&](int xx, int yy) { return in.px[(size_t)std::clamp(yy, 0, h - 1) * w + std::clamp(xx, 0, w - 1)]; };
                uint32_t c = P(x, y), l = P(x - 1, y), r = P(x + 1, y), t = P(x, y - 1), b = P(x, y + 1);
                auto f = [&](int s) { return Clamp255(Ch(c, s) * k + (Ch(l, s) + Ch(r, s) + Ch(t, s) + Ch(b, s)) * e); };
                o.px[(size_t)y * w + x] = (c & 0xFF000000u) | f(16) << 16 | f(8) << 8 | f(0);
            }
    });
    o.alpha = src.alpha;
    return o;
}

Image Pixelate(const Image& src, int size, int borderSize, uint32_t borderColor) {
    size = std::max(2, size);
    Image o = Normalized(src);
    int w = o.w, h = o.h;
    for (int by = 0; by < h; by += size)
        for (int bx = 0; bx < w; bx += size) {
            uint64_t s[4] = {};
            int x1 = std::min(w, bx + size), y1 = std::min(h, by + size), n = (x1 - bx) * (y1 - by);
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x) {
                    uint32_t p = Premul(o.px[(size_t)y * w + x]);
                    for (int c = 0; c < 4; ++c) s[c] += p >> (c * 8) & 255;
                }
            uint32_t avg = 0;
            for (int c = 0; c < 4; ++c) avg |= (uint32_t)((s[c] + n / 2) / n) << (c * 8);
            avg = Unpremul(avg);
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x) {
                    bool edge = borderSize > 0 && (x - bx < borderSize || y - by < borderSize);
                    o.px[(size_t)y * w + x] = edge ? Over(avg, borderColor) : avg;
                }
        }
    Finish(o);
    return o;
}

// GDI+ dash patterns in pen widths: Solid, Dash (3,1), Dot (1,1), DashDot (3,1,1,1).
bool DashOn(int style, float pos) {
    if (style <= 0) return true;
    static const float pat[4][4] = {{}, {3, 1, 0, 0}, {1, 1, 0, 0}, {3, 1, 1, 1}};
    const float* p = pat[std::min(style, 3)];
    float len = p[0] + p[1] + p[2] + p[3];
    float t = std::fmod(pos, len);
    if (t < p[0]) return true;
    if (t < p[0] + p[1]) return false;
    return t < p[0] + p[1] + p[2];
}

Image Border(const Image& src, bool inside, int size, int dash, uint32_t color) {
    size = std::max(1, size);
    Image in = Normalized(src);
    Image o = inside ? in : Blank(in.w + size * 2, in.h + size * 2);
    if (!inside)
        for (int y = 0; y < in.h; ++y)
            memcpy(&o.px[(size_t)(y + size) * o.w + size], &in.px[(size_t)y * in.w], (size_t)in.w * 4);
    int W = o.w, H = o.h;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            bool top = y < size, bottom = y >= H - size, left = x < size, right = x >= W - size;
            if (!top && !bottom && !left && !right) continue;
            float pos = top ? x : right ? W + y : bottom ? W + H + (W - x) : 2.f * W + H + (H - y);
            if (!DashOn(dash, pos / size)) continue;
            uint32_t& p = o.px[(size_t)y * W + x];
            p = Over(p, color);
        }
    Finish(o);
    return o;
}

Image Shadow(const Image& src, float opacity, int size, float darkness, uint32_t color, int ox, int oy, bool autoResize) {
    opacity = std::clamp(opacity, .1f, 1.f);
    size = std::max(0, size);
    Image in = Normalized(src);
    int sw = in.w + size * 2, sh = in.h + size * 2;
    std::vector<uint32_t> mask((size_t)sw * sh, 0);
    float ca = (color >> 24) / 255.f * opacity;
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) mask[(size_t)(y + size) * sw + x + size] = Clamp255((in.px[(size_t)y * in.w + x] >> 24) * ca) << 24;
    // ShareX blurs once with a box `size` wide; three narrower passes give the same reach without the hard box edge.
    if (size > 0) BoxBlur(mask, sw, sh, std::max(1, (int)std::lround(size * 0.29)), 3);
    if (darkness > 1)
        for (auto& m : mask) m = Clamp255((m >> 24) * darkness) << 24;
    uint32_t rgb = color & 0xFFFFFFu;
    Image o;
    int sx, sy, ix, iy;
    if (autoResize) {
        o = Blank(sw + std::abs(ox), sh + std::abs(oy));
        sx = std::max(0, ox), sy = std::max(0, oy);
        ix = std::max(size, -ox + size), iy = std::max(size, -oy + size);
    } else {
        o = Blank(in.w, in.h);
        sx = -size + ox, sy = -size + oy, ix = iy = 0;
    }
    for (int y = 0; y < sh; ++y) {
        int dy = y + sy;
        if (dy < 0 || dy >= o.h) continue;
        for (int x = 0; x < sw; ++x) {
            int dx = x + sx;
            if (dx < 0 || dx >= o.w) continue;
            o.px[(size_t)dy * o.w + dx] = mask[(size_t)y * sw + x] | rgb;
        }
    }
    for (int y = 0; y < in.h; ++y) {
        int dy = y + iy;
        if (dy < 0 || dy >= o.h) continue;
        for (int x = 0; x < in.w; ++x) {
            int dx = x + ix;
            if (dx < 0 || dx >= o.w) continue;
            uint32_t& d = o.px[(size_t)dy * o.w + dx];
            d = Over(d, in.px[(size_t)y * in.w + x]);
        }
    }
    Finish(o);
    return o;
}

Image RoundedCorners(const Image& src, int radius) {
    Image o = Normalized(src);
    float r = (float)std::min({radius, o.w / 2, o.h / 2});
    if (r <= 0) return o;
    int ri = (int)std::ceil(r);
    for (int y = 0; y < ri; ++y)
        for (int x = 0; x < ri; ++x) {
            float dx = r - (x + .5f), dy = r - (y + .5f);
            float cover = std::clamp(r - std::sqrt(dx * dx + dy * dy) + .5f, 0.f, 1.f);
            if (cover >= 1) continue;
            int xs[2] = {x, o.w - 1 - x}, ys[2] = {y, o.h - 1 - y};
            for (int yy : ys)
                for (int xx : xs) {
                    uint32_t& p = o.px[(size_t)yy * o.w + xx];
                    p = (p & 0xFFFFFFu) | Clamp255((p >> 24) * cover) << 24;
                }
        }
    Finish(o);
    return o;
}

Image Outline(const Image& src, int size, int padding, uint32_t color, bool outlineOnly) {
    // ShareX MakeOutline: distance to the nearest fully opaque pixel within (padding, padding + size + 1).
    size = std::max(1, size);
    padding = std::max(0, padding);
    Image in = Normalized(src);
    int minR = padding, maxR = padding + size + 1, w = in.w, h = in.h;
    struct Off { int dx, dy; float d; };
    std::vector<Off> offs;
    for (int dy = -maxR; dy <= maxR; ++dy)
        for (int dx = -maxR; dx <= maxR; ++dx) {
            float d = std::sqrt((float)(dx * dx + dy * dy));
            if (d <= maxR) offs.push_back({dx, dy, d});
        }
    std::sort(offs.begin(), offs.end(), [](const Off& a, const Off& b) { return a.d < b.d; });
    Image line = Blank(w, h);
    ParallelRows(h, (size_t)w * h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float dist = (float)maxR;
                for (const Off& f : offs) {
                    int xx = x + f.dx, yy = y + f.dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    if ((in.px[(size_t)yy * w + xx] >> 24) == 255) {
                        dist = f.d;
                        break;
                    }
                }
                if (dist > minR && dist < maxR) {
                    float a = 1;
                    if (dist - minR < 1) a = dist - minR;
                    else if (maxR - dist < 1) a = maxR - dist;
                    line.px[(size_t)y * w + x] = (color & 0xFFFFFFu) | Clamp255(a * (color >> 24)) << 24;
                }
            }
    });
    if (outlineOnly) {
        Finish(line);
        return line;
    }
    for (size_t i = 0; i < in.px.size(); ++i) in.px[i] = Over(in.px[i], line.px[i]);
    Finish(in);
    return in;
}

Image Reflection(const Image& src, int percentage, int maxAlpha, int minAlpha, int offset, bool skew, int skewSize) {
    percentage = std::clamp(percentage, 1, 100);
    maxAlpha = std::clamp(maxAlpha, 0, 255);
    minAlpha = std::clamp(minAlpha, 0, 255);
    Image in = Normalized(src);
    int rh = std::max(1, (int)(in.h * (percentage / 100.f)));
    Image refl = Blank(in.w, rh);
    for (int y = 0; y < rh; ++y) {
        float a = (maxAlpha - (maxAlpha - minAlpha) * (rh > 1 ? (float)y / (rh - 1) : 0.f)) / 255.f;
        const uint32_t* row = &in.px[(size_t)(in.h - 1 - y) * in.w];
        for (int x = 0; x < in.w; ++x) refl.px[(size_t)y * in.w + x] = (row[x] & 0xFFFFFFu) | Clamp255((row[x] >> 24) * a) << 24;
    }
    if (skew && skewSize != 0) {  // ShareX AddSkew(x, 0): the bottom row slides `skewSize` pixels sideways
        Image sk = Blank(in.w + std::abs(skewSize), rh);
        int startX = -std::min(0, skewSize);
        for (int y = 0; y < rh; ++y) {
            float shift = startX + (rh > 1 ? (float)skewSize * y / (rh - 1) : 0.f);
            for (int x = 0; x < sk.w; ++x) {
                // linear interpolation between the two source pixels covering x
                float sxf = x - shift;
                int sx0 = (int)std::floor(sxf);
                float t = sxf - sx0;
                uint32_t a = sx0 >= 0 && sx0 < in.w ? Premul(refl.px[(size_t)y * in.w + sx0]) : 0;
                uint32_t b = sx0 + 1 >= 0 && sx0 + 1 < in.w ? Premul(refl.px[(size_t)y * in.w + sx0 + 1]) : 0;
                uint32_t o = 0;
                for (int c = 0; c < 4; ++c) o |= Clamp255((a >> (c * 8) & 255) * (1 - t) + (b >> (c * 8) & 255) * t) << (c * 8);
                sk.px[(size_t)y * sk.w + x] = Unpremul(o);
            }
        }
        refl = std::move(sk);
    }
    Image o = Blank(refl.w, std::max(1, in.h + rh + offset));
    for (int y = 0; y < in.h && y < o.h; ++y) memcpy(&o.px[(size_t)y * o.w], &in.px[(size_t)y * in.w], (size_t)in.w * 4);
    for (int y = 0; y < rh; ++y) {
        int dy = in.h + offset + y;
        if (dy < 0 || dy >= o.h) continue;
        for (int x = 0; x < refl.w; ++x) {
            uint32_t& d = o.px[(size_t)dy * o.w + x];
            d = Over(d, refl.px[(size_t)y * refl.w + x]);
        }
    }
    Finish(o);
    return o;
}

// Bilinear sample of a premultiplied image; outside is transparent.
uint32_t SamplePm(const std::vector<uint32_t>& pm, int w, int h, float x, float y) {
    x -= .5f;
    y -= .5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto P = [&](int xx, int yy) -> uint32_t { return xx < 0 || yy < 0 || xx >= w || yy >= h ? 0 : pm[(size_t)yy * w + xx]; };
    uint32_t a = P(x0, y0), b = P(x0 + 1, y0), c = P(x0, y0 + 1), d = P(x0 + 1, y0 + 1);
    uint32_t o = 0;
    for (int k = 0; k < 4; ++k) {
        int s = k * 8;
        float v = ((a >> s & 255) * (1 - fx) + (b >> s & 255) * fx) * (1 - fy) + ((c >> s & 255) * (1 - fx) + (d >> s & 255) * fx) * fy;
        o |= Clamp255(v) << s;
    }
    return o;
}

Image RotateAny(const Image& src, float angle, bool upsize, bool clip) {
    if (angle == 0) return src;
    Image in = Normalized(src);
    std::vector<uint32_t> pm(in.px.size());
    for (size_t i = 0; i < pm.size(); ++i) pm[i] = Premul(in.px[i]);
    double a = angle * kPi / 180, c = std::cos(a), s = std::sin(a);
    double bw2 = std::abs(in.w * c) + std::abs(in.h * s), bh2 = std::abs(in.w * s) + std::abs(in.h * c);
    int nw = in.w, nh = in.h;
    double k = 1;
    if (upsize) {
        nw = (int)std::ceil(bw2 - 1e-6);
        nh = (int)std::ceil(bh2 - 1e-6);
    } else if (!clip) {
        k = std::min(in.w / bw2, in.h / bh2);  // shrink to fit the original size
    }
    Image o = Blank(nw, nh);
    double cx = nw / 2.0, cy = nh / 2.0, scx = in.w / 2.0, scy = in.h / 2.0;
    ParallelRows(nh, (size_t)nw * nh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < nw; ++x) {
                double dx = (x + .5 - cx) / k, dy = (y + .5 - cy) / k;  // GDI+ positive angles turn clockwise
                double sx = c * dx + s * dy + scx, sy = -s * dx + c * dy + scy;
                o.px[(size_t)y * nw + x] = Unpremul(SamplePm(pm, in.w, in.h, (float)sx, (float)sy));
            }
    });
    Finish(o);
    return o;
}

ComPtr<IWICImagingFactory> Wic() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

const wchar_t* kFonts[] = {L"Arial", L"Segoe UI", L"Verdana", L"Tahoma", L"Consolas", L"Times New Roman", L"Impact"};

Image TextWatermark(const Image& src, const Effect& e) {
    // ShareX DrawText: rounded box with padding, border, text shadow at (-1,-1); placed by ContentAlignment.
    Image in = Normalized(src);
    const std::wstring& text = e.text;
    if (text.empty()) return in;
    int placement = std::clamp((int)e.v[1], 0, 8), offX = (int)e.v[2], offY = (int)e.v[3];
    const wchar_t* family = kFonts[std::clamp((int)e.v[4], 0, (int)std::size(kFonts) - 1)];
    float pt = (float)std::max(1.0, e.v[5]);
    uint32_t textColor = (uint32_t)e.v[6], shadowColor = (uint32_t)e.v[8], bgColor = (uint32_t)e.v[10], borderColor = (uint32_t)e.v[12];
    bool shadow = e.v[7] != 0, bg = e.v[9] != 0, border = e.v[11] != 0;
    float radius = (float)std::max(0.0, e.v[13]), pad = (float)std::max(0.0, e.v[14]);

    auto wic = Wic();
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    if (!wic || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.GetAddressOf())))
        return in;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(in.w, in.h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return in;
    {
        ComPtr<IWICBitmapLock> lock;
        WICRect all{0, 0, in.w, in.h};
        if (FAILED(bmp->Lock(&all, WICBitmapLockWrite, &lock))) return in;
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        lock->GetStride(&stride);
        lock->GetDataPointer(&size, &data);
        for (int y = 0; y < in.h; ++y) {
            auto* row = (uint32_t*)(data + (size_t)y * stride);
            for (int x = 0; x < in.w; ++x) row[x] = Premul(in.px[(size_t)y * in.w + x]);
        }
    }
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), p, &rt))) return in;
    ComPtr<IDWriteTextFormat> fmt;
    dw->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, pt * 96 / 72,
                         L"", &fmt);
    if (!fmt) return in;
    ComPtr<IDWriteTextLayout> lay;
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), 100000, 100000, &lay);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    float bw = std::ceil(m.widthIncludingTrailingWhitespace + pad * 2), bh = std::ceil(m.height + pad * 2);
    int col = placement % 3, row = placement / 3;
    float x = col == 0 ? (float)offX : col == 1 ? std::round((in.w - bw) / 2) : in.w - bw - offX;
    float y = row == 0 ? (float)offY : row == 1 ? std::round((in.h - bh) / 2) : in.h - bh - offY;
    auto C = [](uint32_t c) { return D2D1::ColorF(Ch(c, 16) / 255.f, Ch(c, 8) / 255.f, Ch(c, 0) / 255.f, Ch(c, 24) / 255.f); };
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(C(textColor), &br);
    rt->BeginDraw();
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_ROUNDED_RECT box{{x, y, x + bw, y + bh}, radius, radius};
    if (bg) {
        br->SetColor(C(bgColor));
        rt->FillRoundedRectangle(box, br.Get());
    }
    if (border) {
        br->SetColor(C(borderColor));
        D2D1_ROUNDED_RECT b2{{x + .5f, y + .5f, x + bw - .5f, y + bh - .5f}, radius, radius};
        rt->DrawRoundedRectangle(b2, br.Get(), 1);
    }
    if (shadow) {
        br->SetColor(C(shadowColor));
        rt->DrawTextLayout({x + pad - 1, y + pad - 1}, lay.Get(), br.Get());
    }
    br->SetColor(C(textColor));
    rt->DrawTextLayout({x + pad, y + pad}, lay.Get(), br.Get());
    if (FAILED(rt->EndDraw())) return in;
    Image o = Blank(in.w, in.h);
    bmp->CopyPixels(nullptr, in.w * 4, (UINT)o.px.size() * 4, (BYTE*)o.px.data());
    for (auto& q : o.px) q = Unpremul(q);
    Finish(o);
    return o;
}

// ---------- effect table ----------
constexpr uint32_t kBlack = 0xFF000000u;
const wchar_t* kPlaceRu = L"Слева сверху|Сверху по центру|Справа сверху|Слева посередине|По центру|Справа посередине|Слева снизу|Снизу по центру|Справа снизу";
const wchar_t* kPlaceEn = L"Top left|Top center|Top right|Middle left|Middle center|Middle right|Bottom left|Bottom center|Bottom right";
const wchar_t* kFontList = L"Arial|Segoe UI|Verdana|Tahoma|Consolas|Times New Roman|Impact";

const EffectDef kEffects[E_Count] = {
    {L"Brightness", L"Яркость", L"Brightness", 1, {{L"Значение", L"Value", PK_Float, 0, -1, 1, .05}}},
    {L"Contrast", L"Контраст", L"Contrast", 1, {{L"Значение", L"Value", PK_Float, 1, 0, 5, .05}}},
    {L"Saturation", L"Насыщенность", L"Saturation", 1, {{L"Значение", L"Value", PK_Float, 1, -5, 5, .05}}},
    {L"Hue", L"Оттенок", L"Hue", 1, {{L"Угол", L"Angle", PK_Float, 0, 0, 360, 5}}},
    {L"Gamma", L"Гамма", L"Gamma", 1, {{L"Значение", L"Value", PK_Float, 1, .1, 5, .05}}},
    {L"Grayscale", L"Оттенки серого", L"Grayscale", 1, {{L"Значение", L"Value", PK_Float, 1, 0, 1, .05}}},
    {L"Inverse", L"Инверсия", L"Inverse", 0, {}},
    {L"Sepia", L"Сепия", L"Sepia", 1, {{L"Значение", L"Value", PK_Float, 1, 0, 1, .05}}},
    {L"Polaroid", L"Полароид", L"Polaroid", 0, {}},
    {L"Blur", L"Размытие", L"Blur", 1, {{L"Радиус", L"Radius", PK_Int, 15, 3, 199, 2}}},
    {L"Sharpen", L"Резкость", L"Sharpen", 0, {}},
    {L"Pixelate", L"Пикселизация", L"Pixelate", 3,
     {{L"Размер", L"Size", PK_Int, 10, 2, 500, 1}, {L"Толщина сетки", L"Border size", PK_Int, 0, 0, 50, 1},
      {L"Цвет сетки", L"Border color", PK_Color, kBlack, 0, 0, 0}}},
    {L"DrawBorder", L"Рамка", L"Border", 4,
     {{L"Тип", L"Type", PK_Choice, 0, 0, 1, 1, L"Снаружи|Внутри", L"Outside|Inside"},
      {L"Толщина", L"Size", PK_Int, 1, 1, 500, 1},
      {L"Штрих", L"Dash style", PK_Choice, 0, 0, 3, 1, L"Сплошной|Штрих|Точки|Штрих-точка", L"Solid|Dash|Dot|Dash dot"},
      {L"Цвет", L"Color", PK_Color, kBlack, 0, 0, 0}}},
    {L"Shadow", L"Тень", L"Shadow", 7,
     {{L"Непрозрачность", L"Opacity", PK_Float, .6, .1, 1, .05}, {L"Размер", L"Size", PK_Int, 10, 0, 200, 1},
      {L"Темнота", L"Darkness", PK_Float, 0, 0, 10, .1}, {L"Цвет", L"Color", PK_Color, kBlack, 0, 0, 0},
      {L"Смещение X", L"Offset X", PK_Int, 0, -500, 500, 1}, {L"Смещение Y", L"Offset Y", PK_Int, 0, -500, 500, 1},
      {L"Расширять полотно", L"Auto resize", PK_Bool, 1, 0, 1, 1}}},
    {L"RoundedCorners", L"Скруглённые углы", L"Rounded corners", 1, {{L"Радиус", L"Corner radius", PK_Int, 20, 0, 2000, 1}}},
    {L"Outline", L"Контур", L"Outline", 4,
     {{L"Толщина", L"Size", PK_Int, 1, 1, 50, 1}, {L"Отступ", L"Padding", PK_Int, 0, 0, 50, 1},
      {L"Цвет", L"Color", PK_Color, kBlack, 0, 0, 0}, {L"Только контур", L"Outline only", PK_Bool, 0, 0, 1, 1}}},
    {L"Reflection", L"Отражение", L"Reflection", 6,
     {{L"Высота, %", L"Percentage", PK_Int, 20, 1, 100, 1}, {L"Макс. альфа", L"Max alpha", PK_Int, 255, 0, 255, 5},
      {L"Мин. альфа", L"Min alpha", PK_Int, 0, 0, 255, 5}, {L"Смещение", L"Offset", PK_Int, 0, -1000, 1000, 1},
      {L"Наклон", L"Skew", PK_Bool, 0, 0, 1, 1}, {L"Величина наклона", L"Skew size", PK_Int, 25, -1000, 1000, 1}}},
    {L"Canvas", L"Полотно", L"Canvas", 6,
     {{L"Слева", L"Left", PK_Int, 0, -5000, 5000, 1}, {L"Сверху", L"Top", PK_Int, 0, -5000, 5000, 1},
      {L"Справа", L"Right", PK_Int, 0, -5000, 5000, 1}, {L"Снизу", L"Bottom", PK_Int, 0, -5000, 5000, 1},
      {L"Единицы", L"Margin mode", PK_Choice, 0, 0, 1, 1, L"Пиксели|Проценты полотна", L"Absolute size|Percentage of canvas"},
      {L"Цвет", L"Color", PK_Color, 0, 0, 0, 0}}},
    {L"Resize", L"Изменить размер", L"Resize", 3,
     {{L"Ширина (0 = авто)", L"Width (0 = auto)", PK_Int, 250, 0, 30000, 10},
      {L"Высота (0 = авто)", L"Height (0 = auto)", PK_Int, 0, 0, 30000, 10},
      {L"Режим", L"Mode", PK_Choice, 0, 0, 2, 1, L"Всегда|Только если больше|Только если меньше",
       L"Resize all|Resize if bigger|Resize if smaller"}}},
    {L"Rotate", L"Поворот", L"Rotate", 3,
     {{L"Угол", L"Angle", PK_Float, 0, -360, 360, 1}, {L"Увеличить полотно", L"Upsize", PK_Bool, 1, 0, 1, 1},
      {L"Обрезать", L"Clip", PK_Bool, 0, 0, 1, 1}}},
    {L"DrawText", L"Текст (водяной знак)", L"Text watermark", 15,
     {{L"Текст", L"Text", PK_Text, 0, 0, 0, 0},
      {L"Положение", L"Placement", PK_Choice, 8, 0, 8, 1, kPlaceRu, kPlaceEn},
      {L"Смещение X", L"Offset X", PK_Int, 5, -5000, 5000, 1},
      {L"Смещение Y", L"Offset Y", PK_Int, 5, -5000, 5000, 1},
      {L"Шрифт", L"Font", PK_Choice, 0, 0, 6, 1, kFontList, kFontList},
      {L"Размер шрифта, pt", L"Font size, pt", PK_Float, 11.25, 4, 300, .75},
      {L"Цвет текста", L"Text color", PK_Color, 0xFFEBEBEB, 0, 0, 0},
      {L"Тень текста", L"Text shadow", PK_Bool, 1, 0, 1, 1},
      {L"Цвет тени", L"Shadow color", PK_Color, kBlack, 0, 0, 0},
      {L"Фон", L"Background", PK_Bool, 1, 0, 1, 1},
      {L"Цвет фона", L"Background color", PK_Color, 0xFF2A2F38, 0, 0, 0},
      {L"Рамка", L"Border", PK_Bool, 1, 0, 1, 1},
      {L"Цвет рамки", L"Border color", PK_Color, kBlack, 0, 0, 0},
      {L"Скругление", L"Corner radius", PK_Int, 4, 0, 100, 1},
      {L"Отступ", L"Padding", PK_Int, 5, 0, 100, 1}}},
};

std::wstring Num(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%g", std::round(v * 1000) / 1000);
    return b;
}

std::wstring Ini() { return ExeDir() + L"\\zipshot.ini"; }

}  // namespace

// ---------- public ----------

const wchar_t* ResamplingName(int mode) {
    switch (mode) {
    case RS_HighQualityBicubic: return Tr(L"Бикубическая (высокое качество)", L"High quality bicubic");
    case RS_Bicubic: return Tr(L"Бикубическая", L"Bicubic");
    case RS_HighQualityBilinear: return Tr(L"Билинейная (высокое качество)", L"High quality bilinear");
    case RS_Bilinear: return Tr(L"Билинейная", L"Bilinear");
    default: return Tr(L"Ближайший сосед", L"Nearest neighbor");
    }
}

Image ImgNew(int w, int h, uint32_t argb) {
    Image o = Blank(w, h, argb);
    o.alpha = (argb >> 24) != 255;
    return o;
}

Image ImgResize(const Image& src, int w, int h, int mode) {
    if (src.empty() || w < 1 || h < 1) return {};
    if (w == src.w && h == src.h) return src;
    Image in = Normalized(src);
    std::vector<uint32_t> pm(in.px.size());
    for (size_t i = 0; i < pm.size(); ++i) pm[i] = Premul(in.px[i]);
    auto f = Wic();
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICBitmapScaler> sc;
    WICBitmapInterpolationMode m = mode == RS_HighQualityBicubic   ? WICBitmapInterpolationModeHighQualityCubic
                                   : mode == RS_Bicubic            ? WICBitmapInterpolationModeCubic
                                   : mode == RS_HighQualityBilinear ? WICBitmapInterpolationModeFant
                                   : mode == RS_Bilinear           ? WICBitmapInterpolationModeLinear
                                                                   : WICBitmapInterpolationModeNearestNeighbor;
    Image o = Blank(w, h);
    if (!f || FAILED(f->CreateBitmapFromMemory(in.w, in.h, GUID_WICPixelFormat32bppPBGRA, in.w * 4, (UINT)pm.size() * 4, (BYTE*)pm.data(), &bmp)) ||
        FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(bmp.Get(), w, h, m)) ||
        FAILED(sc->CopyPixels(nullptr, w * 4, (UINT)o.px.size() * 4, (BYTE*)o.px.data())))
        return {};
    for (auto& p : o.px) p = Unpremul(p);
    Finish(o);
    return o;
}

Image ImgCanvas(const Image& src, int l, int t, int r, int b, uint32_t argb) {
    int nw = src.w + l + r, nh = src.h + t + b;
    if (nw < 1 || nh < 1) return {};
    Image in = Normalized(src);
    Image o = Blank(nw, nh, argb);
    for (int y = 0; y < in.h; ++y) {
        int dy = y + t;
        if (dy < 0 || dy >= nh) continue;
        int x0 = std::max(0, -l), x1 = std::min(in.w, nw - l);
        if (x1 > x0) memcpy(&o.px[(size_t)dy * nw + x0 + l], &in.px[(size_t)y * in.w + x0], (size_t)(x1 - x0) * 4);
    }
    Finish(o);
    return o;
}

Image ImgRotate90(const Image& src, int q) {
    q = ((q % 4) + 4) % 4;
    if (q == 0) return src;
    int W = src.w, H = src.h;
    Image o = q == 2 ? Blank(W, H) : Blank(H, W);
    o.alpha = src.alpha;
    ParallelRows(o.h, o.px.size(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < o.w; ++x) {
                int sx, sy;
                if (q == 1) sx = y, sy = H - 1 - x;           // clockwise
                else if (q == 3) sx = W - 1 - y, sy = x;      // counter clockwise
                else sx = W - 1 - x, sy = H - 1 - y;
                o.px[(size_t)y * o.w + x] = src.px[(size_t)sy * W + sx];
            }
    });
    return o;
}

Image ImgFlip(const Image& src, bool horizontal) {
    Image o = src;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            o.px[(size_t)y * src.w + x] = horizontal ? src.px[(size_t)y * src.w + (src.w - 1 - x)] : src.px[(size_t)(src.h - 1 - y) * src.w + x];
    return o;
}

Image ImgCrop(const Image& src, RECT r) {
    Image o = src.crop(r);
    o.alpha = src.alpha;
    if (o.alpha) Finish(o);
    return o;
}

RECT ImgAutoCropRect(const Image& src, int tol) {
    RECT full{0, 0, src.w, src.h};
    if (src.empty()) return full;
    Image in = Normalized(src);
    int W = in.w, H = in.h;
    auto px = [&](int x, int y) { return in.px[(size_t)y * W + x]; };
    uint32_t check = px(0, 0);
    auto differs = [&](uint32_t p) {
        if ((check >> 24) == 0) return (p >> 24) != 0;  // ShareX compares only alpha against a transparent corner
        for (int s = 0; s < 32; s += 8)
            if (std::abs((int)(p >> s & 255) - (int)(check >> s & 255)) > tol) return true;
        return false;
    };
    RECT c = full;
    bool found = false;
    for (int x = 0; x < W && !found; ++x)
        for (int y = 0; y < H; ++y)
            if (differs(px(x, y))) { c.left = x; found = true; break; }
    if (!found) return full;  // one colour: nothing to crop
    found = false;
    for (int y = 0; y < H && !found; ++y)
        for (int x = 0; x < W; ++x)
            if (differs(px(x, y))) { c.top = y; found = true; break; }
    check = px(W - 1, H - 1);
    found = false;
    for (int x = W - 1; x >= 0 && !found; --x)
        for (int y = 0; y < H; ++y)
            if (differs(px(x, y))) { c.right = x + 1; found = true; break; }
    found = false;
    for (int y = H - 1; y >= 0 && !found; --y)
        for (int x = 0; x < W; ++x)
            if (differs(px(x, y))) { c.bottom = y + 1; found = true; break; }
    if (c.right <= c.left || c.bottom <= c.top) return full;
    return c;
}

Image ImgThumbnail(const Image& src, int maxW, int maxH) {
    if (src.empty() || maxW < 1 || maxH < 1) return {};
    double k = std::min({1.0, (double)maxW / src.w, (double)maxH / src.h});
    int w = std::max(1, (int)std::lround(src.w * k)), h = std::max(1, (int)std::lround(src.h * k));
    if (w == src.w && h == src.h) return src;
    Image o = ImgResize(src, w, h, RS_HighQualityBilinear);
    return o.empty() ? src : o;
}

const EffectDef& GetEffectDef(int type) { return kEffects[std::clamp(type, 0, E_Count - 1)]; }

Effect MakeEffect(int type) {
    Effect e;
    e.type = std::clamp(type, 0, E_Count - 1);
    const EffectDef& d = kEffects[e.type];
    for (int i = 0; i < d.count; ++i) e.v[i] = d.params[i].def;
    if (e.type == E_TextWatermark) e.text = L"Text watermark";
    return e;
}

std::wstring EffectName(const Effect& e) {
    const EffectDef& d = GetEffectDef(e.type);
    return Tr(d.ru, d.en);
}

std::wstring EffectSummary(const Effect& e) {
    const double* v = e.v;
    switch (e.type) {
    case E_Hue: case E_Rotate: return Num(v[0]) + L"°";
    case E_Border: return Num(v[1]) + L"px";
    case E_Shadow: return Num(v[1]);
    case E_Reflection: return Num(v[0]) + L"%";
    case E_Canvas: return Num(v[0]) + L", " + Num(v[1]) + L", " + Num(v[2]) + L", " + Num(v[3]);
    case E_Resize: return Num(v[0]) + (v[0] > 0 ? L"px" : L"") + L", " + Num(v[1]) + (v[1] > 0 ? L"px" : L"");
    case E_TextWatermark: return e.text;
    case E_Inverse: case E_Polaroid: case E_Sharpen: return L"";
    default: return Num(v[0]);
    }
}

Image ApplyEffect(const Image& src, const Effect& e) {
    if (src.empty()) return src;
    const double* v = e.v;
    switch (e.type) {
    case E_Brightness: {
        Matrix M = Identity();
        M.m[4][0] = M.m[4][1] = M.m[4][2] = (float)v[0];
        return ApplyMatrix(src, M);
    }
    case E_Contrast: {
        Matrix M = Identity();
        M.m[0][0] = M.m[1][1] = M.m[2][2] = (float)v[0];
        return ApplyMatrix(src, M);
    }
    case E_Saturation: {
        float s = (float)v[0];
        Matrix M = Identity();
        float w3[3] = {rw, gw, bw};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M.m[r][c] = (1 - s) * w3[r] + (r == c ? s : 0);
        return ApplyMatrix(src, M);
    }
    case E_Hue: {
        float a = (float)(v[0] * kPi / 180), c = std::cos(a), s = std::sin(a);
        Matrix M = Identity();
        float m[3][3] = {{(rw + c * (1 - rw)) + s * -rw, (rw + c * -rw) + s * 0.143f, (rw + c * -rw) + s * -(1 - rw)},
                         {(gw + c * -gw) + s * -gw, (gw + c * (1 - gw)) + s * 0.14f, (gw + c * -gw) + s * gw},
                         {(bw + c * -bw) + s * (1 - bw), (bw + c * -bw) + s * -0.283f, (bw + c * (1 - bw)) + s * bw}};
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) M.m[r][k] = m[r][k];
        return ApplyMatrix(src, M);
    }
    case E_Gamma: return Gamma(src, (float)std::clamp(v[0], .1, 5.0));
    case E_Grayscale: {
        float k = (float)v[0];
        Matrix M = Identity();
        float w3[3] = {rw, gw, bw};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M.m[r][c] = w3[r] * k;
        return ApplyMatrix(src, M);
    }
    case E_Inverse: {
        Matrix M = Identity();
        M.m[0][0] = M.m[1][1] = M.m[2][2] = -1;
        M.m[4][0] = M.m[4][1] = M.m[4][2] = 1;
        return ApplyMatrix(src, M);
    }
    case E_Sepia: {
        float k = (float)v[0];
        Matrix M = Identity();
        float m[3][3] = {{.393f, .349f, .272f}, {.769f, .686f, .534f}, {.189f, .168f, .131f}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M.m[r][c] = m[r][c] * k;
        return ApplyMatrix(src, M);
    }
    case E_Polaroid: {
        Matrix M = Identity();
        float m[3][3] = {{1.438f, -.062f, -.062f}, {-.122f, 1.378f, -.122f}, {-.016f, -.016f, 1.483f}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M.m[r][c] = m[r][c];
        M.m[4][0] = -.03f, M.m[4][1] = .05f, M.m[4][2] = -.02f;
        return ApplyMatrix(src, M);
    }
    case E_Blur: return Blur(src, (int)v[0]);
    case E_Sharpen: return Sharpen(src);
    case E_Pixelate: return Pixelate(src, (int)v[0], (int)v[1], (uint32_t)v[2]);
    case E_Border: return Border(src, v[0] != 0, (int)v[1], (int)v[2], (uint32_t)v[3]);
    case E_Shadow: return Shadow(src, (float)v[0], (int)v[1], (float)v[2] + 1, (uint32_t)v[3], (int)v[4], (int)v[5], v[6] != 0);
    case E_RoundedCorners: return RoundedCorners(src, (int)v[0]);
    case E_Outline: return Outline(src, (int)v[0], (int)v[1], (uint32_t)v[2], v[3] != 0);
    case E_Reflection: return Reflection(src, (int)v[0], (int)v[1], (int)v[2], (int)v[3], v[4] != 0, (int)v[5]);
    case E_Canvas: {
        int m[4];
        for (int i = 0; i < 4; ++i) m[i] = (int)v[i];
        if (v[4] != 0)
            for (int i = 0; i < 4; ++i) m[i] = (int)std::lround(v[i] / 100.0 * (i % 2 ? src.h : src.w));
        if (!m[0] && !m[1] && !m[2] && !m[3]) return src;
        Image o = ImgCanvas(src, m[0], m[1], m[2], m[3], (uint32_t)v[5]);
        return o.empty() ? src : o;
    }
    case E_Resize: {
        int w = (int)v[0], h = (int)v[1], mode = (int)v[2];
        if (w <= 0 && h <= 0) return src;
        if (w <= 0) w = (int)std::lround((double)h / src.h * src.w);
        if (h <= 0) h = (int)std::lround((double)w / src.w * src.h);
        if ((mode == 1 && src.w <= w && src.h <= h) || (mode == 2 && src.w >= w && src.h >= h)) return src;
        Image o = ImgResize(src, std::max(1, w), std::max(1, h), RS_HighQualityBicubic);
        return o.empty() ? src : o;
    }
    case E_Rotate: return RotateAny(src, (float)v[0], v[1] != 0, v[2] != 0);
    case E_TextWatermark: return TextWatermark(src, e);
    default: return src;
    }
}

Image ApplyEffects(const Image& src, const std::vector<Effect>& stack, const std::atomic<int>* cancel, int gen) {
    Image cur = src;
    for (const Effect& e : stack) {
        if (cancel && cancel->load() != gen) return {};
        if (!e.enabled) continue;
        Image next = ApplyEffect(cur, e);
        if (!next.empty()) cur = std::move(next);
    }
    return cur;
}

std::vector<Effect> LoadEffectStack() {
    std::vector<Effect> out;
    std::wstring ini = Ini();
    int n = GetPrivateProfileIntW(L"effects", L"Count", 0, ini.c_str());
    for (int i = 1; i <= n && i <= 200; ++i) {
        wchar_t buf[4096];
        GetPrivateProfileStringW(L"effects", (L"Effect" + std::to_wstring(i)).c_str(), L"", buf, 4096, ini.c_str());
        std::wstring s = buf;
        size_t a = s.find(L'|');
        if (a == std::wstring::npos) continue;
        size_t b = s.find(L'|', a + 1), c = b == std::wstring::npos ? b : s.find(L'|', b + 1);
        std::wstring key = s.substr(0, a);
        int type = -1;
        for (int t = 0; t < E_Count; ++t)
            if (key == kEffects[t].key) type = t;
        if (type < 0) continue;
        Effect e = MakeEffect(type);
        e.enabled = s.substr(a + 1, b - a - 1) != L"0";
        if (b != std::wstring::npos) {
            std::wstring vals = s.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1);
            int k = 0;
            for (size_t p = 0; p <= vals.size() && k < kEffects[type].count;) {
                size_t q = vals.find(L';', p);
                if (q == std::wstring::npos) q = vals.size();
                if (q > p) e.v[k] = _wtof(vals.substr(p, q - p).c_str());
                ++k;
                p = q + 1;
            }
        }
        if (c != std::wstring::npos) {
            std::wstring t, raw = s.substr(c + 1);
            for (size_t p = 0; p < raw.size(); ++p) {
                if (raw[p] == L'\\' && p + 1 < raw.size()) {
                    ++p;
                    t += raw[p] == L'n' ? L'\n' : raw[p];
                } else {
                    t += raw[p];
                }
            }
            e.text = t;
        }
        out.push_back(std::move(e));
    }
    return out;
}

void SaveEffectStack(const std::vector<Effect>& stack) {
    std::wstring ini = Ini();
    WritePrivateProfileStringW(L"effects", nullptr, nullptr, ini.c_str());  // drop old entries
    WritePrivateProfileStringW(L"effects", L"Count", std::to_wstring(stack.size()).c_str(), ini.c_str());
    for (size_t i = 0; i < stack.size(); ++i) {
        const Effect& e = stack[i];
        const EffectDef& d = GetEffectDef(e.type);
        std::wstring s = d.key;
        s += e.enabled ? L"|1|" : L"|0|";
        for (int k = 0; k < d.count; ++k) {
            if (k) s += L';';
            wchar_t b[40];
            swprintf_s(b, L"%.10g", e.v[k]);
            s += b;
        }
        s += L'|';
        for (wchar_t ch : e.text) {
            if (ch == L'\n') s += L"\\n";
            else if (ch == L'\\') s += L"\\\\";
            else if (ch != L'\r') s += ch;
        }
        WritePrivateProfileStringW(L"effects", (L"Effect" + std::to_wstring(i + 1)).c_str(), s.c_str(), ini.c_str());
    }
}
