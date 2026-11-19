#pragma once

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Metadata only: callers own thumbnails, files and selection UI. Returned indices
// refer to the original input, which is never reordered or truncated.
namespace gallery {

enum class Kind { Image, Gif, Video };
enum class Filter { All, Images, Gifs, Videos, Shelf };

struct Item {
    std::wstring path, name;
    uint64_t time = 0;
    Kind kind = Kind::Image;
    bool shelf = false;
};

namespace detail {

inline int Compare(const std::wstring& left, const std::wstring& right, bool ignoreCase) {
    const int result = CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1,
                                           ignoreCase ? TRUE : FALSE);
    // Keep a strict, deterministic order even if Windows rejects a comparison.
    return result ? result - CSTR_EQUAL : left.compare(right);
}

inline bool Matches(const Item& item, const std::wstring& query, Filter filter) {
    switch (filter) {
    case Filter::All: break;
    case Filter::Images: if (item.kind != Kind::Image) return false; break;
    case Filter::Gifs: if (item.kind != Kind::Gif) return false; break;
    case Filter::Videos: if (item.kind != Kind::Video) return false; break;
    case Filter::Shelf: if (!item.shelf) return false; break;
    default: return false;
    }
    return query.empty() || FindStringOrdinal(FIND_FROMSTART, item.name.c_str(), -1,
                                              query.c_str(), -1, TRUE) >= 0;
}

}  // namespace detail

// Only the time direction changes with oldestFirst. Equal times always use
// ascending ordinal name/path order, ignoring case first, then exact case.
// An exact duplicate retains its original input order. GIFs are a separate kind
// from still images; Shelf accepts every marked kind, including pasted images.
inline std::vector<size_t> Select(const std::vector<Item>& items, std::wstring query,
                                  Filter filter, bool oldestFirst) {
    std::vector<size_t> selected;
    selected.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index)
        if (detail::Matches(items[index], query, filter)) selected.push_back(index);

    std::sort(selected.begin(), selected.end(), [&](size_t left, size_t right) {
        const Item& a = items[left];
        const Item& b = items[right];
        if (a.time != b.time) return oldestFirst ? a.time < b.time : a.time > b.time;
        int order = detail::Compare(a.name, b.name, true);
        if (!order) order = detail::Compare(a.path, b.path, true);
        if (!order) order = detail::Compare(a.name, b.name, false);
        if (!order) order = detail::Compare(a.path, b.path, false);
        return order ? order < 0 : left < right;
    });
    return selected;
}

}  // namespace gallery
