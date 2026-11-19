#pragma once
#include "common.h"

namespace shelf {

struct Payload {
    Image image;  // Straight BGRA, as used by normal image documents.
    std::vector<std::wstring> files;
};

// Called only by an explicit Paste action. Never monitors or modifies the clipboard.
bool ReadClipboard(HWND owner, Payload& out, std::wstring& error);
std::wstring Directory();
bool IsShelfPath(const std::wstring& path);

struct SaveResult {
    std::vector<std::wstring> paths;
    std::wstring error;  // May be nonempty alongside successful paths.
};

// Background-safe; initializes COM itself. Files take precedence over a bitmap preview.
// Reserves CREATE_NEW temporary siblings, then publishes completed files without replacement.
SaveResult Store(Payload data);

}  // namespace shelf
