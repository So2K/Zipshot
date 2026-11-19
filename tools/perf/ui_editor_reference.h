#pragma once
// Verbatim pre-optimization component bodies from1fdf0a7. Only reference names changed.
void LegacyBoxBlurPass(std::vector<uint32_t>& src, std::vector<uint32_t>& dst, int w, int h, int r, bool horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w;
    size_t step = horiz ? 1 : w;
    for (int l = 0; l < lines; ++l) {
        size_t base = horiz ? (size_t)l * w : l;
        int sr = 0, sg = 0, sb = 0;
        auto at = [&](int i) { return src[base + step * std::clamp(i, 0, n - 1)]; };
        for (int i = -r; i <= r; ++i) {
            uint32_t p = at(i);
            sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255;
        }
        int div = 2 * r + 1;
        for (int i = 0; i < n; ++i) {
            dst[base + step * i] = 0xFF000000u | (uint32_t)(sr / div) << 16 | (uint32_t)(sg / div) << 8 | (uint32_t)(sb / div);
            uint32_t a = at(i + r + 1), b = at(i - r);
            sr += (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
            sg += (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
            sb += (int)(a & 255) - (int)(b & 255);
        }
    }
}


struct LegacyEditor : Editor {
    const Image& FxImage(int kind, uint32_t param) {
        uint64_t key = (uint64_t)kind << 32 | param;
        auto it = fxCache.find(key);
        if (it != fxCache.end()) return it->second;
        Image& out = fxCache[key];
        if (kind == FX_Pixel) {
            int b = std::max<int>(2, param);
            int pw = (img->w + b - 1) / b, ph = (img->h + b - 1) / b;
            out.w = pw;
            out.h = ph;
            out.px.resize((size_t)pw * ph);
            for (int by = 0; by < ph; ++by)
                for (int bx = 0; bx < pw; ++bx) {
                    uint32_t r = 0, gg = 0, bb = 0, n = 0;
                    for (int y = by * b; y < std::min(img->h, (by + 1) * b); ++y)
                        for (int x = bx * b; x < std::min(img->w, (bx + 1) * b); ++x) {
                            uint32_t p = img->px[(size_t)y * img->w + x];
                            r += p >> 16 & 255; gg += p >> 8 & 255; bb += p & 255; ++n;
                        }
                    out.px[(size_t)by * pw + bx] = 0xFF000000u | (r / n) << 16 | (gg / n) << 8 | (bb / n);
                }
        } else if (kind == FX_Blur) {
            out = *img;
            std::vector<uint32_t> tmp(out.px.size());
            int r = std::max<int>(1, param / 3);
            for (int i = 0; i < 3; ++i) {
                LegacyBoxBlurPass(out.px, tmp, out.w, out.h, r, true);
                LegacyBoxBlurPass(tmp, out.px, out.w, out.h, r, false);
            }
        } else {  // highlight: multiply by the colour, like ShareX
            out = *img;
            uint32_t hr = param >> 16 & 255, hg = param >> 8 & 255, hb = param & 255;
            for (auto& p : out.px)
                p = 0xFF000000u | ((p >> 16 & 255) * hr / 255) << 16 | ((p >> 8 & 255) * hg / 255) << 8 | ((p & 255) * hb / 255);
        }
        return out;
    }

    Image Export(const D2D1_RECT_F* area = nullptr, ID2D1Geometry* mask = nullptr) {
        Image out;
        D2D1_RECT_F c = area ? *area : Crop();
        int w = (int)(c.right - c.left), h = (int)(c.bottom - c.top);
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return out;
        ComPtr<IWICBitmap> bmp;
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return out;
        ComPtr<ID2D1RenderTarget> t;
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), p, &t))) return out;
        Gfx gx = MakeGfx(t.Get(), *img);
        t->BeginDraw();
        bool transparent = mask || img->alpha;
        t->Clear(transparent ? kClear : kWhite);
        t->SetTransform(D2D1::Matrix3x2F::Translation(-c.left, -c.top));
        if (mask) t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask), nullptr);
        DrawDocument(t.Get(), gx, false);
        if (mask) t->PopLayer();
        t->EndDraw();
        out.w = w;
        out.h = h;
        out.alpha = transparent;
        out.px.resize((size_t)w * h);
        bmp->CopyPixels(nullptr, w * 4, (UINT)(out.px.size() * 4), (BYTE*)out.px.data());
        if (!transparent) {
            for (auto& px : out.px) px |= 0xFF000000u;
        } else {
            for (auto& px : out.px) {  // premultiplied -> straight alpha
                uint32_t a = px >> 24;
                if (a == 0 || a == 255) continue;
                px = a << 24 | std::min(255u, (px >> 16 & 255) * 255 / a) << 16 | std::min(255u, (px >> 8 & 255) * 255 / a) << 8 |
                     std::min(255u, (px & 255) * 255 / a);
            }
            if (!mask) out.alpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
        }
        return out;
    }

};
