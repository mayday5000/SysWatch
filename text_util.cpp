#include "text_util.hpp"
#include <cstddef>
#include <cstdint>

namespace {
// Length of a well-formed UTF-8 sequence at s[i] whose code point is not a
// C1 control, or 0 if the bytes there are not one.
size_t utf8_len(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[i + k]); };
    const auto cont = [&](size_t k) { return i + k < s.size() && (b(k) & 0xC0) == 0x80; };
    const uint8_t c = b(0);
    uint32_t cp = 0;
    size_t len = 0;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
    else return 0;
    for (size_t k = 1; k < len; k++) {
        if (!cont(k)) return 0;
        cp = (cp << 6) | (b(k) & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
        return 0;                       // overlong / out of range
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;   // surrogates
    if (cp >= 0x80 && cp <= 0x9F) return 0;       // C1 controls (8-bit CSI etc.)
    return len;
}
}

std::string sanitize_terminal(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<uint8_t>(text[i]);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            i++;
            continue;
        }
        if (c >= 0x80) {
            if (size_t n = utf8_len(text, i)) {
                out.append(text.substr(i, n));
                i += n;
                continue;
            }
        }
        out.push_back('?');
        i++;
    }
    return out;
}
