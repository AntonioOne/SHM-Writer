// Four-digit counter stamp drawn with a built-in 5x7 pixel font.
#pragma once

#include <array>
#include <cstdint>

namespace digits {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kDigits = 4;
constexpr int kGap = 1;
constexpr int kOutline = 1;

// Stamp size including the black outline: 25 x 9 pixels.
constexpr int kWidth = kDigits * kGlyphW + (kDigits - 1) * kGap + 2 * kOutline;
constexpr int kHeight = kGlyphH + 2 * kOutline;

enum Pixel : uint8_t { kNone = 0, kWhite = 1, kBlack = 2 };

// Stamp pixels in display orientation (row 0 = top), kWidth x kHeight.
using Stamp = std::array<uint8_t, kWidth * kHeight>;

// White digits of (value % 10000), zero-padded, with a 1-pixel black outline.
Stamp render(uint64_t value);

} // namespace digits
