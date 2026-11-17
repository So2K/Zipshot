// Sticker packs, the sticker picker popup, My stickers and animated (GIF) sticker decoding.
#include "shapes_ext.h"
#include "sharex_import.h"
#include <wincodec.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace shapes_ext {
namespace {

IWICImagingFactory* Wic() {  // one per thread (UI and the thumbnail worker)
    thread_local ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}

bool ToPremul(IWICBitmapSource* src, int& w, int& h, std::vector<uint32_t>& px) {
    ComPtr<IWICFormatConverter> conv;
    if (!src || FAILED(Wic()->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw, uh;
    conv->GetSize(&uw, &uh);
    if (!uw || !uh || uw > 16384 || uh > 16384) return false;
    w = (int)uw;
    h = (int)uh;
    px.resize((size_t)w * h);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)(px.size() * 4), (BYTE*)px.data()));
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool HasExt(const std::wstring& p, std::initializer_list<const wchar_t*> exts) {
    const wchar_t* e = PathFindExtensionW(p.c_str());
    for (const wchar_t* x : exts)
        if (_wcsicmp(e, x) == 0) return true;
    return false;
}

bool IsStickerImage(const std::wstring& p) {
    return HasExt(p, {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".webp", L".tif", L".tiff"});
}

bool IsGif(const std::wstring& p) { return HasExt(p, {L".gif"}); }

std::wstring FileStem(const std::wstring& p) {
    std::wstring n = PathFindFileNameW(p.c_str());
    size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

std::wstring ReadUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data((size_t)std::min<LONGLONG>(size.QuadPart, 64 << 20), '\0');
    DWORD got = 0;
    if (!data.empty()) ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    if (data.size() >= 3 && (BYTE)data[0] == 0xEF && (BYTE)data[1] == 0xBB && (BYTE)data[2] == 0xBF) data.erase(0, 3);
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), nullptr, 0), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), w.data(), (int)w.size());
    return w;
}

std::wstring ProfileString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def = L"") {
    std::wstring buf(32768, L'\0');
    DWORD n = GetPrivateProfileStringW(section, key, def, buf.data(), (DWORD)buf.size(), ini.c_str());
    buf.resize(n);
    return buf;
}

int MetaInt(IWICMetadataQueryReader* r, const wchar_t* name, int def) {
    if (!r) return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    int out = def;
    if (SUCCEEDED(r->GetMetadataByName(name, &v))) {
        switch (v.vt) {
        case VT_UI1: out = v.bVal; break;
        case VT_UI2: out = v.uiVal; break;
        case VT_UI4: out = (int)v.ulVal; break;
        case VT_I4: out = v.lVal; break;
        case VT_BOOL: out = v.boolVal ? 1 : 0; break;
        default: break;
        }
    }
    PropVariantClear(&v);
    return out;
}

}  // namespace

// ---------------------------------------------------------------- animations

int Anim::FrameAt(long long ms) const {
    if (frames.size() < 2 || total <= 0) return 0;
    int t = (int)(ms % total);
    for (size_t i = 0; i < delays.size(); ++i) {
        if (t < delays[i]) return (int)i;
        t -= delays[i];
    }
    return (int)frames.size() - 1;
}

bool LoadAnimation(const std::wstring& path, Anim& out, int maxFrames) {
    out = {};
    ComPtr<IWICBitmapDecoder> dec;
    if (!Wic() || FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
        return false;
    UINT n = 0;
    if (FAILED(dec->GetFrameCount(&n)) || n < 2) return false;
    GUID fmt{};
    dec->GetContainerFormat(&fmt);
    bool gif = fmt == GUID_ContainerFormatGif;
    ComPtr<IWICMetadataQueryReader> gr;
    if (gif) dec->GetMetadataQueryReader(&gr);
    int W = gif ? MetaInt(gr.Get(), L"/logscrdesc/Width", 0) : 0, H = gif ? MetaInt(gr.Get(), L"/logscrdesc/Height", 0) : 0;
    std::vector<uint32_t> canvas, saved;
    for (UINT i = 0; i < n && (int)i < maxFrames; ++i) {
        ComPtr<IWICBitmapFrameDecode> f;
        if (FAILED(dec->GetFrame(i, &f))) break;
        int fw, fh;
        std::vector<uint32_t> px;
        if (!ToPremul(f.Get(), fw, fh, px)) break;
        ComPtr<IWICMetadataQueryReader> r;
        f->GetMetadataQueryReader(&r);
        int left = 0, top = 0, delay = 100, disposal = 0;
        if (gif) {
            left = MetaInt(r.Get(), L"/imgdesc/Left", 0);
            top = MetaInt(r.Get(), L"/imgdesc/Top", 0);
            delay = MetaInt(r.Get(), L"/grctlext/Delay", 10) * 10;
            disposal = MetaInt(r.Get(), L"/grctlext/Disposal", 0);
        }
        if (W <= 0 || H <= 0) W = fw, H = fh;
        if (canvas.empty()) canvas.assign((size_t)W * H, 0);
        if (disposal == 3) saved = canvas;
        int x0 = std::max(0, left), y0 = std::max(0, top), x1 = std::min(W, left + fw), y1 = std::min(H, top + fh);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uint32_t s = px[(size_t)(y - top) * fw + (x - left)], a = s >> 24;
                if (!a) continue;
                uint32_t& d = canvas[(size_t)y * W + x];
                if (a == 255) {
                    d = s;
                } else {  // source-over, premultiplied
                    uint32_t k = 255 - a, r2 = 0;
                    for (int sh = 0; sh < 32; sh += 8) r2 |= std::min(255u, (s >> sh & 255) + (d >> sh & 255) * k / 255) << sh;
                    d = r2;
                }
            }
        auto frame = std::make_shared<Image>();
        frame->w = W;
        frame->h = H;
        frame->px = canvas;
        frame->alpha = std::any_of(canvas.begin(), canvas.end(), [](uint32_t p) { return (p >> 24) != 255; });
        out.frames.push_back(std::move(frame));
        if (delay <= 10) delay = 100;  // browsers: 0 or 1 hundredths mean 100 ms
        out.delays.push_back(delay);
        out.total += delay;
        if (disposal == 2) {  // restore to background (transparent, as browsers do)
            for (int y = y0; y < y1; ++y) std::fill_n(&canvas[(size_t)y * W + x0], x1 - x0, 0u);
        } else if (disposal == 3 && !saved.empty()) {
            canvas = saved;
        }
    }
    return out.frames.size() >= 2;
}

std::shared_ptr<Anim> ResizeAnim(const Anim& a, int size) {
    auto r = std::make_shared<Anim>();
    r->delays = a.delays;
    r->total = a.total;
    for (auto& f : a.frames) r->frames.push_back(std::make_shared<Image>(ResizeLimit(*f, size)));
    return r;
}

// ---------------------------------------------------------------- packs

std::wstring StickersRoot() {
    wchar_t b[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ZIPSHOT_STICKERS_ROOT", b, MAX_PATH) && PathIsDirectoryW(b)) return b;
    std::wstring r = IniString(L"stickers", L"Root", L"");
    if (!r.empty() && PathIsDirectoryW(r.c_str())) return r;
    return ExeDir() + L"\\Stickers";
}

std::wstring CustomStickerFolder() {
    std::wstring d = StickersRoot() + L"\\Custom";
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}

std::wstring StickerFolder() {
    wchar_t buf[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"annotation", L"StickerFolder", L"", buf, MAX_PATH, ZipshotIni().c_str());
    std::vector<std::wstring> c;
    if (*buf) c.push_back(buf);
    c.push_back(StickersRoot() + L"\\BlobEmoji");
    c.push_back(ExeDir() + L"\\Stickers\\BlobEmoji");
    c.push_back(L"D:\\Programs\\ShareX\\Stickers\\BlobEmoji");  // the user's portable ShareX (see sharex_import.cpp)
    wchar_t pf[MAX_PATH];
    if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\ShareX\\Stickers\\BlobEmoji", pf, MAX_PATH)) c.push_back(pf);
    for (auto& d : c)
        if (PathIsDirectoryW(d.c_str())) return d;
    return L"";
}

std::vector<std::wstring> StickerFiles(const std::wstring& folder) {
    std::vector<std::wstring> v;
    if (folder.empty()) return v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsStickerImage(fd.cFileName)) v.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    return v;
}

namespace {

struct GroupInfo { const wchar_t *key, *ru, *en; };
const GroupInfo kGroups[] = {
    {L"Smileys & Emotion", L"Смайлы и эмоции", L"Smileys & emotion"},
    {L"People & Body", L"Люди и тело", L"People & body"},
    {L"Animals & Nature", L"Животные и природа", L"Animals & nature"},
    {L"Food & Drink", L"Еда и напитки", L"Food & drink"},
    {L"Travel & Places", L"Путешествия и места", L"Travel & places"},
    {L"Activities", L"Занятия", L"Activities"},
    {L"Objects", L"Предметы", L"Objects"},
    {L"Symbols", L"Символы", L"Symbols"},
    {L"Flags", L"Флаги", L"Flags"},
};

std::wstring GroupTitle(const std::wstring& key) {
    for (auto& g : kGroups)
        if (key == g.key) return Tr(g.ru, g.en);
    return key;
}

// A few Russian words for the search box; the packs' names and keywords are English (CLDR).
const std::pair<const wchar_t*, const wchar_t*> kRuWords[] = {
    {L"улыб", L"smil"}, {L"смех", L"laugh"}, {L"смеш", L"joy"}, {L"рж", L"rolling"}, {L"плач", L"cry"}, {L"слез", L"tear"},
    {L"груст", L"sad"}, {L"зл", L"angry"}, {L"злост", L"rage"}, {L"любов", L"love"}, {L"сердц", L"heart"}, {L"поцел", L"kiss"},
    {L"лиц", L"face"}, {L"глаз", L"eye"}, {L"рук", L"hand"}, {L"палец", L"thumb"}, {L"класс", L"thumbs up"}, {L"лайк", L"thumbs up"},
    {L"ок", L"ok"}, {L"хлоп", L"clap"}, {L"молит", L"pray"}, {L"мышц", L"muscle"}, {L"дума", L"think"},
    {L"шок", L"astonish"}, {L"удив", L"surprise"}, {L"страх", L"fear"}, {L"испуг", L"scream"}, {L"спат", L"sleep"},
    {L"сон", L"sleep"}, {L"больн", L"sick"}, {L"холод", L"cold"}, {L"жар", L"hot"}, {L"очк", L"glasses"}, {L"крут", L"cool"},
    {L"кот", L"cat"}, {L"кош", L"cat"}, {L"соба", L"dog"}, {L"пёс", L"dog"}, {L"пес", L"dog"}, {L"мыш", L"mouse"},
    {L"медвед", L"bear"}, {L"лис", L"fox"}, {L"панд", L"panda"}, {L"обезья", L"monkey"}, {L"птиц", L"bird"}, {L"рыб", L"fish"},
    {L"змея", L"snake"}, {L"цвет", L"flower"}, {L"дерев", L"tree"}, {L"солн", L"sun"}, {L"луна", L"moon"}, {L"звезд", L"star"},
    {L"огон", L"fire"}, {L"огн", L"fire"}, {L"вод", L"water"}, {L"дожд", L"rain"}, {L"снег", L"snow"}, {L"молни", L"lightning"},
    {L"еда", L"food"}, {L"пицц", L"pizza"}, {L"бургер", L"burger"}, {L"торт", L"cake"}, {L"кофе", L"coffee"}, {L"чай", L"tea"},
    {L"пив", L"beer"}, {L"вин", L"wine"}, {L"яблок", L"apple"}, {L"машин", L"car"}, {L"дом", L"house"}, {L"ракет", L"rocket"},
    {L"самол", L"airplane"}, {L"деньг", L"money"}, {L"час", L"clock"}, {L"телеф", L"phone"}, {L"компь", L"computer"},
    {L"подар", L"gift"}, {L"праздн", L"party"}, {L"шар", L"balloon"}, {L"череп", L"skull"}, {L"призрак", L"ghost"},
    {L"привид", L"ghost"}, {L"какаш", L"poo"}, {L"клоун", L"clown"}, {L"робот", L"robot"}, {L"инопл", L"alien"},
    {L"флаг", L"flag"}, {L"галоч", L"check"}, {L"крест", L"cross"}, {L"вопрос", L"question"}, {L"восклиц", L"exclamation"},
    {L"стрел", L"arrow"}, {L"мяч", L"ball"}, {L"музык", L"music"}, {L"книг", L"book"}, {L"ключ", L"key"}, {L"замок", L"lock"},
    {L"бомб", L"bomb"}, {L"кубок", L"trophy"}, {L"корон", L"crown"}, {L"сто", L"100"},
};

struct Item { std::wstring file, name, group, hay; };

struct Pack {
    std::wstring id, dir, title, shortName, info;
    int order = 100;
    bool custom = false, recent = false, loaded = false;
    std::vector<Item> items;
    std::vector<std::wstring> groups;
    std::vector<int> groupFirst;  // the first item of each group (category icon)
};

std::wstring IniFile() { return ZipshotIni(); }

std::vector<std::wstring> LoadRecent() {
    std::vector<std::wstring> v;
    std::wstring s = ProfileString(IniFile(), L"stickers", L"Recent"), root = StickersRoot();
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find(L'|', a);
        if (b == std::wstring::npos) b = s.size();
        std::wstring p = s.substr(a, b - a);
        if (!p.empty() && PathIsRelativeW(p.c_str())) p = root + L"\\" + p;
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) v.push_back(p);
        a = b + 1;
    }
    return v;
}

void SaveRecent(const std::vector<std::wstring>& v) {
    std::wstring s, root = Lower(StickersRoot()) + L"\\";
    for (size_t i = 0; i < v.size() && i < 32; ++i) {
        std::wstring p = v[i];
        if (Lower(p).rfind(root, 0) == 0) p = p.substr(root.size());
        if (!s.empty()) s += L'|';
        s += p;
    }
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"stickers", L"Recent", s.c_str(), IniFile().c_str());
}

void AddRecent(const std::wstring& path) {
    std::vector<std::wstring> v = LoadRecent();
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }), v.end());
    v.insert(v.begin(), path);
    if (v.size() > 32) v.resize(32);
    SaveRecent(v);
}

std::wstring PrettyName(const std::wstring& file) {
    std::wstring n = FileStem(file);
    for (auto& c : n)
        if (c == L'_' || c == L'-') c = L' ';
    return n;
}

void BuildGroups(Pack& p) {
    p.groups.clear();
    p.groupFirst.clear();
    std::map<std::wstring, int> first;
    for (int i = 0; i < (int)p.items.size(); ++i)
        if (!p.items[i].group.empty() && !first.count(p.items[i].group)) first[p.items[i].group] = i;
    for (auto& g : kGroups)
        if (auto it = first.find(g.key); it != first.end()) {
            p.groups.push_back(it->first);
            p.groupFirst.push_back(it->second);
            first.erase(it);
        }
    for (auto& [k, i] : first) p.groups.push_back(k), p.groupFirst.push_back(i);
    if (p.groups.size() < 2) p.groups.clear(), p.groupFirst.clear();
}

void LoadItems(Pack& p) {
    p.items.clear();
    std::wstring idx = ReadUtf8(p.dir + L"\\index.tsv");
    if (!idx.empty()) {
        size_t a = 0;
        while (a < idx.size()) {
            size_t b = idx.find(L'\n', a);
            if (b == std::wstring::npos) b = idx.size();
            std::wstring line = idx.substr(a, b - a);
            a = b + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty() || line[0] == L'#') continue;
            std::wstring f[4];
            size_t s = 0;
            for (int k = 0; k < 4; ++k) {
                size_t t = k < 3 ? line.find(L'\t', s) : std::wstring::npos;
                f[k] = line.substr(s, t == std::wstring::npos ? std::wstring::npos : t - s);
                if (t == std::wstring::npos) break;
                s = t + 1;
            }
            if (f[0].empty() || f[0].find_first_of(L"\\/:") != std::wstring::npos || !IsStickerImage(f[0])) continue;  // plain names only
            Item it;
            it.file = p.dir + L"\\" + f[0];
            it.name = f[1].empty() ? PrettyName(f[0]) : f[1];
            it.group = f[2];
            for (auto& c : f[3])
                if (c == L',') c = L' ';
            it.hay = Lower(it.name + L" " + f[3] + L" " + FileStem(f[0]));
            p.items.push_back(std::move(it));
        }
    } else {
        for (auto& file : StickerFiles(p.dir)) {
            Item it;
            it.file = file;
            it.name = PrettyName(file);
            it.hay = Lower(it.name);
            p.items.push_back(std::move(it));
        }
    }
    BuildGroups(p);
    p.loaded = true;
}

std::vector<Pack>& Packs() {
    static std::vector<Pack> packs;
    static std::wstring root;
    std::wstring r = StickersRoot();
    if (!packs.empty() && root == r) return packs;
    root = r;
    packs.clear();
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((r + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != L"." && n != L".." && _wcsicmp(n.c_str(), L"Custom") != 0)
                dirs.push_back(r + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring blob = StickerFolder();
    if (!blob.empty() && std::none_of(dirs.begin(), dirs.end(), [&](auto& d) { return _wcsicmp(d.c_str(), blob.c_str()) == 0; }))
        dirs.push_back(blob);
    Pack recent;
    recent.id = L"Recent";
    recent.recent = true;
    recent.order = -1;
    packs.push_back(recent);
    for (auto& d : dirs) {
        Pack p;
        p.id = PathFindFileNameW(d.c_str());
        p.dir = d;
        std::wstring ini = d + L"\\pack.ini";
        bool blobPack = _wcsicmp(p.id.c_str(), L"BlobEmoji") == 0;
        p.title = ProfileString(ini, L"pack", IsEnglish() ? L"name" : L"name_ru");
        if (p.title.empty()) p.title = ProfileString(ini, L"pack", L"name");
        if (p.title.empty()) p.title = blobPack ? L"Blob Emoji" : p.id;
        p.shortName = ProfileString(ini, L"pack", L"short", blobPack ? L"Blob" : p.title.c_str());
        p.order = GetPrivateProfileIntW(L"pack", L"order", blobPack ? 10 : 100, ini.c_str());
        std::wstring lic = ProfileString(ini, L"pack", L"license", blobPack ? L"Apache-2.0" : L"");
        if (!lic.empty()) p.info = lic;
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES && StickerFiles(d).empty()) continue;  // not a pack
        packs.push_back(std::move(p));
    }
    Pack custom;
    custom.id = L"Custom";
    custom.custom = true;
    custom.order = 1000;
    custom.dir = r + L"\\Custom";
    packs.push_back(custom);
    std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.order < b.order; });
    return packs;
}

void LoadRecentPack(Pack& rp) {
    rp.items.clear();
    std::unordered_map<std::wstring, const Item*> byFile;
    for (auto& p : Packs())
        if (p.loaded && !p.recent)
            for (auto& it : p.items) byFile[Lower(it.file)] = &it;
    for (auto& f : LoadRecent()) {
        auto it = byFile.find(Lower(f));
        Item i = it != byFile.end() ? *it->second : Item{f, PrettyName(f), L"", Lower(PrettyName(f))};
        i.group.clear();
        rp.items.push_back(std::move(i));
    }
    rp.loaded = true;
}

void EnsureLoaded(Pack& p) {
    if (p.recent) LoadRecentPack(p);
    else if (p.custom) {
        p.dir = StickersRoot() + L"\\Custom";
        LoadItems(p);
    } else if (!p.loaded) LoadItems(p);
}

std::wstring UniqueIn(const std::wstring& dir, const std::wstring& name) {
    std::wstring stem = FileStem(name), ext = PathFindExtensionW(name.c_str());
    std::wstring p = dir + L"\\" + name;
    for (int i = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000; ++i)
        p = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    return p;
}

// ---------------------------------------------------------------- thumbnails (worker thread)

struct ThumbEntry {
    int state = 0;  // 0 not requested, 1 queued, 2 ready, 3 failed
    int w = 0, h = 0, n = 0, total = 0;
    std::vector<uint32_t> px;  // frames stacked vertically, premultiplied (freed once the HBITMAP exists)
    std::vector<int> delays;
    HBITMAP bmp = nullptr;
    size_t bytes = 0;
    uint64_t lastUse = 0;
    int FrameAt(long long ms) const {
        if (n < 2 || total <= 0) return 0;
        int t = (int)(ms % total);
        for (int i = 0; i < n; ++i) {
            if (t < delays[i]) return i;
            t -= delays[i];
        }
        return n - 1;
    }
};

struct ThumbCache {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::wstring, std::shared_ptr<ThumbEntry>> map;
    std::deque<std::pair<std::wstring, int>> queue;  // key, size
    int inFlight = 0;
    std::atomic<HWND> notify{nullptr};
    bool started = false;
    size_t gdiBytes = 0;
    uint64_t useTick = 0;

    static std::wstring Key(const std::wstring& path, int size) { return std::to_wstring(size) + L"|" + path; }

    void Start() {
        if (started) return;
        started = true;
        std::thread([this] { Worker(); }).detach();
    }

    void Worker() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::pair<std::wstring, int> job;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return !queue.empty(); });
                job = queue.front();
                queue.pop_front();
                ++inFlight;
            }
            std::wstring path = job.first.substr(job.first.find(L'|') + 1);
            ThumbEntry r;
            Decode(path, job.second, r);
            {
                std::lock_guard<std::mutex> lk(mu);
                --inFlight;
                auto it = map.find(job.first);
                if (it != map.end() && it->second->state == 1) {
                    ThumbEntry& e = *it->second;
                    e.w = r.w, e.h = r.h, e.n = r.n, e.total = r.total;
                    e.px = std::move(r.px);
                    e.delays = std::move(r.delays);
                    e.state = e.n > 0 ? 2 : 3;
                }
            }
            if (HWND h = notify.load()) PostMessageW(h, WM_APP + 7, 0, 0);
        }
    }

    static void Decode(const std::wstring& path, int size, ThumbEntry& r) {
        Anim a;
        if (IsGif(path) && LoadAnimation(path, a, 400)) {
            // Thumbnails keep at most 40 frames (merged delays) to bound memory.
            int step = std::max(1, ((int)a.frames.size() + 39) / 40);
            std::vector<Image> frames;
            for (size_t i = 0; i < a.frames.size(); i += step) {
                int d = 0;
                for (size_t k = i; k < std::min(a.frames.size(), i + step); ++k) d += a.delays[k];
                frames.push_back(ResizeLimit(*a.frames[i], size));
                r.delays.push_back(d);
                r.total += d;
            }
            r.w = frames[0].w, r.h = frames[0].h, r.n = (int)frames.size();
            r.px.reserve((size_t)r.w * r.h * r.n);
            for (auto& f : frames) {
                if (f.w != r.w || f.h != r.h) continue;
                r.px.insert(r.px.end(), f.px.begin(), f.px.end());
            }
            r.n = (int)(r.px.size() / ((size_t)r.w * r.h));
            return;
        }
        Image im;
        if (!LoadImagePremul(path, im)) return;
        im = ResizeLimit(im, size);
        r.w = im.w, r.h = im.h, r.n = 1;
        r.px = std::move(im.px);
    }

    // UI thread: the entry for a cell; `want` collects the ones still to decode.
    std::shared_ptr<ThumbEntry> Get(const std::wstring& path, int size, std::vector<std::pair<std::wstring, int>>& want) {
        std::wstring k = Key(path, size);
        std::lock_guard<std::mutex> lk(mu);
        auto& slot = map[k];
        if (!slot) slot = std::make_shared<ThumbEntry>();
        std::shared_ptr<ThumbEntry> e = slot;
        if (e->state == 0 || e->state == 1) want.push_back({k, size});
        e->lastUse = useTick;
        if (e->state == 2 && !e->bmp && e->n > 0) {
            void* bits;
            e->bmp = CreateDib(e->w, e->h * e->n, &bits);
            if (e->bmp) {
                memcpy(bits, e->px.data(), e->px.size() * 4);
                e->bytes = e->px.size() * 4;
                gdiBytes += e->bytes;
            }
            std::vector<uint32_t>().swap(e->px);
        }
        return e;
    }

    // The queue becomes exactly what is visible now (fast scrolling drops stale requests).
    void Request(const std::vector<std::pair<std::wstring, int>>& want) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& q : queue)
            if (auto it = map.find(q.first); it != map.end() && it->second->state == 1) it->second->state = 0;
        queue.clear();
        for (auto& w : want)
            if (auto it = map.find(w.first); it != map.end() && it->second->state != 2 && it->second->state != 3) {
                it->second->state = 1;
                queue.push_back(w);
            }
        if (!queue.empty()) cv.notify_one();
    }

    bool Busy() {
        std::lock_guard<std::mutex> lk(mu);
        return !queue.empty() || inFlight > 0;
    }

    // Keeps GDI memory bounded: drops the least recently drawn bitmaps (not the ones on screen now).
    void Trim(size_t budget) {
        if (gdiBytes <= budget) return;
        std::vector<std::pair<uint64_t, ThumbEntry*>> v;
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [k, e] : map)
            if (e->bmp && e->lastUse < useTick) v.push_back({e->lastUse, e.get()});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [t, e] : v) {
            if (gdiBytes <= budget * 3 / 4) break;
            DeleteObject(e->bmp);
            e->bmp = nullptr;
            gdiBytes -= e->bytes;
            e->bytes = 0;
            e->state = 0;
        }
    }
};

ThumbCache& Thumbs() {
    static ThumbCache c;
    return c;
}

// ---------------------------------------------------------------- popup

enum { kTimerAnim = 1, WM_THUMB = WM_APP + 7 };

struct StickerPopup {
    HWND hwnd = nullptr, edit = nullptr;
    WNDPROC editProc = nullptr;
    float ui = 1;
    int cell = 44, cols = 12, visRows = 8, pad = 6, tabH = 28, rowH = 30, catH = 30, footer = 32, sbW = 10;
    int cur = 0, cat = -1, scrollY = 0, hover = -1, result = -1, size = 64, hoverTab = -1, hoverCat = -1;
    bool done = false, pinned = false, modal = false, sbDrag = false, tipShown = false, testMode = false;
    int sbGrab = 0;
    DWORD hoverTick = 0, openTick = GetTickCount();
    std::wstring query, resultPath;
    std::vector<int> view;  // filtered item indices of the current pack
    std::vector<RECT> tabRects, catRects;
    std::vector<std::pair<int, RECT>> chips;
    RECT pinRect{}, addRect{}, editRect{};
    HFONT font = nullptr, fontSmall = nullptr, icons = nullptr;
    HBRUSH editBrush = nullptr;
    int missing = 0;          // visible cells still waiting for a thumbnail (tests wait for 0)
    long long animSig = -1;   // frame indices of the visible animated cells at the last paint

    Pack& P() { return Packs()[cur]; }
    int S(float v) const { return (int)std::lround(v * ui); }
    int Thumb() const { return cell - S(8); }
    int minW = 0;  // the tab row may need more than the grid
    int Width() const { return std::max(minW, 2 * pad + cols * cell + sbW); }
    RECT Grid() const {
        int top = pad + tabH + rowH + catH;
        return {pad, top, pad + cols * cell, top + visRows * cell};
    }
    int Height() const { return Grid().bottom + footer; }
    int ContentH() const { return ((int)view.size() + cols - 1) / cols * cell; }
    int MaxScroll() const { return std::max(0, ContentH() - visRows * cell); }
    RECT Scrollbar() const {
        RECT g = Grid();
        return {g.right + S(2), g.top, g.right + S(2) + sbW - S(4), g.bottom};
    }

    // A query word matches the start of a word in the name/keywords ("cat" finds "cat face", not "indicator").
    static bool HasWord(const std::wstring& hay, const std::wstring& w) {
        for (size_t pos = hay.find(w); pos != std::wstring::npos; pos = hay.find(w, pos + 1))
            if (pos == 0 || !IsCharAlphaNumericW(hay[pos - 1])) return true;
        return false;
    }

    void Filter() {
        Pack& p = P();
        view.clear();
        std::vector<std::wstring> words;
        std::wstring q = Lower(query);
        size_t a = 0;
        while (a < q.size()) {
            size_t b = q.find(L' ', a);
            if (b == std::wstring::npos) b = q.size();
            if (b > a) words.push_back(q.substr(a, b - a));
            a = b + 1;
        }
        std::wstring group = cat >= 0 && cat < (int)p.groups.size() ? p.groups[cat] : L"";
        for (int i = 0; i < (int)p.items.size(); ++i) {
            const Item& it = p.items[i];
            if (!group.empty() && it.group != group) continue;
            bool ok = true;
            for (auto& w : words) {
                if (HasWord(it.hay, w)) continue;
                bool ru = false;
                for (auto& [k, en] : kRuWords) {
                    std::wstring kk = k;
                    if ((kk.rfind(w, 0) == 0 && w.size() >= 2) || (w.rfind(kk, 0) == 0 && kk.size() >= 2))
                        if (HasWord(it.hay, en)) { ru = true; break; }
                }
                if (!ru) { ok = false; break; }
            }
            if (ok) view.push_back(i);
        }
        scrollY = std::clamp(scrollY, 0, MaxScroll());
        hover = -1;
    }

    void SetPack(int i, bool keepScroll = false) {
        cur = std::clamp(i, 0, (int)Packs().size() - 1);
        EnsureLoaded(P());
        cat = -1;
        if (!keepScroll) scrollY = 0;
        Filter();
        WritePrivateProfileStringW(L"stickers", L"Pack", P().id.c_str(), IniFile().c_str());
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    int At(POINT pt) const {
        RECT g = Grid();
        if (!PtInRect(&g, pt)) return -1;
        int row = (pt.y - g.top + scrollY) / cell, col = (pt.x - g.left) / cell;
        int i = row * cols + col;
        return i < (int)view.size() ? i : -1;
    }

    const Item* ItemAt(int v) {
        if (v < 0 || v >= (int)view.size()) return nullptr;
        return &P().items[view[v]];
    }

    void Layout() {
        // tabs measured with the font; the window is wide enough for them
        HDC dc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(dc, font);
        tabRects.clear();
        int x = pad;
        for (auto& p : Packs()) {
            std::wstring t = TabText(p);
            SIZE z{};
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z);
            RECT r{x, pad, x + z.cx + S(16), pad + tabH - S(2)};
            tabRects.push_back(r);
            x = r.right + S(2);
        }
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        pinRect = {Width() - pad - S(26), pad, Width() - pad, pad + tabH - S(2)};
        int top = pad + tabH + S(3);
        addRect = {Width() - pad - S(118), top, Width() - pad, top + rowH - S(6)};
        editRect = {pad, top, pad + (cols * cell) * 55 / 100, top + rowH - S(6)};
        if (edit) MoveWindow(edit, editRect.left + S(24), editRect.top + S(4), editRect.right - editRect.left - S(28),
                             editRect.bottom - editRect.top - S(8), FALSE);
    }

    int TabsWidth() {
        Layout();
        return tabRects.empty() ? 0 : tabRects.back().right + S(30) + pad;
    }

    static std::wstring TabText(const Pack& p) {
        if (p.recent) return Tr(L"Недавние", L"Recent");
        if (p.custom) return Tr(L"Мои", L"Mine");
        return p.shortName;
    }

    void Paint(HDC out) {
        RECT cr;
        GetClientRect(hwnd, &cr);
        HDC dc = CreateCompatibleDC(out);
        HBITMAP buf = CreateCompatibleBitmap(out, cr.right, cr.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, buf);
        auto fill = [&](RECT r, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &r, b);
            DeleteObject(b);
        };
        auto frame = [&](RECT r, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            FrameRect(dc, &r, b);
            DeleteObject(b);
        };
        fill(cr, RGB(39, 39, 39));
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(231, 233, 234));
        Thumbs().useTick++;
        std::vector<std::pair<std::wstring, int>> want;
        HDC src = CreateCompatibleDC(out);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        long long now = (long long)(GetTickCount() - openTick);
        long long sig = 0;
        auto blit = [&](const std::shared_ptr<ThumbEntry>& e, RECT r, int maxSide, bool anim) {
            if (!e->bmp) return;
            int f = anim ? e->FrameAt(now) : 0;
            if (anim && e->n > 1) sig = sig * 31 + f + 1;
            int w = e->w, h = e->h;
            if (std::max(w, h) > maxSide) {
                w = std::max(1, e->w * maxSide / std::max(e->w, e->h));
                h = std::max(1, e->h * maxSide / std::max(e->w, e->h));
            }
            HGDIOBJ o = SelectObject(src, e->bmp);
            AlphaBlend(dc, (r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2, w, h, src, 0, f * e->h, e->w, e->h, bf);
            SelectObject(src, o);
        };

        // Row 1: pack tabs and the pin.
        for (int i = 0; i < (int)tabRects.size(); ++i) {
            RECT r = tabRects[i];
            bool sel = i == cur;
            if (sel) fill(r, RGB(46, 46, 46));
            else if (i == hoverTab) fill(r, RGB(52, 52, 56));
            if (sel) fill({r.left, r.bottom - S(2), r.right, r.bottom}, RGB(0, 138, 255));
            SetTextColor(dc, sel ? RGB(255, 255, 255) : RGB(190, 192, 194));
            std::wstring t = TabText(Packs()[i]);
            DrawTextW(dc, t.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SetTextColor(dc, pinned ? RGB(0, 138, 255) : RGB(170, 172, 174));
        SelectObject(dc, icons);
        DrawTextW(dc, pinned ? L"\xE840" : L"\xE718", -1, &pinRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, font);
        fill({pad, pad + tabH - S(1), cr.right - pad, pad + tabH}, RGB(58, 58, 60));

        // Row 2: search box (an EDIT child drawn over it), count / Add button.
        fill(editRect, RGB(46, 46, 46));
        frame(editRect, GetFocus() == edit ? RGB(0, 138, 255) : RGB(70, 70, 74));
        RECT ir{editRect.left + S(4), editRect.top, editRect.left + S(22), editRect.bottom};
        SetTextColor(dc, RGB(150, 152, 154));
        SelectObject(dc, icons);
        DrawTextW(dc, L"\xE721", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, font);
        Pack& p = P();
        if (p.custom) {
            fill(addRect, RGB(0, 120, 215));
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, Tr(L"+ Добавить...", L"+ Add..."), -1, &addRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(dc, RGB(150, 152, 154));
            SelectObject(dc, fontSmall);
            std::wstring info = std::to_wstring(view.size());
            if (view.size() != p.items.size()) info += L" / " + std::to_wstring(p.items.size());
            if (!p.info.empty()) info += L"  \x00B7  " + p.info;
            RECT r{editRect.right + S(8), editRect.top, cr.right - pad, editRect.bottom};
            DrawTextW(dc, info.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, font);
        }

        // Row 3: categories (icon of the group's first sticker) or a hint.
        catRects.clear();
        int ctop = pad + tabH + rowH;
        if (!p.groups.empty()) {
            int cs = catH - S(4), x = pad;
            RECT all{x, ctop, x + S(40), ctop + cs};
            catRects.push_back(all);
            if (cat < 0) fill(all, RGB(46, 66, 92)), frame(all, RGB(0, 138, 255));
            else if (hoverCat == 0) fill(all, RGB(52, 52, 56));
            SetTextColor(dc, RGB(231, 233, 234));
            DrawTextW(dc, Tr(L"Все", L"All"), -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x = all.right + S(4);
            for (int g = 0; g < (int)p.groups.size(); ++g) {
                RECT r{x, ctop, x + cs + S(6), ctop + cs};
                catRects.push_back(r);
                if (cat == g) fill(r, RGB(46, 66, 92)), frame(r, RGB(0, 138, 255));
                else if (hoverCat == g + 1) fill(r, RGB(52, 52, 56));
                auto e = Thumbs().Get(p.items[p.groupFirst[g]].file, Thumb(), want);
                blit(e, r, cs - S(6), false);
                x = r.right + S(4);
            }
        } else {
            SetTextColor(dc, RGB(150, 152, 154));
            SelectObject(dc, fontSmall);
            const wchar_t* hint = p.custom ? Tr(L"Перетащите сюда картинки или GIF, Ctrl+V вставит из буфера. ПКМ: удалить, показать в папке.",
                                                L"Drop images or GIFs here, Ctrl+V pastes from the clipboard. Right click: delete, show in folder.")
                                 : p.recent ? Tr(L"Последние 32 выбранных стикера.", L"The last 32 stickers you used.")
                                            : L"";
            RECT r{pad + S(2), ctop, cr.right - pad, ctop + catH - S(4)};
            DrawTextW(dc, hint, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, font);
        }

        // Grid: only the visible rows.
        RECT g = Grid();
        HRGN clip = CreateRectRgn(g.left, g.top, g.right, g.bottom);
        SelectClipRgn(dc, clip);
        int firstRow = scrollY / cell, lastRow = (scrollY + visRows * cell - 1) / cell;
        missing = 0;
        for (int r = firstRow; r <= lastRow; ++r)
            for (int c = 0; c < cols; ++c) {
                int v = r * cols + c;
                if (v >= (int)view.size()) break;
                RECT cell_r{g.left + c * cell, g.top + r * cell - scrollY, g.left + (c + 1) * cell, g.top + (r + 1) * cell - scrollY};
                if (v == hover) {
                    RECT hr{cell_r.left + 1, cell_r.top + 1, cell_r.right - 1, cell_r.bottom - 1};
                    fill(hr, RGB(62, 62, 66));
                }
                auto e = Thumbs().Get(p.items[view[v]].file, Thumb(), want);
                if (e->state == 3) {
                    SetTextColor(dc, RGB(120, 120, 124));
                    DrawTextW(dc, L"?", -1, &cell_r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (!e->bmp) {
                    ++missing;
                    RECT dot{(cell_r.left + cell_r.right) / 2 - S(2), (cell_r.top + cell_r.bottom) / 2 - S(2),
                             (cell_r.left + cell_r.right) / 2 + S(2), (cell_r.top + cell_r.bottom) / 2 + S(2)};
                    fill(dot, RGB(70, 70, 74));
                }
                blit(e, cell_r, Thumb(), true);
            }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        if (view.empty()) {
            SetTextColor(dc, RGB(150, 152, 154));
            const wchar_t* t = p.custom ? Tr(L"Пока пусто: нажмите «Добавить...» или перетащите файлы.", L"Empty: press Add... or drop files here.")
                               : p.recent ? Tr(L"Здесь появятся стикеры, которые вы выбирали.", L"Stickers you pick will show up here.")
                                          : Tr(L"Ничего не найдено.", L"Nothing found.");
            DrawTextW(dc, t, -1, &g, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        DeleteDC(src);
        Thumbs().Request(want);
        animSig = sig;
        if (MaxScroll() > 0) {  // scrollbar
            RECT sb = Scrollbar();
            int th = std::max(S(24), (int)((long long)(sb.bottom - sb.top) * visRows * cell / std::max(1, ContentH())));
            int y = sb.top + (int)((long long)(sb.bottom - sb.top - th) * scrollY / std::max(1, MaxScroll()));
            fill(sb, RGB(50, 50, 52));
            fill({sb.left, y, sb.right, y + th}, sbDrag ? RGB(0, 138, 255) : RGB(110, 110, 116));
        }

        // Footer: sticker size chips, the hovered sticker's name.
        RECT lr{pad + 2, g.bottom + S(4), pad + S(100), cr.bottom - pad};
        SetTextColor(dc, RGB(231, 233, 234));
        DrawTextW(dc, Tr(L"Размер стикера:", L"Sticker size:"), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        chips.clear();
        int x = lr.right + S(4);
        for (int v : {32, 48, 64, 96, 128, 192, 256}) {
            RECT ch{x, lr.top + S(3), x + S(34), lr.bottom - S(3)};
            if (v == size) fill(ch, RGB(46, 66, 92)), frame(ch, RGB(0, 138, 255));
            std::wstring s = std::to_wstring(v);
            DrawTextW(dc, s.c_str(), -1, &ch, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            chips.push_back({v, ch});
            x = ch.right + S(3);
        }
        if (const Item* it = ItemAt(hover)) {
            RECT nr{x + S(8), lr.top, cr.right - pad, lr.bottom};
            SetTextColor(dc, RGB(170, 172, 174));
            SelectObject(dc, fontSmall);
            DrawTextW(dc, it->name.c_str(), -1, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, font);
        }

        // Tooltip after a short hover: the name next to the cell.
        if (tipShown && (hover >= 0 || hoverCat > 0)) {
            std::wstring tip;
            RECT anchor{};
            if (const Item* it = ItemAt(hover)) {
                tip = it->name;
                int r = hover / cols, c = hover % cols;
                anchor = {g.left + c * cell, g.top + r * cell - scrollY, g.left + (c + 1) * cell, g.top + (r + 1) * cell - scrollY};
            } else if (hoverCat > 0 && hoverCat - 1 < (int)p.groups.size()) {
                tip = GroupTitle(p.groups[hoverCat - 1]);
                anchor = catRects[hoverCat];
            }
            if (!tip.empty()) {
                SelectObject(dc, fontSmall);
                RECT m{0, 0, S(300), 0};
                DrawTextW(dc, tip.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE);
                int tw = m.right + S(12), tht = m.bottom + S(8);
                int tx = std::clamp<int>((anchor.left + anchor.right) / 2 - tw / 2, pad, cr.right - pad - tw);
                int ty = anchor.bottom + S(2);
                if (ty + tht > g.bottom) ty = anchor.top - tht - S(2);
                RECT tr{tx, ty, tx + tw, ty + tht};
                fill(tr, RGB(28, 28, 30));
                frame(tr, RGB(90, 90, 96));
                SetTextColor(dc, RGB(231, 233, 234));
                DrawTextW(dc, tip.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, font);
            }
        }

        frame(cr, RGB(70, 70, 74));
        SelectObject(dc, oldFont);
        BitBlt(out, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(buf);
        DeleteDC(dc);
        Thumbs().Trim(96u << 20);
    }

    long long VisibleAnimSig() {
        long long sig = 0, now = (long long)(GetTickCount() - openTick);
        int firstRow = scrollY / cell, lastRow = (scrollY + visRows * cell - 1) / cell;
        std::vector<std::pair<std::wstring, int>> want;
        for (int v = firstRow * cols; v < std::min((int)view.size(), (lastRow + 1) * cols); ++v) {
            std::wstring k = ThumbCache::Key(P().items[view[v]].file, Thumb());
            std::lock_guard<std::mutex> lk(Thumbs().mu);
            auto it = Thumbs().map.find(k);
            if (it != Thumbs().map.end() && it->second->bmp && it->second->n > 1) sig = sig * 31 + it->second->FrameAt(now) + 1;
        }
        return sig;
    }

    void Pick(int v) {
        if (const Item* it = ItemAt(v)) {
            result = v;
            resultPath = it->file;
            done = true;
        }
    }

    void Scroll(int dy) {
        int s = std::clamp(scrollY + dy, 0, MaxScroll());
        if (s == scrollY) return;
        scrollY = s;
        tipShown = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void AfterCustomChange(bool show) {
        for (int i = 0; i < (int)Packs().size(); ++i)
            if (Packs()[i].custom && (show || i == cur)) {
                SetPack(i, !show);
                return;
            }
    }

    void AddFiles(const std::vector<std::wstring>& files) {
        int added = 0;
        for (auto& f : files)
            if (IsStickerImage(f) && !AddCustomStickerFile(f).empty()) ++added;
        if (added) AfterCustomChange(true);
    }

    bool PasteImage() {
        Image im;
        if (!ClipboardImage(hwnd, im)) return false;
        if (!AddCustomStickerImage(im).empty()) AfterCustomChange(true);
        return true;
    }

    void AddDialog() {
        std::vector<wchar_t> buf(64 * 1024, L'\0');
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Images (*.png, *.jpg, *.webp, *.gif, *.bmp)\0*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp\0All files\0*.*\0";
        ofn.lpstrFile = buf.data();
        ofn.nMaxFile = (DWORD)buf.size();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
        modal = true;
        BOOL ok = GetOpenFileNameW(&ofn);
        modal = false;
        SetForegroundWindow(hwnd);
        if (!ok) return;
        std::vector<std::wstring> files;
        std::wstring first = buf.data();
        const wchar_t* q = buf.data() + first.size() + 1;
        if (!*q) files.push_back(first);
        for (; *q; q += wcslen(q) + 1) files.push_back(first + L"\\" + q);
        AddFiles(files);
    }

    void ContextMenu(int v, POINT screen) {
        const Item* it = ItemAt(v);
        if (!it) return;
        std::wstring file = it->file;
        HMENU m = CreatePopupMenu();
        Pack& p = P();
        if (p.custom) AppendMenuW(m, MF_STRING, 1, Tr(L"Удалить (в корзину)", L"Delete (to Recycle Bin)"));
        else AppendMenuW(m, MF_STRING, 3, Tr(L"Добавить в мои стикеры", L"Add to my stickers"));
        if (p.recent) AppendMenuW(m, MF_STRING, 4, Tr(L"Убрать из недавних", L"Remove from recent"));
        AppendMenuW(m, MF_STRING, 2, Tr(L"Показать в папке", L"Show in folder"));
        modal = true;
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
        modal = false;
        DestroyMenu(m);
        if (id == 1) {
            std::wstring from = file + L'\0';
            SHFILEOPSTRUCTW op{};
            op.hwnd = hwnd;
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            modal = true;
            SHFileOperationW(&op);
            modal = false;
            AfterCustomChange(false);
        } else if (id == 2) {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        } else if (id == 3) {
            if (!AddCustomStickerFile(file).empty()) AfterCustomChange(true);
        } else if (id == 4) {
            std::vector<std::wstring> r = LoadRecent();
            r.erase(std::remove_if(r.begin(), r.end(), [&](auto& x) { return _wcsicmp(x.c_str(), file.c_str()) == 0; }), r.end());
            SaveRecent(r);
            SetPack(cur, true);
        }
    }

    static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* p = (StickerPopup*)GetWindowLongPtrW(GetParent(h), GWLP_USERDATA);
        static WNDPROC base = nullptr;
        if (p) base = p->editProc;
        if (!base) return DefWindowProcW(h, m, w, l);
        if (!p) return CallWindowProcW(base, h, m, w, l);  // detached: plain edit
        switch (m) {
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                if (GetWindowTextLengthW(h)) SetWindowTextW(h, L"");
                else p->done = true;
                return 0;
            }
            if (w == VK_RETURN) {
                p->Pick(p->hover >= 0 ? p->hover : 0);
                return 0;
            }
            if (w == VK_DOWN || w == VK_UP || w == VK_NEXT || w == VK_PRIOR) {
                int d = w == VK_DOWN ? p->cell : w == VK_UP ? -p->cell : w == VK_NEXT ? p->visRows * p->cell : -p->visRows * p->cell;
                p->Scroll(d);
                return 0;
            }
            if (w == VK_TAB) {
                int n = (int)Packs().size();
                p->SetPack((p->cur + (GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1)) % n);
                return 0;
            }
            break;
        case WM_CHAR:
            if (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB) return 0;  // no beep
            break;
        case WM_PASTE:
            if (p->PasteImage()) return 0;
            break;
        }
        return CallWindowProcW(base, h, m, w, l);
    }

    static bool InRect(const RECT& r, POINT pt) { return PtInRect(&r, pt) != 0; }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* p = (StickerPopup*)GetWindowLongPtrW(h, GWLP_USERDATA);
        // Messages before WM_NCCREATE or after PickSticker detached the popup (DestroyWindow sends WM_ACTIVATE)
        // must not touch it: that null dereference crashed the app when the sticker picker closed.
        if (!p && m != WM_NCCREATE) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_NCCREATE:
            p = (StickerPopup*)((CREATESTRUCTW*)l)->lpCreateParams;
            p->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            p->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            p->Paint((HDC)w);
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)w, RGB(231, 233, 234));
            SetBkColor((HDC)w, RGB(46, 46, 46));
            return (LRESULT)p->editBrush;
        case WM_COMMAND:
            if ((HWND)l == p->edit && HIWORD(w) == EN_CHANGE) {
                int n = GetWindowTextLengthW(p->edit);
                std::wstring t(n + 1, L'\0');
                GetWindowTextW(p->edit, t.data(), n + 1);
                t.resize(n);
                p->query = t;
                p->scrollY = 0;
                p->Filter();
                InvalidateRect(h, nullptr, FALSE);
            }
            if ((HWND)l == p->edit && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) InvalidateRect(h, &p->editRect, FALSE);
            return 0;
        case WM_THUMB:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (w == kTimerAnim) {
                bool tip = (p->hover >= 0 || p->hoverCat > 0) && !p->tipShown && GetTickCount() - p->hoverTick > 450;
                if (tip) p->tipShown = true;
                if (tip || (p->animSig != 0 && p->VisibleAnimSig() != p->animSig)) InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
            if (p->sbDrag) {
                RECT sb = p->Scrollbar();
                int th = std::max(p->S(24), (int)((long long)(sb.bottom - sb.top) * p->visRows * p->cell / std::max(1, p->ContentH())));
                int span = std::max(1, (int)(sb.bottom - sb.top) - th);
                p->scrollY = std::clamp((int)((long long)(pt.y - sb.top - p->sbGrab) * p->MaxScroll() / span), 0, p->MaxScroll());
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            int i = p->At(pt), t = -1, c = -1;
            for (int k = 0; k < (int)p->tabRects.size(); ++k)
                if (InRect(p->tabRects[k], pt)) t = k;
            for (int k = 0; k < (int)p->catRects.size(); ++k)
                if (InRect(p->catRects[k], pt)) c = k;
            if (i != p->hover || t != p->hoverTab || c != p->hoverCat) {
                p->hover = i;
                p->hoverTab = t;
                p->hoverCat = c;
                p->hoverTick = GetTickCount();
                p->tipShown = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            p->hover = p->hoverTab = p->hoverCat = -1;
            p->tipShown = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(w);
            p->Scroll(-d * p->cell * 3 / 2 / WHEEL_DELTA);  // 1.5 rows per notch, high-resolution wheels scroll smoothly
            POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
            ScreenToClient(h, &pt);
            p->hover = p->At(pt);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
            for (int k = 0; k < (int)p->tabRects.size(); ++k)
                if (InRect(p->tabRects[k], pt)) {
                    p->SetPack(k);
                    return 0;
                }
            if (InRect(p->pinRect, pt)) {
                p->pinned = !p->pinned;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (p->P().custom && InRect(p->addRect, pt)) {
                p->AddDialog();
                return 0;
            }
            for (int k = 0; k < (int)p->catRects.size(); ++k)
                if (InRect(p->catRects[k], pt)) {
                    p->cat = k - 1;
                    p->scrollY = 0;
                    p->Filter();
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            for (auto& [v, r] : p->chips)
                if (InRect(r, pt)) {
                    p->size = v;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            RECT sb = p->Scrollbar();
            if (p->MaxScroll() > 0 && pt.x >= sb.left - p->S(2) && pt.x <= sb.right + p->S(2) && pt.y >= sb.top && pt.y < sb.bottom) {
                int th = std::max(p->S(24), (int)((long long)(sb.bottom - sb.top) * p->visRows * p->cell / std::max(1, p->ContentH())));
                int y = sb.top + (int)((long long)(sb.bottom - sb.top - th) * p->scrollY / std::max(1, p->MaxScroll()));
                p->sbGrab = pt.y >= y && pt.y < y + th ? pt.y - y : th / 2;
                p->sbDrag = true;
                SetCapture(h);
                SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, l);
                return 0;
            }
            p->Pick(p->At(pt));
            return 0;
        }
        case WM_LBUTTONUP:
            if (p->sbDrag) {
                p->sbDrag = false;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
            int v = p->At(pt);
            ClientToScreen(h, &pt);
            p->ContextMenu(v, pt);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)w;
            std::vector<std::wstring> files;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t f[MAX_PATH];
                if (DragQueryFileW(drop, i, f, MAX_PATH)) files.push_back(f);
            }
            DragFinish(drop);
            p->AddFiles(files);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) p->done = true;
            if (w == VK_RETURN && p->hover >= 0) p->Pick(p->hover);
            if (w == 'F' && GetKeyState(VK_CONTROL) < 0) SetFocus(p->edit);
            if (w == 'V' && GetKeyState(VK_CONTROL) < 0) p->PasteImage();
            return 0;
        case WM_CHAR:
            if (w >= 32 && p->edit) {  // typing anywhere goes to the search box
                SetFocus(p->edit);
                SendMessageW(p->edit, WM_CHAR, w, l);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && !p->pinned && !p->modal) p->done = true;  // click outside
            return 0;
        case WM_CLOSE:
            p->done = true;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

// Grabs the popup through WM_PRINTCLIENT (it never comes on screen in tests).
bool DumpPopup(StickerPopup& p, const std::wstring& file) {
    RECT r;
    GetClientRect(p.hwnd, &r);
    Image img;
    img.w = r.right;
    img.h = r.bottom;
    void* bits;
    HBITMAP bmp = CreateDib(img.w, img.h, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(p.hwnd, dc, PW_CLIENTONLY);  // synchronous WM_PRINT (the DWM copy of an off-screen window lags)
    GdiFlush();
    img.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)img.w * img.h);
    for (auto& px : img.px) px |= 0xFF000000u;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bmp);
    return EncodeImage(img, file, false);
}

void PumpUntilLoaded(StickerPopup& p, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        InvalidateRect(p.hwnd, nullptr, FALSE);
        UpdateWindow(p.hwnd);
        if ((p.missing == 0 && !Thumbs().Busy()) || GetTickCount() - start > timeoutMs) break;
        Sleep(15);
    }
}

}  // namespace

// ---------------------------------------------------------------- my stickers

std::wstring AddCustomStickerFile(const std::wstring& file) {
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    std::wstring dir = CustomStickerFolder();
    if (Lower(file).rfind(Lower(dir) + L"\\", 0) == 0) return file;  // already there
    std::wstring to = UniqueIn(dir, PathFindFileNameW(file.c_str()));
    return CopyFileW(file.c_str(), to.c_str(), TRUE) ? to : L"";
}

std::wstring AddCustomStickerImage(const Image& premul) {
    if (premul.empty()) return L"";
    Image st = premul;
    st.alpha = false;
    for (auto& px : st.px) {  // premultiplied -> straight
        uint32_t a = px >> 24;
        if (a != 255) st.alpha = true;
        if (a == 0 || a == 255) continue;
        px = a << 24 | std::min(255u, (px >> 16 & 255) * 255 / a) << 16 | std::min(255u, (px >> 8 & 255) * 255 / a) << 8 |
             std::min(255u, (px & 255) * 255 / a);
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"sticker_%04d%02d%02d_%02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring to = UniqueIn(CustomStickerFolder(), name);
    return EncodeImage(st, to, false) ? to : L"";
}

// ---------------------------------------------------------------- PickSticker

bool PickSticker(HWND owner, POINT at, float ui, std::wstring& path, int& size) {
    std::vector<Pack>& packs = Packs();
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = StickerPopup::Proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ZipshotStickers";
        RegisterClassW(&wc);
        reg = true;
    }
    bool test = GetEnvironmentVariableW(L"ZIPSHOT_TEST_STICKERS", nullptr, 0) != 0;
    StickerPopup p;
    p.ui = ui;
    p.testMode = test;
    p.size = size;
    p.cell = (int)std::round(44 * ui);
    p.pad = (int)std::round(6 * ui);
    p.tabH = (int)std::round(28 * ui);
    p.rowH = (int)std::round(32 * ui);
    p.catH = (int)std::round(32 * ui);
    p.footer = (int)std::round(32 * ui);
    p.sbW = (int)std::round(10 * ui);
    p.font = CreateFontW(-(int)std::round(12 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    p.fontSmall = CreateFontW(-(int)std::round(11 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    p.icons = CreateFontW(-(int)std::round(13 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    p.editBrush = CreateSolidBrush(RGB(46, 46, 46));
    // Last tab; the BlobEmoji pack (ShareX's) on the first run.
    std::wstring last = IniString(L"stickers", L"Pack", L"BlobEmoji");
    p.cur = 0;
    for (int i = 0; i < (int)packs.size(); ++i)
        if (_wcsicmp(packs[i].id.c_str(), last.c_str()) == 0) p.cur = i;
    Thumbs().Start();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, L"ZipshotStickers", L"Stickers", WS_POPUP | WS_CLIPCHILDREN,
                              -32000, -32000, 100, 100, owner, nullptr, g_inst, &p);
    auto cleanup = [&] {
        DeleteObject(p.font);
        DeleteObject(p.fontSmall);
        DeleteObject(p.icons);
        DeleteObject(p.editBrush);
    };
    if (!hw) {
        cleanup();
        return false;
    }
    p.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hw, nullptr, g_inst, nullptr);
    SendMessageW(p.edit, WM_SETFONT, (WPARAM)p.font, FALSE);
    SendMessageW(p.edit, EM_SETCUEBANNER, TRUE, (LPARAM)Tr(L"Поиск (Ctrl+F)", L"Search (Ctrl+F)"));
    p.editProc = (WNDPROC)SetWindowLongPtrW(p.edit, GWLP_WNDPROC, (LONG_PTR)StickerPopup::EditProc);
    p.SetPack(p.cur, false);
    p.minW = p.TabsWidth();
    int w = p.Width(), h = p.Height();
    p.Layout();
    int x = std::clamp<int>(at.x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w));
    int y = std::clamp<int>(at.y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h));
    Thumbs().notify = hw;
    if (test) {  // off-screen self-test, never activated
        SetWindowPos(hw, nullptr, -20000, -20000, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hw, SW_SHOWNOACTIVATE);
        UpdateWindow(hw);
        if (GetEnvironmentVariableW(L"ZIPSHOT_TEST_STICKERS_CUSTOM", nullptr, 0)) {  // My stickers + Recent (use a scratch root)
            for (auto& pk : packs)
                if (!pk.recent && !pk.custom) {
                    EnsureLoaded(pk);
                    if (pk.items.empty()) continue;
                    std::wstring added = AddCustomStickerFile(pk.items[std::min<size_t>(3, pk.items.size() - 1)].file);
                    AddRecent(pk.items[0].file);
                    Log(L"test-stickers: custom %s", added.c_str());
                }
            Log(L"test-stickers: custom image %s", AddCustomStickerImage(CursorImage(3)).c_str());
        }
        wchar_t dump[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"ZIPSHOT_TEST_STICKERS_DUMP", dump, MAX_PATH)) {
            std::wstring dir = dump;
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            for (int i = 0; i < (int)packs.size(); ++i) {
                p.SetPack(i);
                PumpUntilLoaded(p, 15000);
                if (!p.view.empty()) {  // hover a cell: tooltip and name
                    p.hover = std::min<int>(13, (int)p.view.size() - 1);
                    p.tipShown = true;
                }
                PumpUntilLoaded(p, 2000);
                wchar_t n[16];
                swprintf_s(n, L"%02d_", i);
                DumpPopup(p, dir + L"\\picker_" + n + packs[i].id + L".png");
                Log(L"test-stickers: tab %s items %d groups %d missing %d", packs[i].id.c_str(), (int)packs[i].items.size(),
                    (int)packs[i].groups.size(), p.missing);
                p.hover = -1;
                p.tipShown = false;
                if (!packs[i].groups.empty() && packs[i].items.size() > 1000) {  // a category and a search
                    p.cat = 2;
                    p.Filter();
                    PumpUntilLoaded(p, 15000);
                    DumpPopup(p, dir + L"\\picker_" + n + packs[i].id + L"_category.png");
                    p.cat = -1;
                    SetWindowTextW(p.edit, L"кот");
                    PumpUntilLoaded(p, 15000);
                    DumpPopup(p, dir + L"\\picker_" + n + packs[i].id + L"_search_ru.png");
                    Log(L"test-stickers: %s search 'кот' -> %d", packs[i].id.c_str(), (int)p.view.size());
                    SetWindowTextW(p.edit, L"heart");
                    PumpUntilLoaded(p, 15000);
                    DumpPopup(p, dir + L"\\picker_" + n + packs[i].id + L"_search_en.png");
                    SetWindowTextW(p.edit, L"");
                }
                if (packs[i].id == L"NotoAnimated") {  // a later animation frame
                    Sleep(300);
                    PumpUntilLoaded(p, 2000);
                    DumpPopup(p, dir + L"\\picker_" + n + packs[i].id + L"_later.png");
                }
            }
            for (int i = 0; i < (int)packs.size(); ++i)
                if (_wcsicmp(packs[i].id.c_str(), last.c_str()) == 0) p.SetPack(i);
        }
        if (p.view.empty()) {  // pick something real
            for (int i = 0; i < (int)packs.size() && p.view.empty(); ++i)
                if (!packs[i].recent && !packs[i].custom) p.SetPack(i);
        }
        p.Pick(0);
        p.done = true;
    } else {
        SetWindowPos(hw, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hw, SW_SHOW);
        SetForegroundWindow(hw);
        SetFocus(p.edit);
    }
    SetTimer(hw, kTimerAnim, 40, nullptr);  // animated thumbnails and the hover tooltip; dies with the window
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Pinned (stays open for drag and drop from Explorer): a click on another window of ours closes it and is
        // swallowed, so the editor is never re-entered while the picker runs.
        if (p.pinned && msg.hwnd != hw && !IsChild(hw, msg.hwnd) &&
            (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
             msg.message == WM_NCLBUTTONDOWN || msg.message == WM_NCRBUTTONDOWN)) {
            p.done = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!p.done) PostQuitMessage(0);
    Thumbs().notify = nullptr;
    KillTimer(hw, kTimerAnim);
    SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
    // An active popup gets WM_ACTIVATE(WA_INACTIVE) from DestroyWindow; the off-screen self-test is never active,
    // so it sends that message itself to exercise the same path.
    if (test) SendMessageW(hw, WM_ACTIVATE, WA_INACTIVE, 0);
    DestroyWindow(hw);
    cleanup();
    if (owner && !test) SetForegroundWindow(owner);
    size = p.size;
    if (p.result < 0 || p.resultPath.empty()) return false;
    path = p.resultPath;
    if (!test) AddRecent(path);
    return true;
}

}  // namespace shapes_ext
