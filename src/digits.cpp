#include "digits.h"

namespace digits {
namespace {

// 5x7 font, one string per row, '#' = pixel set.
const char* const kFont[10][kGlyphH] = {
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}, // 0
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}, // 1
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}, // 2
    {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}, // 3
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}, // 4
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}, // 5
    {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}, // 6
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}, // 7
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}, // 8
    {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}, // 9
};

} // namespace

Stamp render(uint64_t value)
{
    Stamp s{};
    unsigned v = static_cast<unsigned>(value % 10000);
    const unsigned d[kDigits] = {v / 1000, v / 100 % 10, v / 10 % 10, v % 10};

    for (int i = 0; i < kDigits; ++i) {
        const int x0 = kOutline + i * (kGlyphW + kGap);
        for (int y = 0; y < kGlyphH; ++y)
            for (int x = 0; x < kGlyphW; ++x)
                if (kFont[d[i]][y][x] == '#')
                    s[(kOutline + y) * kWidth + x0 + x] = kWhite;
    }

    // Outline: every empty pixel touching a white pixel (8-neighbourhood).
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (s[y * kWidth + x] != kNone)
                continue;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int ny = y + dy, nx = x + dx;
                    if (ny >= 0 && ny < kHeight && nx >= 0 && nx < kWidth && s[ny * kWidth + nx] == kWhite)
                        s[y * kWidth + x] = kBlack;
                }
            }
        }
    }
    return s;
}

} // namespace digits
