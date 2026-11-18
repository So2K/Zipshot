// Documentation-only harness. Render the real production editor on public source text.
// Build instead of src/editor.cpp; the installed/release app is never modified.
#include "../src/common.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

Microsoft::WRL::ComPtr<IWICBitmap> g_readmeBitmap;
HRESULT CreateReadmeRenderTarget(ID2D1Factory* factory, HWND window, ID2D1RenderTarget** target) {
    RECT client{};
    GetClientRect(window, &client);
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return hr;
    g_readmeBitmap.Reset();
    hr = wic->CreateBitmap(client.right, client.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &g_readmeBitmap);
    if (FAILED(hr)) return hr;
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    return factory->CreateWicBitmapRenderTarget(g_readmeBitmap.Get(), properties, target);
}

#define EditorDemoTest LegacyEditorDemoTest
#include "../build/readme-native/editor_readme.inc"
#undef EditorDemoTest

bool EditorDemoTest(const std::wstring& in, const std::wstring& directory) {
    auto image = std::make_shared<Image>();
    if (!LoadImageFile(in, *image)) return false;
    CreateDirectoryW(directory.c_str(), nullptr);
    g_quietEditor = true;
    OpenEditor(image, in);
    HWND window = OwnEditorWindow();
    if (!window) return false;
    SetWindowPos(window, nullptr, -12000, -12000, 1500, 940, SWP_NOZORDER | SWP_NOACTIVATE);
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    editor->ui = 1.25f;
    editor->tool = T_Select;
    editor->LayoutAll();
    editor->o.border = Rgb(58, 141, 255);
    editor->o.borderSize = 3;
    editor->o.shadow = 0;
    auto capture = [&](int step) {
        editor->LayoutToolbar();
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        RECT rect{};
        GetWindowRect(window, &rect);
        Image frame;
        frame.w = rect.right - rect.left;
        frame.h = rect.bottom - rect.top;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDib(frame.w, frame.h, &pixels);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!bitmap || !pixels || !dc) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return false; }
        HGDIOBJ old = SelectObject(dc, bitmap);
        bool ok = PrintWindow(window, dc, PW_RENDERFULLCONTENT) != 0;
        GdiFlush();
        frame.px.assign((uint32_t*)pixels, (uint32_t*)pixels + (size_t)frame.w * frame.h);
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bitmap);
        // Capture the real Win32 non-client frame, then read the actual production
        // Direct2D client renderer from its offscreen WIC target.
        editor->Render();
        if (!g_readmeBitmap) return false;
        UINT width = 0, height = 0;
        g_readmeBitmap->GetSize(&width, &height);
        std::vector<uint32_t> client((size_t)width * height);
        if (FAILED(g_readmeBitmap->CopyPixels(nullptr, width * 4, (UINT)(client.size() * 4), (BYTE*)client.data()))) return false;
        POINT origin{};
        ClientToScreen(window, &origin);
        int x = origin.x - rect.left, y = origin.y - rect.top;
        if (x < 0 || y < 0 || x + (int)width > frame.w || y + (int)height > frame.h) return false;
        for (UINT row = 0; row < height; ++row)
            std::copy_n(client.data() + (size_t)row * width, width, frame.px.data() + (size_t)(y + row) * frame.w + x);
        return ok && EncodeImage(frame, directory + L"\\editor-" + std::to_wstring(step) + L".png", false);
    };
    auto add = [&](Tool tool, D2D1_POINT_2F from, D2D1_POINT_2F to) -> Shape& {
        editor->tool = tool;
        Shape shape = editor->NewShape(from);
        shape.pts = {from, to};
        editor->shapes.push_back(std::move(shape));
        editor->selected = (int)editor->shapes.size() - 1;
        return editor->shapes.back();
    };
    bool ok = capture(0);
    add(T_Rect, {145, 430}, {460, 462});
    editor->tool = T_Select;
    editor->selected = -1;
    ok = capture(1) && ok;
    add(T_Arrow, {1010, 365}, {480, 439});
    editor->tool = T_Select;
    editor->selected = -1;
    ok = capture(2) && ok;
    editor->o.textFont = 17;
    editor->o.textFill = Rgb(48, 92, 160);
    Shape& note = add(T_TextBackground, {825, 480}, {1170, 540});
    note.text = L"Keep the alpha channel";
    Editor::UpdateTextBox(note);
    editor->tool = T_Select;
    editor->selected = -1;
    ok = capture(3) && ok;
    ok = EncodeImage(editor->Export(), directory + L"\\result.png", false) && ok;
    editor->dirty = false;
    DestroyWindow(window);
    g_quietEditor = false;
    return ok;
}
