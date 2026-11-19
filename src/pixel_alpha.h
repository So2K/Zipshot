#pragma once
#include <cstddef>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

// Image pixels are straight BGRA. AND preserves an all-255 alpha byte only
// when every pixel is opaque. SSE2 is guaranteed by the Windows x64 ABI.
inline bool PixelsHaveTransparency(const uint32_t* pixels, size_t count) {
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i alpha = _mm_set1_epi32((int)0xff000000u);
    for (; count - i >= 16; i += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i + 4));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i + 8));
        const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i + 12));
        const __m128i bits = _mm_and_si128(_mm_and_si128(a, b), _mm_and_si128(c, d));
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(bits, alpha), alpha)) != 0xffff) return true;
    }
#endif
    for (; i < count; ++i) if ((pixels[i] >> 24) != 255) return true;
    return false;
}
