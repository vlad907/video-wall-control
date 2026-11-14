// Identify: a still picture that says which screen this is (a big number and a few lines of
// text on a coloured background, with a white frame along the edges of the picture), shown
// instead of the display's stream while the wall is being mapped. Drawn here with a built-in
// 5 x 7 font, so it needs nothing from the system and looks the same everywhere.
#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "frames.hpp"

namespace mosaic::rx {

// A character as 7 rows of 5 dots ('1' = set), top row first. Letters are upper case only.
inline const char* identifyGlyph(char ch) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case ' ': return "00000000000000000000000000000000000";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11111000100010000010000011000101110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001111000001000011000101110";
        case '6': return "00110010001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010001001100";
        case 'A': return "01110100011000111111100011000110001";
        case 'B': return "11110100011000111110100011000111110";
        case 'C': return "01110100011000010000100001000101110";
        case 'D': return "11100100101000110001100011001011100";
        case 'E': return "11111100001000011110100001000011111";
        case 'F': return "11111100001000011110100001000010000";
        case 'G': return "01110100011000010111100011000101111";
        case 'H': return "10001100011000111111100011000110001";
        case 'I': return "01110001000010000100001000010001110";
        case 'J': return "00111000100001000010000101001001100";
        case 'K': return "10001100101010011000101001001010001";
        case 'L': return "10000100001000010000100001000011111";
        case 'M': return "10001110111010110101100011000110001";
        case 'N': return "10001100011100110101100111000110001";
        case 'O': return "01110100011000110001100011000101110";
        case 'P': return "11110100011000111110100001000010000";
        case 'Q': return "01110100011000110001101011001001101";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'U': return "10001100011000110001100011000101110";
        case 'V': return "10001100011000110001100010101000100";
        case 'W': return "10001100011000110101101011010101010";
        case 'X': return "10001100010101000100010101000110001";
        case 'Y': return "10001100011000101010001000010000100";
        case 'Z': return "11111000010001000100010001000011111";
        case '-': return "00000000000000011111000000000000000";
        case '_': return "00000000000000000000000000000011111";
        case '.': return "00000000000000000000000000110001100";
        case ',': return "00000000000000000000011000010001000";
        case ':': return "00000011000110000000011000110000000";
        case '/': return "00001000010001000100010001000010000";
        case '(': return "00010001000100001000010000010000010";
        case ')': return "01000001000001000010000100010001000";
        case '#': return "01010010101111101010111110101001010";
        case '+': return "00000001000010011111001000010000000";
        default: return "01110100010000100010001000000000100";  // ?
    }
}

struct IdentifyOptions {
    int width = 960, height = 540;
    std::string big;                 // the number, as large as fits
    std::vector<std::string> lines;  // smaller lines under it (up to four are shown)
    uint32_t colour = 0xFF1E3A8A;    // background, 0xAARRGGBB
};

// Backgrounds that are easy to tell apart from across a room, dark enough for white text.
inline uint32_t identifyColour(int n) {
    static const uint32_t colours[] = {0xFF1E3A8A, 0xFF7F1D1D, 0xFF14532D, 0xFF581C87, 0xFF7C4A03, 0xFF134E4A, 0xFF9D174D, 0xFF374151};
    return colours[static_cast<size_t>(std::max(n, 0)) % (sizeof colours / sizeof colours[0])];
}

// The picture, as 0xAARRGGBB pixels, row by row from the top.
inline std::vector<uint32_t> renderIdentify(const IdentifyOptions& o) {
    const int W = std::max(o.width, 64), H = std::max(o.height, 64);
    const uint32_t white = 0xFFFFFFFF;
    std::vector<uint32_t> px(static_cast<size_t>(W) * H, o.colour | 0xFF000000);
    auto fill = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        x0 = std::clamp(x0, 0, W), x1 = std::clamp(x1, 0, W), y0 = std::clamp(y0, 0, H), y1 = std::clamp(y1, 0, H);
        for (int y = y0; y < y1; ++y) std::fill(px.begin() + static_cast<std::ptrdiff_t>(y) * W + x0, px.begin() + static_cast<std::ptrdiff_t>(y) * W + x1, c);
    };
    // A white frame right at the edges: if any side of it is missing on the screen, the window
    // doesn't fill that screen (or the screen overscans).
    const int b = std::max(2, std::min(W, H) / 90);
    fill(0, 0, W, b, white), fill(0, H - b, W, H, white), fill(0, 0, b, H, white), fill(W - b, 0, W, H, white);
    // Text centred on `cx`, its top at `top`, as large as fits in maxW x maxH (whole dots only).
    auto text = [&](const std::string& s, int top, int maxW, int maxH) {
        if (s.empty()) return;
        const int n = static_cast<int>(s.size());
        const int dot = std::max(1, std::min(maxH / 7, maxW / (6 * n - 1)));
        int x = (W - (6 * n - 1) * dot) / 2;
        for (char ch : s) {
            const char* g = identifyGlyph(ch);
            for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c)
                    if (g[r * 5 + c] == '1') fill(x + c * dot, top + r * dot, x + (c + 1) * dot, top + (r + 1) * dot, white);
            x += 6 * dot;
        }
    };
    text(o.big, H * 10 / 100, W * 80 / 100, H * 44 / 100);
    for (size_t i = 0; i < o.lines.size() && i < 4; ++i)
        text(o.lines[i], H * (60 + 9 * static_cast<int>(i)) / 100, W * 90 / 100, H * 6 / 100);
    return px;
}

// The picture as a stream of one frame: it has no engine frame id, so it's shown as it is and
// never waited for or counted late.
class IdentifyInput : public FrameInput {
public:
    explicit IdentifyInput(const IdentifyOptions& o) {
        auto pixels = std::make_shared<std::vector<uint32_t>>(renderIdentify(o));
        RxFrame f;
        f.width = std::max(o.width, 64);
        f.height = std::max(o.height, 64);
        f.stride = f.width * 4;
        f.data = reinterpret_cast<const uint8_t*>(pixels->data());
        f.hold = pixels;
        f.arrived = Clock::now();
        queue_.push(std::move(f));
    }
    StreamQueue& queue() override { return queue_; }
    std::string status() const override { return "identify"; }

private:
    StreamQueue queue_;
};

}  // namespace mosaic::rx
