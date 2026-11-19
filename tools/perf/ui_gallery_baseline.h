#pragma once
// Verbatim pre-optimization RebuildView from1fdf0a7; renamed only for paired profiling.
void BaselineRebuildView(bool resetScroll = false) {
    const auto pathAt = [](int at) -> std::wstring {
        return at >= 0 && at < (int)g_mw->files.size() ? g_mw->files[at].path : L"";
    };
    const std::wstring selected = pathAt(g_mw->sel), anchor = pathAt(g_mw->anchor);
    std::vector<Entry> v;
    for (size_t at : gallery::Select(g_mw->catalog, g_mw->query, g_mw->filter, g_mw->oldestFirst)) {
        Entry e;
        static_cast<gallery::Item&>(e) = g_mw->catalog[at];
        v.push_back(std::move(e));
    }
    // Keep thumbnails of files that are still listed.
    std::map<std::wstring, Entry*> old;
    for (Entry& e : g_mw->files) old[e.path] = &e;
    for (Entry& e : v) {
        auto it = old.find(e.path);
        if (it != old.end() && it->second->time == e.time) {
            std::swap(e.thumb, it->second->thumb);
            e.tw = it->second->tw;
            e.th = it->second->th;
            e.done = it->second->done;
        }
    }
    FreeAllThumbs();
    g_mw->files = std::move(v);
    g_mw->sel = g_mw->anchor = -1;
    for (int i = 0; i < (int)g_mw->files.size(); ++i) {
        if (g_mw->files[i].path == selected) g_mw->sel = i;
        if (g_mw->files[i].path == anchor) g_mw->anchor = i;
    }
    std::erase_if(g_mw->picked, [](const std::wstring& p) {
        return std::none_of(g_mw->files.begin(), g_mw->files.end(), [&](const Entry& e) { return e.path == p; });
    });
    g_mw->hoverTile = g_mw->dropTile = g_mw->downTile = -1;
    g_mw->dragArmed = g_mw->collapseOnUp = g_mw->scrollDrag = false;
    if (GetCapture() == g_mw->hwnd) ReleaseCapture();
    if (resetScroll) g_mw->scroll = 0;
    ClampScroll();
    LayoutToolbar();
    InvalidateRect(g_mw->hwnd, nullptr, FALSE);
}
