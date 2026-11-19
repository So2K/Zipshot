// Synthetic metadata only: no windows, disk scans, clipboard or user settings.
#include "../src/gallery_model.h"
#include <cstdio>
#include <limits>

namespace {
int checks = 0, failures = 0;

void Check(bool condition, const char* message) {
    ++checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

void Expect(const std::vector<gallery::Item>& items, const wchar_t* query,
            gallery::Filter filter, bool oldestFirst,
            std::initializer_list<size_t> expected, const char* message) {
    Check(gallery::Select(items, query, filter, oldestFirst) == std::vector<size_t>(expected), message);
}

bool SameMetadata(const std::vector<gallery::Item>& a, const std::vector<gallery::Item>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].name != b[i].name || a[i].time != b[i].time ||
            a[i].kind != b[i].kind || a[i].shelf != b[i].shelf) return false;
    }
    return true;
}
}  // namespace

int main() {
    using gallery::Filter;
    using gallery::Item;
    using gallery::Kind;
    const std::vector<Item> mixed = {
        {L"C:\\captures\\СНИМОК_Москва.PNG", L"СНИМОК_Москва.PNG", 300, Kind::Image, false},
        {L"C:\\captures\\demo.gif", L"demo.gif", 100, Kind::Gif, false},
        {L"C:\\captures\\Demo.MP4", L"Demo.MP4", 200, Kind::Video, false},
        {L"", L"Вставка_Пример.png", 400, Kind::Image, true},
        {L"C:\\captures\\Résumé.JpG", L"Résumé.JpG", 150, Kind::Image, false},
        {L"C:\\captures\\animation.GIF", L"animation.GIF", 250, Kind::Gif, true},
        {L"C:\\captures\\shelf-movie.mp4", L"shelf-movie.mp4", 350, Kind::Video, true},
    };
    const auto mixedBefore = mixed;
    Expect({}, L"", Filter::All, false, {}, "empty history has no results");
    Expect(mixed, L"", Filter::All, false, {3, 6, 0, 5, 2, 4, 1}, "empty query includes all kinds, newest first");
    Expect(mixed, L"", Filter::All, true, {1, 4, 2, 5, 0, 6, 3}, "oldest first sorts mixed capture kinds");
    Expect(mixed, L"мОсКвА.PnG", Filter::All, false, {0}, "Cyrillic substring and extension ignore case");
    Expect(mixed, L"снимок", Filter::Images, false, {0}, "Cyrillic prefix ignores case");
    Expect(mixed, L"réSUMÉ.jPg", Filter::All, false, {4}, "accented Latin filename ignores case");
    Expect(mixed, L"dEmO", Filter::All, false, {2, 1}, "Latin substring spans video and GIF kinds");
    Expect(mixed, L".mP4", Filter::All, false, {6, 2}, "extension-only search includes both videos");
    Expect(mixed, L"captures", Filter::All, false, {}, "search uses filename rather than parent folder");
    Expect(mixed, L"Москва.PNG.extra", Filter::All, false, {}, "query longer than filename has no match");
    Expect(mixed, L"", Filter::Images, false, {3, 0, 4}, "Images excludes GIFs and videos");
    Expect(mixed, L"", Filter::Gifs, false, {5, 1}, "GIF filter excludes stills and videos");
    Expect(mixed, L"", Filter::Videos, true, {2, 6}, "Videos filter respects oldest first");
    Expect(mixed, L"dEmO", Filter::Images, false, {}, "filename match does not bypass kind filter");
    Expect(mixed, L".GiF", Filter::Gifs, false, {5, 1}, "GIF extension query combines with GIF filter");
    Expect(mixed, L"", Filter::Shelf, false, {3, 6, 5}, "Shelf includes marked still, GIF and video");
    Expect(mixed, L"вСтАвКа", Filter::Shelf, true, {3}, "Shelf search includes a pathless pasted image");
    Expect(mixed, L".gif", Filter::Shelf, false, {5}, "Shelf query excludes unmarked matching GIF");
    Check(SameMetadata(mixed, mixedBefore), "queries and sorting leave input metadata untouched");

    const std::vector<Item> ties = {
        {L"D:\\z\\beta.png", L"Beta.png", 42, Kind::Video, false},
        {L"D:\\b\\alpha.png", L"alpha.png", 42, Kind::Gif, false},
        {L"D:\\a\\Alpha.png", L"ALPHA.PNG", 42, Kind::Image, false},
        {L"D:\\a\\alpha.png", L"alpha.png", 42, Kind::Image, true},
        {L"D:\\a\\alpha.png", L"alpha.png", 42, Kind::Image, true},
        {L"D:\\later.png", L"zz-later.png", 99, Kind::Gif, false},
    };
    Expect(ties, L"", Filter::All, false, {5, 2, 3, 4, 1, 0}, "equal times use names and paths with stable exact duplicates");
    Expect(ties, L"", Filter::All, true, {2, 3, 4, 1, 0, 5}, "oldest direction preserves deterministic tie ordering");
    Expect(ties, L"alpha", Filter::All, false, {2, 3, 4, 1}, "search preserves case-insensitive name/path tie ordering");
    const std::vector<Item> extremes = {
        {L"zero", L"zero.png", 0},
        {L"max", L"max.png", (std::numeric_limits<uint64_t>::max)()},
        {L"one", L"one.png", 1},
    };
    Expect(extremes, L"", Filter::All, false, {1, 2, 0}, "large unsigned timestamps sort without subtraction overflow");

    std::vector<Item> history;
    for (size_t i = 0; i < 750; ++i) {
        const std::wstring name = L"capture_" + std::to_wstring(i) + L".png";
        history.push_back({L"C:\\history\\" + name, name, 100 + i});
    }
    history[0].name = L"Архив_Нужный.PNG";
    history[0].time = 1;
    const auto historyBefore = history;
    Check(gallery::Select(history, L"", Filter::All, false).size() == 750, "history is not capped at 500 entries");
    Expect(history, L"нУжНыЙ.PnG", Filter::All, false, {0}, "search finds an older file beyond the newest 500");
    Check(SameMetadata(history, historyBefore), "large history selection preserves every input field");
    history.push_back({L"C:\\history\\new.mp4", L"new.mp4", 1000000, Kind::Video, false});
    Expect(history, L"нужный", Filter::Images, true, {0}, "later capture does not hide an old search result");
    Expect(history, L"", Filter::Videos, false, {750}, "later video is visible through its kind filter");
    const auto newest = gallery::Select(history, L"", Filter::All, false);
    Check(newest.size() == 751 && newest.front() == 750 && newest.back() == 0,
          "new capture joins complete history in timestamp order");

    std::printf("%d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
