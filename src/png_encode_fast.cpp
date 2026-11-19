#include "png_encode_fast.h"
#include "../third_party/fpng/fpng.h"
#include <cstring>
#include <iterator>
#include <mutex>

bool TryEncodePngFast(const Image& image, std::vector<uint8_t>& output) {
    if (image.w <= 0 || image.h <= 0 || image.w > (1 << 24) || image.h > (1 << 24) ||
        (uint64_t)image.w * image.h != image.px.size() || image.px.size() > UINT_MAX / 4) return false;
    try {
        static std::once_flag initialized;
        // Upstream probes SSE4.1/PCLMUL itself and keeps scalar fallbacks.
        // No AVX/SSE4 requirement is imposed on the rest of this application.
        std::call_once(initialized, fpng::fpng_init);
        const uint32_t channels = image.alpha ? 4 : 3;
        std::vector<uint8_t> rgb(image.px.size() * channels);
        for (size_t i = 0; i < image.px.size(); ++i) {
            const uint32_t pixel = image.px[i];
            rgb[i * channels] = (uint8_t)(pixel >> 16);
            rgb[i * channels + 1] = (uint8_t)(pixel >> 8);
            rgb[i * channels + 2] = (uint8_t)pixel;
            if (channels == 4) rgb[i * 4 + 3] = (uint8_t)(pixel >> 24);
        }
        std::vector<uint8_t> png;
        if (!fpng::fpng_encode_image_to_memory(rgb.data(), image.w, image.h, channels, png, fpng::FPNG_ENCODE_SLOWER) ||
            png.size() < 33 || png.size() > MAXDWORD - 29 ||
            std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) ||
            std::memcmp(png.data() + 12, "IHDR", 4)) return false;
        const auto read32 = [&](size_t at) {
            return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) |
                   (uint32_t(png[at + 2]) << 8) | png[at + 3];
        };
        if (read32(16) != (uint32_t)image.w || read32(20) != (uint32_t)image.h ||
            png[24] != 8 || png[25] != (image.alpha ? 6 : 2)) return false;
        // WIC's current raw Image encoder emits these two standard chunks and
        // no DPI chunk. Retain that interpretation instead of relying on a
        // viewer's untagged-image color defaults. CRCs cover fixed chunk bytes.
        static const uint8_t colorChunks[] = {
            0,0,0,1,'s','R','G','B',0,0xAE,0xCE,0x1C,0xE9,
            0,0,0,4,'g','A','M','A',0,0,0xB1,0x8F,0x0B,0xFC,0x61,0x05
        };
        png.insert(png.begin() + 33, std::begin(colorChunks), std::end(colorChunks));
        output = std::move(png);
        return true;
    } catch (...) {
        return false;
    }
}
