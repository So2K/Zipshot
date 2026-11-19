#pragma once
#include "common.h"

// Exact 8-bit lossless PNG; same default sRGB/gAMA metadata as WIC. No file IO.
// Leaves output unchanged on unsupported dimensions, allocation or codec failure.
bool TryEncodePngFast(const Image& image, std::vector<uint8_t>& output);
