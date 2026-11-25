#include "updater.h"
#include "common.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

// Built-in GitHub Releases auto-updater inspired by FluxTube:
// - Silent background check after startup (zero UI impact)
// - Native WinHTTP with HTTP/2, automatic proxy, 128 KB streaming, atomic .part write
// - Optional SHA-256 verification against SHA256SUMS.txt when published in the release
// - Instant in-place rename-swap for portable zipshot.exe / .zip (<1s total) or silent Inno Setup upgrade

namespace updater {
namespace {

struct State {
    std::mutex mu;
    bool checking = false;
    bool downloading = false;
    double progress = 0.0;
    ReleaseInfo available;
    std::wstring status;
    bool statusInitialized = false;
};

State& S() {
    static State s;
    return s;
}

std::wstring DefaultStatus() {
    return std::wstring(Tr(L"Версия ", L"Version ")) + kAppVersion;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t sl = wcslen(suffix);
    return s.size() >= sl && _wcsicmp(s.c_str() + s.size() - sl, suffix) == 0;
}

// Minimal, strict JSON value reader for GitHub Releases API responses.
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal& operator[](const char* k) const {
        static const JVal nullVal;
        if (type == Obj) {
            for (const auto& kv : obj)
                if (kv.first == k) return kv.second;
        }
        return nullVal;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}
    bool Parse(JVal& out) {
        if (!Value(out, 0)) return false;
        Ws();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void Ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
    }
    bool Lit(const char* w) {
        size_t n = strlen(w);
        if (i_ + n > s_.size() || memcmp(s_.data() + i_, w, n) != 0) return false;
        i_ += n;
        return true;
    }
    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool Str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i_ + 4 > s_.size()) return false;
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    int h = Hex(s_[i_++]);
                    if (h < 0) return false;
                    cp = (cp << 4) | (uint32_t)h;
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(JVal& v, int depth) {
        if (depth > 64) return false;
        Ws();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.type = JVal::Obj;
            Ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!Str(k)) return false;
                Ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                JVal child;
                if (!Value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                Ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            v.type = JVal::Arr;
            Ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                JVal child;
                if (!Value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = JVal::Str; return Str(v.str); }
        if (Lit("true")) { v.type = JVal::Bool; v.num = 1; return true; }
        if (Lit("false")) { v.type = JVal::Bool; v.num = 0; return true; }
        if (Lit("null")) { v.type = JVal::Null; return true; }
        size_t start = i_;
        if (c == '-') ++i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        if (i_ == start) return false;
        char* end = nullptr;
        std::string tmp = s_.substr(start, i_ - start);
        v.num = strtod(tmp.c_str(), &end);
        if (!end || *end) return false;
        v.type = JVal::Num;
        return true;
    }
};

bool ParseVersionTuple(const std::wstring& raw, std::array<int, 4>& out) {
    out = {0, 0, 0, 0};
    size_t pos = 0;
    while (pos < raw.size() && (raw[pos] == L' ' || raw[pos] == L'\t' || raw[pos] == L'v' || raw[pos] == L'V')) ++pos;
    if (pos >= raw.size()) return false;
    std::wstring s = raw.substr(pos);
    size_t cut = s.find_first_of(L"-+ \t\r\n");
    if (cut != std::wstring::npos) s.resize(cut);
    if (s.empty()) return false;
    int part = 0;
    size_t start = 0;
    while (start < s.size() && part < 4) {
        size_t dot = s.find(L'.', start);
        std::wstring seg = s.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
        if (seg.empty()) return false;
        int val = 0;
        for (wchar_t ch : seg) {
            if (ch < L'0' || ch > L'9') return false;
            if (val > 100000000) return false;
            val = val * 10 + (ch - L'0');
        }
        out[part++] = val;
        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }
    return part > 0;
}

struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET v = nullptr) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};

WinHttpHandle OpenSession() {
    std::wstring ua = std::wstring(L"Zipshot/") + kAppVersion;
    HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (h) {
        DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(h, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
        DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    }
    return WinHttpHandle(h);
}

bool CrackUrl(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& https) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    port = uc.nPort;
    https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return !host.empty();
}

bool HttpGetText(const std::wstring& url, const wchar_t* accept, DWORD timeoutMs, DWORD& statusCode, std::string& body) {
    statusCode = 0;
    body.clear();
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = true;
    if (!CrackUrl(url, host, path, port, https)) return false;
    WinHttpHandle ses = OpenSession();
    if (!ses) return false;
    WinHttpSetTimeouts(ses.h, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
    WinHttpHandle conn(WinHttpConnect(ses.h, host.c_str(), port, 0));
    if (!conn) return false;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return false;
    if (accept && *accept) {
        std::wstring hdr = std::wstring(L"Accept: ") + accept + L"\r\n";
        WinHttpAddRequestHeaders(req.h, hdr.c_str(), (DWORD)hdr.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return false;
    DWORD sz = sizeof(statusCode);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode < 200 || statusCode >= 300) return false;
    char buf[16384];
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req.h, &avail) && avail > 0) {
        DWORD toRead = std::min<DWORD>(avail, sizeof(buf));
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf, toRead, &got) || !got) break;
        body.append(buf, got);
        if (body.size() > 4 * 1024 * 1024) return false;
    }
    return true;
}

bool ComputeFileSha256(const std::wstring& path, std::string& outHex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash);
    BYTE buf[65536];
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = CryptHashData(hash, buf, got, 0) != FALSE;
    }
    CloseHandle(f);
    BYTE digest[32];
    DWORD dlen = sizeof(digest);
    ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0) && dlen == 32;
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    if (!ok) return false;
    static const char hex[] = "0123456789abcdef";
    outHex.clear();
    outHex.reserve(64);
    for (BYTE b : digest) { outHex += hex[b >> 4]; outHex += hex[b & 15]; }
    return true;
}

bool HasHeaderMagic(const std::wstring& path, const char* magic, size_t len) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[8]{};
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, (DWORD)std::min(len, sizeof(buf)), &got, nullptr);
    CloseHandle(f);
    return ok && got == len && memcmp(buf, magic, len) == 0;
}

bool ExtractExeFromZip(const std::wstring& zipPath, const std::wstring& extractDir, std::wstring& outExe) {
    CreateDirectoryW(extractDir.c_str(), nullptr);
    std::wstring cmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + extractDir + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) return false;
    std::wstring direct = extractDir + L"\\zipshot.exe";
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) {
        outExe = direct;
        return true;
    }
    // Check one directory level deep in case the archive wraps a root folder.
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((extractDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                std::wstring cand = extractDir + L"\\" + fd.cFileName + L"\\zipshot.exe";
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    outExe = cand;
                    FindClose(h);
                    return true;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return false;
}

bool SwapAndRestart(const std::wstring& newExePath) {
    wchar_t curExe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, curExe, MAX_PATH)) return false;
    std::wstring current(curExe);
    std::wstring oldExe = current + L".old";
    std::wstring staged = current + L".new";

    DeleteFileW(oldExe.c_str());
    DeleteFileW(staged.c_str());

    // Stage next to current exe so the final swap is an atomic same-volume rename.
    if (!MoveFileExW(newExePath.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        return false;
    }
    // Windows allows renaming a running executable, then moving the new binary into its original name.
    if (!MoveFileExW(current.c_str(), oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(staged.c_str());
        return false;
    }
    if (!MoveFileExW(staged.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Roll back immediately so the installation is never left without zipshot.exe.
        MoveFileExW(oldExe.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(staged.c_str());
        return false;
    }
    std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId());
    ShellExecuteW(nullptr, L"open", current.c_str(), args.c_str(), ExeDir().c_str(), SW_SHOWNORMAL);
    return true;
}

}  // namespace

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ull) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    else swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
    return buf;
}

bool IsNewerVersion(const std::wstring& remote, const std::wstring& local) {
    std::array<int, 4> rv{}, lv{};
    if (!ParseVersionTuple(remote, rv) || !ParseVersionTuple(local, lv)) return false;
    return rv > lv;
}

bool ParseLatestReleaseJson(const std::string& jsonUtf8, ReleaseInfo& out) {
    out = {};
    JVal root;
    if (!JsonReader(jsonUtf8).Parse(root) || root.type != JVal::Obj) return false;
    const JVal& tagVal = root["tag_name"];
    if (tagVal.type != JVal::Str || tagVal.str.empty()) return false;
    std::wstring tag = Utf8ToWide(tagVal.str);
    while (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V' || tag.front() == L' ')) tag.erase(0, 1);
    while (!tag.empty() && tag.back() == L' ') tag.pop_back();
    if (tag.empty()) return false;
    out.version = tag;

    const JVal& assets = root["assets"];
    if (assets.type != JVal::Arr) return false;

    int bestScore = 0;
    for (const JVal& a : assets.arr) {
        if (a.type != JVal::Obj) continue;
        std::wstring name = Utf8ToWide(a["name"].str);
        std::wstring url = Utf8ToWide(a["browser_download_url"].str);
        uint64_t size = a["size"].type == JVal::Num && a["size"].num > 0 ? (uint64_t)a["size"].num : 0;
        if (name.empty() || url.empty()) continue;
        std::wstring low = LowerW(name);
        if (low == L"sha256sums.txt" || low == L"sha256sums") {
            out.sha256Url = url;
            continue;
        }
        int score = 0;
        AssetKind kind = AssetKind::None;
        if (EndsWithI(low, L".exe")) {
            bool isSetup = low.find(L"setup") != std::wstring::npos || low.find(L"installer") != std::wstring::npos;
            if (!isSetup && (low == L"zipshot.exe" || low.find(L"zipshot") != std::wstring::npos)) {
                score = 30;  // fastest: direct portable exe swap (<1s)
                kind = AssetKind::StandaloneExe;
            } else if (isSetup) {
                score = 20;  // Inno Setup silent upgrade
                kind = AssetKind::InstallerExe;
            }
        } else if (EndsWithI(low, L".zip") && low.find(L"zipshot") != std::wstring::npos) {
            score = 10;      // portable zip with zipshot.exe inside
            kind = AssetKind::PortableZip;
        }
        if (score > bestScore) {
            bestScore = score;
            out.assetName = name;
            out.downloadUrl = url;
            out.assetSize = size;
            out.kind = kind;
        }
    }
    return out.kind != AssetKind::None && !out.downloadUrl.empty();
}

bool ParseSha256ForAsset(const std::string& sumsUtf8, const std::wstring& assetName, std::string& outHexLower) {
    outHexLower.clear();
    std::wstring target = LowerW(PathFindFileNameW(assetName.c_str()));
    size_t pos = 0;
    while (pos < sumsUtf8.size()) {
        size_t eol = sumsUtf8.find('\n', pos);
        std::string line = sumsUtf8.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? sumsUtf8.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.size() < 66) continue;
        std::string hex = line.substr(0, 64);
        bool validHex = true;
        for (char& c : hex) {
            if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { validHex = false; break; }
        }
        if (!validHex) continue;
        size_t ns = 64;
        while (ns < line.size() && (line[ns] == ' ' || line[ns] == '\t' || line[ns] == '*')) ++ns;
        std::wstring file = LowerW(PathFindFileNameW(Utf8ToWide(line.substr(ns)).c_str()));
        if (file == target) {
            outHexLower = hex;
            return true;
        }
    }
    return false;
}

void CleanupOldExe() {
    wchar_t curExe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, curExe, MAX_PATH)) return;
    std::wstring oldExe = std::wstring(curExe) + L".old";
    if (GetFileAttributesW(oldExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (int i = 0; i < 10; ++i) {
            if (DeleteFileW(oldExe.c_str())) break;
            Sleep(100);
        }
    }
}

void Check(bool manual) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().checking || S().downloading) return;
        S().checking = true;
        if (manual) {
            S().status = Tr(L"Проверяю обновления…", L"Checking for updates…");
            S().statusInitialized = true;
        }
    }
    MainWindowRefresh();
    try {
        std::thread([manual] {
            std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepo + L"/releases/latest";
            DWORD code = 0;
            std::string body;
            bool ok = HttpGetText(url, L"application/vnd.github+json", 12000, code, body);
            ReleaseInfo info;
            bool parsed = ok && ParseLatestReleaseJson(body, info);
            bool newer = parsed && IsNewerVersion(info.version, kAppVersion);
            RunOnMain([manual, ok, code, newer, info = std::move(info)] {
                std::wstring msg;
                {
                    std::lock_guard<std::mutex> lk(S().mu);
                    S().checking = false;
                    S().statusInitialized = true;
                    if (!ok) {
                        S().status = DefaultStatus();
                        if (manual) {
                            msg = code ? (std::wstring(Tr(L"Не удалось проверить обновления (HTTP ", L"Could not check for updates (HTTP ")) +
                                          std::to_wstring(code) + L")")
                                       : Tr(L"Не удалось подключиться к GitHub для проверки обновлений.",
                                            L"Could not connect to GitHub to check for updates.");
                        }
                    } else if (newer) {
                        S().available = info;
                        S().status = std::wstring(Tr(L"Доступно обновление ", L"Update available: ")) + info.version +
                                     (info.assetSize ? L" (" + FormatBytes(info.assetSize) + L")" : L"");
                        if (manual) {
                            msg = std::wstring(Tr(L"Доступна новая версия Zipshot ", L"New Zipshot version available: ")) + info.version +
                                  (info.assetSize ? L" (" + FormatBytes(info.assetSize) + L")" : L"") +
                                  Tr(L".\n\nУстановить сейчас?", L".\n\nInstall now?");
                        }
                    } else {
                        S().available = {};
                        S().status = std::wstring(Tr(L"Последняя версия (", L"Up to date (")) + kAppVersion + L")";
                        if (manual) {
                            msg = std::wstring(Tr(L"У вас установлена последняя версия Zipshot (", L"You already have the latest Zipshot (")) +
                                  kAppVersion + L").";
                        }
                    }
                }
                MainWindowRefresh();
                if (manual && !msg.empty()) {
                    if (newer) {
                        if (MessageBoxW(nullptr, msg.c_str(), Tr(L"Обновление Zipshot", L"Zipshot Update"),
                                        MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES) {
                            DownloadAndInstall();
                        }
                    } else {
                        MessageBoxW(nullptr, msg.c_str(), Tr(L"Обновление Zipshot", L"Zipshot Update"),
                                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_SETFOREGROUND);
                    }
                }
            });
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lk(S().mu);
        S().checking = false;
        S().status = DefaultStatus();
        S().statusInitialized = true;
    }
}

void DownloadAndInstall() {
    ReleaseInfo info;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().downloading || S().available.downloadUrl.empty()) return;
        S().downloading = true;
        S().progress = 0.0;
        info = S().available;
        S().status = std::wstring(Tr(L"Скачиваю Zipshot ", L"Downloading Zipshot ")) + info.version + L"…";
        S().statusInitialized = true;
    }
    MainWindowRefresh();

    try {
        std::thread([info] {
            auto fail = [&](const std::wstring& err) {
                Log(L"updater: %s", err.c_str());
                RunOnMain([err, ver = info.version] {
                    {
                        std::lock_guard<std::mutex> lk(S().mu);
                        S().downloading = false;
                        S().progress = 0.0;
                        S().status = std::wstring(Tr(L"Ошибка обновления (", L"Update failed (")) + ver + L")";
                        S().statusInitialized = true;
                    }
                    MainWindowRefresh();
                    PlayUiSound(S_Error);
                    MessageBoxW(nullptr, err.c_str(), Tr(L"Обновление Zipshot", L"Zipshot Update"),
                                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                });
            };

            wchar_t tmpBuf[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmpBuf);
            std::wstring safeName = PathFindFileNameW(info.assetName.c_str());
            if (safeName.empty()) safeName = L"zipshot-update.exe";
            std::wstring dest = std::wstring(tmpBuf) + L"zipshot_upd_" + info.version + L"_" + safeName;
            std::wstring part = dest + L".part";
            DeleteFileW(part.c_str());

            std::wstring host, path;
            INTERNET_PORT port = 0;
            bool https = true;
            if (!CrackUrl(info.downloadUrl, host, path, port, https)) {
                fail(Tr(L"Некорректная ссылка на обновление.", L"Invalid update download URL."));
                return;
            }
            WinHttpHandle ses = OpenSession();
            if (!ses) { fail(Tr(L"Не удалось инициализировать WinHTTP.", L"Could not initialize WinHTTP.")); return; }
            WinHttpSetTimeouts(ses.h, 15000, 15000, 30000, 60000);
            WinHttpHandle conn(WinHttpConnect(ses.h, host.c_str(), port, 0));
            if (!conn) { fail(Tr(L"Не удалось подключиться к серверу загрузки.", L"Could not connect to download server.")); return; }
            WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
            if (!req) { fail(Tr(L"Не удалось создать HTTP-запрос.", L"Could not create HTTP request.")); return; }
            if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(req.h, nullptr)) {
                fail(Tr(L"Не удалось начать загрузку обновления.", L"Could not start update download."));
                return;
            }
            DWORD statusCode = 0, sz = sizeof(statusCode);
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
            if (statusCode < 200 || statusCode >= 300) {
                fail(std::wstring(Tr(L"Сервер вернул ошибку HTTP ", L"Server returned HTTP ")) + std::to_wstring(statusCode));
                return;
            }
            wchar_t clBuf[64]{};
            DWORD clSz = sizeof(clBuf);
            uint64_t total = info.assetSize;
            if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clBuf, &clSz,
                                    WINHTTP_NO_HEADER_INDEX)) {
                uint64_t parsed = _wcstoui64(clBuf, nullptr, 10);
                if (parsed > 0) total = parsed;
            }

            HANDLE outFile = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (outFile == INVALID_HANDLE_VALUE) {
                fail(Tr(L"Не удалось создать временный файл обновления.", L"Could not create temporary update file."));
                return;
            }
            std::vector<BYTE> buf(128 * 1024);
            uint64_t readTotal = 0;
            ULONGLONG lastUi = 0;
            bool writeOk = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) { writeOk = false; break; }
                if (!got) break;
                DWORD wr = 0;
                if (!WriteFile(outFile, buf.data(), got, &wr, nullptr) || wr != got) { writeOk = false; break; }
                readTotal += got;
                ULONGLONG now = GetTickCount64();
                if (now - lastUi > 100) {
                    lastUi = now;
                    double prog = total > 0 ? std::clamp((double)readTotal / (double)total, 0.0, 1.0) : 0.0;
                    std::wstring st = total > 0
                        ? std::wstring(Tr(L"Скачивание ", L"Downloading ")) + info.version + L": " + FormatBytes(readTotal) + L" / " +
                              FormatBytes(total) + L" (" + std::to_wstring((int)(prog * 100)) + L"%)"
                        : std::wstring(Tr(L"Скачивание ", L"Downloading ")) + info.version + L": " + FormatBytes(readTotal);
                    RunOnMain([prog, st] {
                        {
                            std::lock_guard<std::mutex> lk(S().mu);
                            S().progress = prog;
                            S().status = st;
                        }
                        MainWindowRefresh();
                    });
                }
            }
            CloseHandle(outFile);
            if (!writeOk || readTotal == 0) {
                DeleteFileW(part.c_str());
                fail(Tr(L"Загрузка обновления прервана.", L"Update download was interrupted."));
                return;
            }
            if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(part.c_str());
                fail(Tr(L"Не удалось сохранить файл обновления.", L"Could not finalize downloaded update file."));
                return;
            }

            // Optional SHA-256 verification if the release publishes SHA256SUMS.txt.
            if (!info.sha256Url.empty()) {
                DWORD sumCode = 0;
                std::string sums;
                std::string expected;
                if (HttpGetText(info.sha256Url, L"text/plain", 10000, sumCode, sums) &&
                    ParseSha256ForAsset(sums, info.assetName, expected)) {
                    std::string actual;
                    if (!ComputeFileSha256(dest, actual) || actual != expected) {
                        DeleteFileW(dest.c_str());
                        fail(Tr(L"Контрольная сумма SHA-256 обновления не совпала. Установка отменена.",
                                L"Update SHA-256 checksum mismatch. Installation aborted."));
                        return;
                    }
                }
            }

            RunOnMain([ver = info.version] {
                {
                    std::lock_guard<std::mutex> lk(S().mu);
                    S().progress = 1.0;
                    S().status = std::wstring(Tr(L"Устанавливаю ", L"Installing ")) + ver + L"…";
                }
                MainWindowRefresh();
            });

            if (info.kind == AssetKind::InstallerExe) {
                if (!HasHeaderMagic(dest, "MZ", 2)) {
                    DeleteFileW(dest.c_str());
                    fail(Tr(L"Скачанный установщик повреждён.", L"Downloaded installer is invalid."));
                    return;
                }
                HINSTANCE r = ShellExecuteW(nullptr, L"open", dest.c_str(),
                                            L"/SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART", nullptr, SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32) {
                    fail(Tr(L"Не удалось запустить установщик обновления.", L"Could not launch the update installer."));
                    return;
                }
                Sleep(300);
                RunOnMain([] { RunCommand(C_Quit); });
                return;
            }

            std::wstring newExe = dest;
            if (info.kind == AssetKind::PortableZip) {
                if (!HasHeaderMagic(dest, "PK\x03\x04", 4)) {
                    DeleteFileW(dest.c_str());
                    fail(Tr(L"Скачанный архив повреждён.", L"Downloaded archive is invalid."));
                    return;
                }
                std::wstring extractDir = std::wstring(tmpBuf) + L"zipshot_upd_" + info.version + L"_ext";
                if (!ExtractExeFromZip(dest, extractDir, newExe)) {
                    DeleteFileW(dest.c_str());
                    fail(Tr(L"Не удалось распаковать zipshot.exe из архива.", L"Could not extract zipshot.exe from the archive."));
                    return;
                }
                DeleteFileW(dest.c_str());
            }

            if (!HasHeaderMagic(newExe, "MZ", 2)) {
                DeleteFileW(newExe.c_str());
                fail(Tr(L"Скачанный исполняемый файл повреждён.", L"Downloaded executable is invalid."));
                return;
            }
            if (!SwapAndRestart(newExe)) {
                fail(Tr(L"Не удалось заменить zipshot.exe (нет прав на запись в папку программы).",
                        L"Could not replace zipshot.exe (no write access to the program folder)."));
                return;
            }
            RunOnMain([] { RunCommand(C_Quit); });
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lk(S().mu);
        S().downloading = false;
        S().progress = 0.0;
        S().status = DefaultStatus();
        S().statusInitialized = true;
    }
}

bool Checking() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().checking;
}
bool Downloading() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().downloading;
}
bool HasUpdate() {
    std::lock_guard<std::mutex> lk(S().mu);
    return !S().available.version.empty() && !S().available.downloadUrl.empty();
}
double Progress() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().progress;
}
std::wstring AvailableVersion() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().available.version;
}
std::wstring StatusText() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().statusInitialized ? S().status : DefaultStatus();
}

}  // namespace updater
