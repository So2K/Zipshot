// Production prepared-PNG regression; clipboard API is replaced by the same
// synthetic boundary used by image_native.cpp, never the host clipboard.
#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain

static int assertions = 0, failures = 0;
static void Check(bool ok, const char* label) {
    ++assertions; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures;
}
static std::vector<char> Read(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::wstring root = std::filesystem::absolute(argv[1]).wstring();
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto image = std::make_shared<Image>(bench::Fixture(128, 80, L"alpha"));
    const auto expected = *image;
    const std::wstring baseline = root + L"\\baseline.png";
    ComPtr<IStream> referenceStream; referenceStream.Attach(SHCreateMemStream(nullptr, 0));
    const bool referenceOk = Encode(*image, referenceStream.Get(), false);
    const auto referenceBytes = bench::Bytes(referenceStream.Get());
    bench::Write(baseline, referenceBytes.data(), referenceBytes.size());
    Check(referenceOk, "baseline WIC PNG encodes");
    auto prepared = PreparePngWic(image);
    Check(prepared != nullptr, "valid immutable source creates prepared PNG packet");
    const int beforeEmpty = clipboard_probe::empties, beforeClose = clipboard_probe::closes;
    bool predicateInsideLock = false;
    CopyPreparedImageToClipboard(prepared, [&] {
        predicateInsideLock = clipboard_probe::isOpen && clipboard_probe::empties == beforeEmpty;
        return false;
    });
    Check(predicateInsideLock && clipboard_probe::empties == beforeEmpty &&
          clipboard_probe::handles.empty() && clipboard_probe::closes == beforeClose + 1,
          "stale predicate runs inside clipboard lock before empty and leaves contents untouched");
    CopyPreparedImageToClipboard(prepared, []() -> bool { throw 1; });
    Check(!clipboard_probe::isOpen && clipboard_probe::empties == beforeEmpty,
          "throwing freshness predicate releases lock without changing clipboard");
    std::weak_ptr<const Image> lifetime = image;
    g_jpegOverKB = 1;
    std::wstring saved = root + L"\\prepared.png";
    CopyPreparedImageToClipboard(prepared);
    Check(clipboard_probe::handles.size() == 2, "prepared alpha copy produces DIB and PNG formats");
    Check(SavePreparedImageAuto(prepared, saved) && saved.ends_with(L".png"), "alpha packet preserves PNG even above automatic JPEG threshold");
    const auto bytes = Read(baseline), actual = Read(saved);
    Check(bytes == actual, "prepared disk file is byte-identical to ordinary WIC output");
    HGLOBAL clipPng = clipboard_probe::handles.back().second;
    Check(bench::Hash(GlobalLock(clipPng), bytes.size()) == bench::Hash(bytes.data(), bytes.size()),
          "prepared clipboard PNG equals original encoded bytes");
    GlobalUnlock(clipPng); clipboard_probe::Clear();
    Image decoded;
    Check(LoadImageFile(saved, decoded) && decoded.w == expected.w && decoded.h == expected.h &&
          decoded.alpha == expected.alpha && decoded.px == expected.px, "prepared PNG preserves every straight-alpha pixel and image flag");
    image->w = 1;
    std::wstring invalid = root + L"\\invalid.png";
    CopyPreparedImageToClipboard(prepared);
    Check(!SavePreparedImageAuto(prepared, invalid) && clipboard_probe::handles.empty() &&
          !std::filesystem::exists(invalid), "mutated dimensions reject stale prepared packet without clipboard/file output");
    image->w = expected.w;
    image.reset();
    Check(!lifetime.expired(), "packet retains its source while capture worker releases its pointer");
    prepared.reset();
    Check(lifetime.expired(), "source is released with final packet owner");
    Check(!PreparePngWic({}) && !SavePreparedImageAuto({}, invalid), "null packet and null source fail safely");
    auto opaque = std::make_shared<Image>(bench::Fixture(256, 256, L"entropy"));
    auto opaquePng = PreparePngWic(opaque);
    std::wstring oldJpeg = root + L"\\ordinary-threshold.png", newJpeg = root + L"\\prepared-threshold.png";
    Check(SaveImageAuto(*opaque, oldJpeg) && SavePreparedImageAuto(opaquePng, newJpeg) &&
          oldJpeg.ends_with(L".jpg") && newJpeg.ends_with(L".jpg") && Read(oldJpeg) == Read(newJpeg),
          "opaque prepared WIC PNG preserves automatic-JPEG branch and exact JPEG bytes");
    std::wstring collision = saved;
    Check(SavePreparedImageAuto(PreparePngWic(std::make_shared<Image>(expected)), collision) && collision != saved &&
          Read(saved) == bytes && Read(collision) == bytes, "prepared save keeps atomic no-overwrite collision behavior");
    CoUninitialize();
    std::printf("%d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
