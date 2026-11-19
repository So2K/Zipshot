#include "../src/pixel_alpha.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

static bool Reference(const uint32_t* pixels, size_t count) {
    return std::any_of(pixels, pixels + count, [](uint32_t pixel) { return (pixel >> 24) != 255; });
}
static volatile unsigned sink = 0;
int main(int argc, char** argv) {
    size_t assertions = 0, failures = 0;
    for (size_t offset = 0; offset < 4; ++offset) for (size_t count = 0; count < 97; ++count) {
        std::vector<uint32_t> pixels(count + 8, 0xff123456u);
        auto check = [&] {
            const auto original = pixels;
            const bool expected = Reference(pixels.data() + offset, count);
            const bool actual = PixelsHaveTransparency(pixels.data() + offset, count);
            ++assertions;
            if (expected != actual || original != pixels) ++failures;
        };
        check();
        if (!count) continue;
        for (unsigned alpha = 0; alpha < 256; ++alpha) {
            for (size_t at : {size_t(0), count / 2, count - 1}) {
                pixels[offset + at] = (alpha << 24) | 0x00123456u;
                check();
                pixels[offset + at] = 0xff123456u;
            }
        }
    }
    std::printf("pixel-alpha assertions=%zu failures=%zu\n", assertions, failures);
    if (failures || argc < 2 || std::strcmp(argv[1], "--profile")) return failures ? 1 : 0;
    std::puts("pixels,case,trial,reference_ms,simd_ms");
    for (size_t count : {size_t(1920 * 1080), size_t(3840 * 2160)}) {
        std::vector<uint32_t> pixels(count, 0xff123456u);
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            std::fill(pixels.begin(), pixels.end(), 0xff123456u);
            if (scenario == 1) pixels.back() &= 0x7fffffffu;
            if (scenario == 2) pixels.front() &= 0x7fffffffu;
            for (unsigned trial = 0; trial < 30; ++trial) {
                double times[2]{};
                for (unsigned pass = 0; pass < 2; ++pass) {
                    const unsigned method = pass ^ (trial & 1);
                    const auto start = std::chrono::steady_clock::now();
                    const bool result = method ? PixelsHaveTransparency(pixels.data(), count) : Reference(pixels.data(), count);
                    times[method] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    sink = sink + unsigned(result);
                }
                std::printf("%zu,%u,%u,%.6f,%.6f\n", count, scenario, trial, times[0], times[1]);
            }
        }
    }
    return 0;
}
