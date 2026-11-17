#pragma once
#include "common.h"
#include <cstring>

namespace shapes_ext::detail {

// CF_DIB has no file header. Validate its bounded header/palette before asking WIC to decode it.
// Does not access the system clipboard, so malformed input can be tested in isolation.
inline bool WrapClipboardDib(const void* data, size_t size, std::vector<BYTE>& file, WORD& bitCount) {
    if (!data || size < sizeof(BITMAPINFOHEADER) || size > MAXDWORD - sizeof(BITMAPFILEHEADER)) return false;
    BITMAPINFOHEADER bi;
    std::memcpy(&bi, data, sizeof(bi));
    if (bi.biSize < sizeof(BITMAPINFOHEADER) || bi.biSize > size) return false;
    DWORD extra = bi.biCompression == BI_BITFIELDS && bi.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    DWORD colors = bi.biBitCount <= 8 ? (bi.biClrUsed ? bi.biClrUsed : 1u << bi.biBitCount) : bi.biClrUsed;
    size_t pixelOffset = sizeof(BITMAPFILEHEADER) + (size_t)bi.biSize + extra + (size_t)colors * 4;
    if (pixelOffset > sizeof(BITMAPFILEHEADER) + size) return false;
    file.assign(sizeof(BITMAPFILEHEADER) + size, 0);
    auto* fh = reinterpret_cast<BITMAPFILEHEADER*>(file.data());
    fh->bfType = 0x4D42;
    fh->bfSize = (DWORD)file.size();
    fh->bfOffBits = (DWORD)pixelOffset;
    std::memcpy(file.data() + sizeof(BITMAPFILEHEADER), data, size);
    bitCount = bi.biBitCount;
    return true;
}

}  // namespace shapes_ext::detail
