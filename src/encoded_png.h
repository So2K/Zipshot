#pragma once
#include "common.h"

// One immutable capture task and its encoded PNG. The packet retains the source
// Image for its entire lifetime; callers must not mutate aliases of that Image.
// Consumers take the packet alone, preventing mismatched-image byte reuse.
class PreparedPng;
using PreparedPngPtr = std::shared_ptr<const PreparedPng>;

// Same WIC encoding as the ordinary output path; null on invalid input/failure.
PreparedPngPtr PreparePngWic(std::shared_ptr<const Image> image);
// Fast lossless for alpha; opaque automatic-JPEG policy still uses WIC bytes.
PreparedPngPtr PreparePng(std::shared_ptr<const Image> image);
// Optional freshness check runs with the clipboard open, before EmptyClipboard.
void CopyPreparedImageToClipboard(const PreparedPngPtr& prepared, std::function<bool()> allow = {});
bool SavePreparedImageAuto(const PreparedPngPtr& prepared, std::wstring& path);
