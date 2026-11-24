/**
 * @file prestigenamefold.cpp
 * @brief Readable chat: folds the fancy letters of decorated display names
 *        to plain letters (see prestigenamefold.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigenamefold.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
    typedef uint32_t cp_t;

    struct FoldPair
    {
        cp_t mFrom;
        cp_t mTo;
    };

    bool operator<(const FoldPair& a, const FoldPair& b) { return a.mFrom < b.mFrom; }

    // ---- UTF-8 ------------------------------------------------------------------------------

    void decode_utf8(const std::string& in, std::vector<cp_t>& out)
    {
        out.clear();
        out.reserve(in.size());
        size_t i = 0;
        const size_t n = in.size();
        while (i < n)
        {
            const unsigned char c = (unsigned char)in[i];
            cp_t cp = 0xFFFD;
            size_t len = 1;
            if (c < 0x80)
            {
                cp = c;
            }
            else if ((c & 0xE0) == 0xC0)
            {
                cp = c & 0x1F;
                len = 2;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                cp = c & 0x0F;
                len = 3;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                cp = c & 0x07;
                len = 4;
            }
            else
            {
                // stray continuation byte: kept as the replacement character
                out.push_back(0xFFFD);
                ++i;
                continue;
            }
            if (i + len > n)
            {
                out.push_back(0xFFFD);
                break;
            }
            bool valid = true;
            for (size_t k = 1; k < len; ++k)
            {
                const unsigned char cc = (unsigned char)in[i + k];
                if ((cc & 0xC0) != 0x80)
                {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            out.push_back(valid ? cp : 0xFFFD);
            i += valid ? len : 1;
        }
    }

    void encode_utf8(cp_t cp, std::string& out)
    {
        if (cp < 0x80)
        {
            out += (char)cp;
        }
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    // ---- styled letters: always folded ------------------------------------------------------

    const char LATIN52[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    // One math Greek style (58 code points): capitals with the theta symbol in the place of the
    // unused U+03A2, nabla, small letters (final sigma included), partial differential and the
    // six variant forms.
    cp_t math_greek(cp_t i)
    {
        if (i < 25)
        {
            return i == 17 ? 0x03F4 : 0x0391 + i;
        }
        if (i == 25)
        {
            return 0x2207;
        }
        if (i < 51)
        {
            return 0x03B1 + (i - 26);
        }
        static const cp_t tail[] = { 0x2202, 0x03F5, 0x03D1, 0x03F0, 0x03D5, 0x03F1, 0x03D6 };
        return tail[i - 51];
    }

    // Letterlike symbols (the letters missing from the math block live here), small capitals,
    // modifier, superscript and subscript letters and digits.
    const FoldPair STYLED_PAIRS[] =
    {
        // letterlike symbols
        { 0x2102, 'C' }, { 0x2107, 'E' }, { 0x210A, 'g' }, { 0x210B, 'H' }, { 0x210C, 'H' },
        { 0x210D, 'H' }, { 0x210E, 'h' }, { 0x210F, 'h' }, { 0x2110, 'I' }, { 0x2111, 'I' },
        { 0x2112, 'L' }, { 0x2113, 'l' }, { 0x2115, 'N' }, { 0x2118, 'P' }, { 0x2119, 'P' },
        { 0x211A, 'Q' }, { 0x211B, 'R' }, { 0x211C, 'R' }, { 0x211D, 'R' }, { 0x2124, 'Z' },
        { 0x2128, 'Z' }, { 0x212A, 'K' }, { 0x212B, 0x00C5 }, { 0x212C, 'B' }, { 0x212D, 'C' },
        { 0x212F, 'e' }, { 0x2130, 'E' }, { 0x2131, 'F' }, { 0x2133, 'M' }, { 0x2134, 'o' },
        { 0x2139, 'i' }, { 0x2145, 'D' }, { 0x2146, 'd' }, { 0x2147, 'e' }, { 0x2148, 'i' },
        { 0x2149, 'j' },
        // small capitals
        { 0x1D00, 'a' }, { 0x0299, 'b' }, { 0x1D04, 'c' }, { 0x1D05, 'd' }, { 0x1D07, 'e' },
        { 0xA730, 'f' }, { 0x0262, 'g' }, { 0x029C, 'h' }, { 0x026A, 'i' }, { 0x1D0A, 'j' },
        { 0x1D0B, 'k' }, { 0x029F, 'l' }, { 0x1D0D, 'm' }, { 0x0274, 'n' }, { 0x1D0F, 'o' },
        { 0x1D18, 'p' }, { 0x0280, 'r' }, { 0xA731, 's' }, { 0x1D1B, 't' }, { 0x1D1C, 'u' },
        { 0x1D20, 'v' }, { 0x1D21, 'w' }, { 0x028F, 'y' }, { 0x1D22, 'z' }, { 0x0276, 'e' },
        // modifier (superscript) letters
        { 0x1D43, 'a' }, { 0x1D47, 'b' }, { 0x1D9C, 'c' }, { 0x1D48, 'd' }, { 0x1D49, 'e' },
        { 0x1DA0, 'f' }, { 0x1D4D, 'g' }, { 0x02B0, 'h' }, { 0x2071, 'i' }, { 0x02B2, 'j' },
        { 0x1D4F, 'k' }, { 0x02E1, 'l' }, { 0x1D50, 'm' }, { 0x207F, 'n' }, { 0x1D52, 'o' },
        { 0x1D56, 'p' }, { 0x02B3, 'r' }, { 0x02E2, 's' }, { 0x1D57, 't' }, { 0x1D58, 'u' },
        { 0x1D5B, 'v' }, { 0x02B7, 'w' }, { 0x02E3, 'x' }, { 0x02B8, 'y' }, { 0x1DBB, 'z' },
        { 0x1D2C, 'A' }, { 0x1D2E, 'B' }, { 0x1D30, 'D' }, { 0x1D31, 'E' }, { 0x1D33, 'G' },
        { 0x1D34, 'H' }, { 0x1D35, 'I' }, { 0x1D36, 'J' }, { 0x1D37, 'K' }, { 0x1D38, 'L' },
        { 0x1D39, 'M' }, { 0x1D3A, 'N' }, { 0x1D3C, 'O' }, { 0x1D3E, 'P' }, { 0x1D3F, 'R' },
        { 0x1D40, 'T' }, { 0x1D41, 'U' }, { 0x1D42, 'W' },
        // subscript letters
        { 0x2090, 'a' }, { 0x2091, 'e' }, { 0x2092, 'o' }, { 0x2093, 'x' }, { 0x2095, 'h' },
        { 0x2096, 'k' }, { 0x2097, 'l' }, { 0x2098, 'm' }, { 0x2099, 'n' }, { 0x209A, 'p' },
        { 0x209B, 's' }, { 0x209C, 't' }, { 0x1D62, 'i' }, { 0x1D63, 'r' }, { 0x1D64, 'u' },
        { 0x1D65, 'v' }, { 0x2C7C, 'j' },
        // superscript and subscript digits
        { 0x2070, '0' }, { 0x00B9, '1' }, { 0x00B2, '2' }, { 0x00B3, '3' }, { 0x2074, '4' },
        { 0x2075, '5' }, { 0x2076, '6' }, { 0x2077, '7' }, { 0x2078, '8' }, { 0x2079, '9' },
        { 0x2080, '0' }, { 0x2081, '1' }, { 0x2082, '2' }, { 0x2083, '3' }, { 0x2084, '4' },
        { 0x2085, '5' }, { 0x2086, '6' }, { 0x2087, '7' }, { 0x2088, '8' }, { 0x2089, '9' },
        // ideographic space
        { 0x3000, ' ' },
    };

    // Greek and Cyrillic letters standing in for Latin ones; only used in decorated words.
    const FoldPair LOOKALIKE_PAIRS[] =
    {
        // Greek capitals (with tonos, dialytika) and symbols
        { 0x0386, 'A' }, { 0x0388, 'E' }, { 0x0389, 'H' }, { 0x038A, 'I' }, { 0x038C, 'O' },
        { 0x038E, 'Y' }, { 0x038F, 'O' }, { 0x0391, 'A' }, { 0x0392, 'B' }, { 0x0394, 'A' },
        { 0x0395, 'E' }, { 0x0396, 'Z' }, { 0x0397, 'H' }, { 0x0398, 'O' }, { 0x0399, 'I' },
        { 0x039A, 'K' }, { 0x039B, 'A' }, { 0x039C, 'M' }, { 0x039D, 'N' }, { 0x039E, 'E' },
        { 0x039F, 'O' }, { 0x03A1, 'P' }, { 0x03A3, 'E' }, { 0x03A4, 'T' }, { 0x03A5, 'Y' },
        { 0x03A6, 'O' }, { 0x03A7, 'X' }, { 0x03A8, 'Y' }, { 0x03A9, 'O' }, { 0x03AA, 'I' },
        { 0x03AB, 'Y' }, { 0x03F4, 'O' }, { 0x03DC, 'F' }, { 0x03D2, 'Y' }, { 0x03D3, 'Y' },
        { 0x03D4, 'Y' }, { 0x03F9, 'C' }, { 0x037F, 'J' },
        // Greek small letters (with tonos, dialytika) and symbols
        { 0x0390, 'i' }, { 0x03AC, 'a' }, { 0x03AD, 'e' }, { 0x03AE, 'n' }, { 0x03AF, 'i' },
        { 0x03B0, 'u' }, { 0x03B1, 'a' }, { 0x03B2, 'b' }, { 0x03B3, 'y' }, { 0x03B4, 'd' },
        { 0x03B5, 'e' }, { 0x03B7, 'n' }, { 0x03B8, 'o' }, { 0x03B9, 'i' }, { 0x03BA, 'k' },
        { 0x03BC, 'u' }, { 0x03BD, 'v' }, { 0x03BF, 'o' }, { 0x03C0, 'n' }, { 0x03C1, 'p' },
        { 0x03C2, 'c' }, { 0x03C3, 'o' }, { 0x03C4, 't' }, { 0x03C5, 'u' }, { 0x03C7, 'x' },
        { 0x03C9, 'w' }, { 0x03CA, 'i' }, { 0x03CB, 'u' }, { 0x03CC, 'o' }, { 0x03CD, 'u' },
        { 0x03CE, 'w' }, { 0x03DD, 'f' }, { 0x03F0, 'k' }, { 0x03F1, 'p' }, { 0x03F2, 'c' },
        { 0x03F3, 'j' }, { 0x03F5, 'e' }, { 0x03D1, 'o' }, { 0x03D6, 'w' },
        // Cyrillic capitals
        { 0x0400, 'E' }, { 0x0401, 'E' }, { 0x0404, 'E' }, { 0x0405, 'S' }, { 0x0406, 'I' },
        { 0x0407, 'I' }, { 0x0408, 'J' }, { 0x040C, 'K' }, { 0x040D, 'N' }, { 0x040E, 'Y' },
        { 0x0410, 'A' }, { 0x0411, 'B' }, { 0x0412, 'B' }, { 0x0414, 'A' }, { 0x0415, 'E' },
        { 0x0416, 'X' }, { 0x0418, 'N' }, { 0x0419, 'N' }, { 0x041A, 'K' }, { 0x041C, 'M' },
        { 0x041D, 'H' }, { 0x041E, 'O' }, { 0x0420, 'P' }, { 0x0421, 'C' }, { 0x0422, 'T' },
        { 0x0423, 'Y' }, { 0x0425, 'X' }, { 0x0428, 'W' }, { 0x0429, 'W' }, { 0x042A, 'B' },
        { 0x042C, 'B' }, { 0x042D, 'E' }, { 0x042F, 'R' }, { 0x0460, 'W' }, { 0x0474, 'V' },
        { 0x049A, 'K' }, { 0x049C, 'K' }, { 0x04A2, 'H' }, { 0x04AE, 'Y' }, { 0x04B0, 'Y' },
        { 0x04B2, 'X' }, { 0x04BA, 'H' }, { 0x04C0, 'I' }, { 0x04C3, 'K' }, { 0x0500, 'd' },
        { 0x051A, 'Q' }, { 0x051C, 'W' },
        // Cyrillic small letters
        { 0x0430, 'a' }, { 0x0431, 'b' }, { 0x0432, 'b' }, { 0x0433, 'r' }, { 0x0434, 'd' },
        { 0x0435, 'e' }, { 0x0438, 'n' }, { 0x0439, 'n' }, { 0x043A, 'k' }, { 0x043C, 'm' },
        { 0x043D, 'h' }, { 0x043E, 'o' }, { 0x043F, 'n' }, { 0x0440, 'p' }, { 0x0441, 'c' },
        { 0x0442, 't' }, { 0x0443, 'y' }, { 0x0445, 'x' }, { 0x0446, 'u' }, { 0x0448, 'w' },
        { 0x0449, 'w' }, { 0x044A, 'b' }, { 0x044C, 'b' }, { 0x044D, 'e' }, { 0x044F, 'r' },
        { 0x0450, 'e' }, { 0x0451, 'e' }, { 0x0454, 'e' }, { 0x0455, 's' }, { 0x0456, 'i' },
        { 0x0457, 'i' }, { 0x0458, 'j' }, { 0x045D, 'n' }, { 0x045E, 'y' }, { 0x0461, 'w' },
        { 0x0475, 'v' }, { 0x0491, 'r' }, { 0x049B, 'k' }, { 0x049D, 'k' }, { 0x04A3, 'h' },
        { 0x04AF, 'y' }, { 0x04B1, 'y' }, { 0x04B3, 'x' }, { 0x04BB, 'h' }, { 0x04C4, 'k' },
        { 0x04CF, 'l' }, { 0x0501, 'd' }, { 0x051B, 'q' }, { 0x051D, 'w' },
        // Latin Extended-B letters with hooks, strokes and other decorations
        { 0x0180, 'b' }, { 0x0181, 'B' }, { 0x0182, 'B' }, { 0x0183, 'b' }, { 0x0187, 'C' },
        { 0x0188, 'c' }, { 0x0189, 'D' }, { 0x018A, 'D' }, { 0x018E, 'E' }, { 0x0190, 'E' },
        { 0x0191, 'F' }, { 0x0192, 'f' }, { 0x0193, 'G' }, { 0x0197, 'I' }, { 0x0198, 'K' },
        { 0x0199, 'k' }, { 0x019A, 'l' }, { 0x019D, 'N' }, { 0x019E, 'n' }, { 0x019F, 'O' },
        { 0x01A0, 'O' }, { 0x01A1, 'o' }, { 0x01A4, 'P' }, { 0x01A5, 'p' }, { 0x01A6, 'R' },
        { 0x01AC, 'T' }, { 0x01AD, 't' }, { 0x01AE, 'T' }, { 0x01AF, 'U' }, { 0x01B0, 'u' },
        { 0x01B2, 'V' }, { 0x01B3, 'Y' }, { 0x01B4, 'y' }, { 0x01B5, 'Z' }, { 0x01B6, 'z' },
        { 0x01CD, 'A' }, { 0x01CE, 'a' }, { 0x01CF, 'I' }, { 0x01D0, 'i' }, { 0x01D1, 'O' },
        { 0x01D2, 'o' }, { 0x01D3, 'U' }, { 0x01D4, 'u' }, { 0x01D5, 'U' }, { 0x01D6, 'u' },
        { 0x01D7, 'U' }, { 0x01D8, 'u' }, { 0x01D9, 'U' }, { 0x01DA, 'u' }, { 0x01DB, 'U' },
        { 0x01DC, 'u' }, { 0x01DE, 'A' }, { 0x01DF, 'a' }, { 0x01E0, 'A' }, { 0x01E1, 'a' },
        { 0x01E4, 'G' }, { 0x01E5, 'g' }, { 0x01E6, 'G' }, { 0x01E7, 'g' }, { 0x01E8, 'K' },
        { 0x01E9, 'k' }, { 0x01EA, 'O' }, { 0x01EB, 'o' }, { 0x01EC, 'O' }, { 0x01ED, 'o' },
        { 0x01F0, 'j' }, { 0x01F4, 'G' }, { 0x01F5, 'g' }, { 0x01F8, 'N' }, { 0x01F9, 'n' },
        { 0x01FA, 'A' }, { 0x01FB, 'a' }, { 0x01FE, 'O' }, { 0x01FF, 'o' }, { 0x0200, 'A' },
        { 0x0201, 'a' }, { 0x0202, 'A' }, { 0x0203, 'a' }, { 0x0204, 'E' }, { 0x0205, 'e' },
        { 0x0206, 'E' }, { 0x0207, 'e' }, { 0x0208, 'I' }, { 0x0209, 'i' }, { 0x020A, 'I' },
        { 0x020B, 'i' }, { 0x020C, 'O' }, { 0x020D, 'o' }, { 0x020E, 'O' }, { 0x020F, 'o' },
        { 0x0210, 'R' }, { 0x0211, 'r' }, { 0x0212, 'R' }, { 0x0213, 'r' }, { 0x0214, 'U' },
        { 0x0215, 'u' }, { 0x0216, 'U' }, { 0x0217, 'u' }, { 0x0218, 'S' }, { 0x0219, 's' },
        { 0x021A, 'T' }, { 0x021B, 't' }, { 0x021E, 'H' }, { 0x021F, 'h' }, { 0x0220, 'N' },
        { 0x0224, 'Z' }, { 0x0225, 'z' }, { 0x0226, 'A' }, { 0x0227, 'a' }, { 0x0228, 'E' },
        { 0x0229, 'e' }, { 0x022A, 'O' }, { 0x022B, 'o' }, { 0x022C, 'O' }, { 0x022D, 'o' },
        { 0x022E, 'O' }, { 0x022F, 'o' }, { 0x0230, 'O' }, { 0x0231, 'o' }, { 0x0232, 'Y' },
        { 0x0233, 'y' }, { 0x023A, 'A' }, { 0x023B, 'C' }, { 0x023C, 'c' }, { 0x023D, 'L' },
        { 0x023E, 'T' }, { 0x023F, 's' }, { 0x0240, 'z' }, { 0x0243, 'B' }, { 0x0244, 'U' },
        { 0x0246, 'E' }, { 0x0247, 'e' }, { 0x0248, 'J' }, { 0x0249, 'j' }, { 0x024A, 'Q' },
        { 0x024B, 'q' }, { 0x024C, 'R' }, { 0x024D, 'r' }, { 0x024E, 'Y' }, { 0x024F, 'y' },
    };

    // Latin-1 Supplement and Latin Extended-A letters to their base letter ('.' = left alone),
    // only used in decorated words.
    const char LATIN_00C0[] =
        "AAAAAA.C" "EEEEIIII" "DNOOOOO." "OUUUUYPB"
        "aaaaaa.c" "eeeeiiii" "dnooooo." "ouuuuypy"
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "Ii..JjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "Oo..RrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

    // Latin Extended Additional (U+1E00..1EFF)
    const char LATIN_1E00[] =
        "AaBbBbBbCcDdDdDd" "DdDdEeEeEeEeEeFf" "GgHhHhHhHhHhIiIi" "KkKkKkLlLlLlLlMm"
        "MmMmNnNnNnNnOoOo" "OoOoPpPpRrRrRrRr" "SsSsSsSsSsTtTtTt" "TtUuUuUuUuUuVvVv"
        "WwWwWwWwWwXxXxYy" "ZzZzZzhtwyasssB." "AaAaAaAaAaAaAaAa" "AaAaAaAaEeEeEeEe"
        "EeEeEeEeIiIiOoOo" "OoOoOoOoOoOoOoOo" "OoOoUuUuUuUuUuUu" "UuYyYyYyYy....Yy";

    static_assert(sizeof(LATIN_00C0) == 0x180 - 0xC0 + 1, "Latin table size");
    static_assert(sizeof(LATIN_1E00) == 0x100 + 1, "Latin Extended Additional table size");

    template <size_t N>
    std::vector<FoldPair> make_sorted(const FoldPair (&pairs)[N])
    {
        std::vector<FoldPair> table(pairs, pairs + N);
        std::sort(table.begin(), table.end());
        return table;
    }

    const std::vector<FoldPair>& styled_table()
    {
        static const std::vector<FoldPair> table = make_sorted(STYLED_PAIRS);
        return table;
    }

    const std::vector<FoldPair>& lookalike_table()
    {
        static const std::vector<FoldPair> table = make_sorted(LOOKALIKE_PAIRS);
        return table;
    }

    bool lookup(const std::vector<FoldPair>& table, cp_t cp, cp_t& out)
    {
        FoldPair key = { cp, 0 };
        auto it = std::lower_bound(table.begin(), table.end(), key);
        if (it != table.end() && it->mFrom == cp)
        {
            out = it->mTo;
            return true;
        }
        return false;
    }

    void push_number(int value, std::vector<cp_t>& out)
    {
        if (value >= 10)
        {
            out.push_back('0' + (cp_t)(value / 10));
        }
        out.push_back('0' + (cp_t)(value % 10));
    }

    // Styled letters, digits and symbols to plain ones. false: not a styled character.
    bool fold_styled(cp_t c, std::vector<cp_t>& out)
    {
        if (c < 0x00B2)
        {
            return false;
        }
        // Mathematical Alphanumeric Symbols: 13 Latin styles of A-Z a-z ...
        if (c >= 0x1D400 && c <= 0x1D6A3)
        {
            out.push_back((cp_t)LATIN52[(c - 0x1D400) % 52]);
            return true;
        }
        // ... dotless i and j ...
        if (c == 0x1D6A4 || c == 0x1D6A5)
        {
            out.push_back(c == 0x1D6A4 ? 'i' : 'j');
            return true;
        }
        // ... 5 Greek styles ...
        if (c >= 0x1D6A8 && c <= 0x1D7C9)
        {
            out.push_back(math_greek((c - 0x1D6A8) % 58));
            return true;
        }
        // ... digamma ...
        if (c == 0x1D7CA || c == 0x1D7CB)
        {
            out.push_back(c == 0x1D7CA ? 0x03DC : 0x03DD);
            return true;
        }
        // ... and 5 styles of digits
        if (c >= 0x1D7CE && c <= 0x1D7FF)
        {
            out.push_back('0' + (c - 0x1D7CE) % 10);
            return true;
        }
        // Enclosed Alphanumerics
        if (c >= 0x2460 && c <= 0x24FF)
        {
            if (c <= 0x2473) { push_number((int)(c - 0x2460) + 1, out); }          // circled 1-20
            else if (c <= 0x2487) { push_number((int)(c - 0x2474) + 1, out); }     // parenthesized 1-20
            else if (c <= 0x249B) { push_number((int)(c - 0x2488) + 1, out); }     // 1.-20.
            else if (c <= 0x24B5) { out.push_back('a' + (c - 0x249C)); }           // parenthesized a-z
            else if (c <= 0x24CF) { out.push_back('A' + (c - 0x24B6)); }           // circled A-Z
            else if (c <= 0x24E9) { out.push_back('a' + (c - 0x24D0)); }           // circled a-z
            else if (c == 0x24EA || c == 0x24FF) { out.push_back('0'); }
            else if (c <= 0x24F4) { push_number((int)(c - 0x24EB) + 11, out); }    // negative circled 11-20
            else { push_number((int)(c - 0x24F5) + 1, out); }                      // double circled 1-10
            return true;
        }
        // Dingbat circled digits 1-10 (three styles)
        if (c >= 0x2776 && c <= 0x2793)
        {
            push_number((int)((c - 0x2776) % 10) + 1, out);
            return true;
        }
        // Enclosed Alphanumeric Supplement (regional indicators, i.e. flags, are left alone)
        if (c >= 0x1F100 && c <= 0x1F189)
        {
            if (c == 0x1F100 || c == 0x1F10B || c == 0x1F10C) { out.push_back('0'); }
            else if (c <= 0x1F10A) { out.push_back('0' + (c - 0x1F101)); }          // digit comma
            else if (c >= 0x1F110 && c <= 0x1F129) { out.push_back('A' + (c - 0x1F110)); }
            else if (c == 0x1F12A) { out.push_back('S'); }
            else if (c == 0x1F12B) { out.push_back('C'); }
            else if (c == 0x1F12C) { out.push_back('R'); }
            else if (c >= 0x1F130 && c <= 0x1F149) { out.push_back('A' + (c - 0x1F130)); }
            else if (c >= 0x1F150 && c <= 0x1F169) { out.push_back('A' + (c - 0x1F150)); }
            else if (c >= 0x1F170 && c <= 0x1F189) { out.push_back('A' + (c - 0x1F170)); }
            else { return false; }
            return true;
        }
        // Fullwidth ASCII
        if (c >= 0xFF01 && c <= 0xFF5E)
        {
            out.push_back(c - 0xFEE0);
            return true;
        }
        cp_t to = 0;
        if (lookup(styled_table(), c, to))
        {
            out.push_back(to);
            return true;
        }
        return false;
    }

    bool is_space(cp_t c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0 || c == 0x2007 || c == 0x202F;
    }

    bool is_ascii_letter(cp_t c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    bool is_accented_latin(cp_t c)
    {
        return (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) || (c >= 0x1E00 && c <= 0x1EFF);
    }

    bool is_greek(cp_t c)
    {
        return (c >= 0x0370 && c <= 0x03FF) || (c >= 0x1F00 && c <= 0x1FFF);
    }

    bool is_cyrillic(cp_t c)
    {
        return c >= 0x0400 && c <= 0x052F;
    }

    bool is_combining(cp_t c)
    {
        return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
            || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
    }

    // Decoration of one word: Greek/Cyrillic look-alikes, accents, hooks, combining marks.
    cp_t fold_decorated(cp_t c)
    {
        if (c >= 0x00C0 && c <= 0x017F)
        {
            const char base = LATIN_00C0[c - 0x00C0];
            return base == '.' ? c : (cp_t)base;
        }
        if (c >= 0x1E00 && c <= 0x1EFF)
        {
            const char base = LATIN_1E00[c - 0x1E00];
            return base == '.' ? c : (cp_t)base;
        }
        cp_t to = c;
        lookup(lookalike_table(), c, to);
        return to;
    }

    void fold_word(const std::vector<cp_t>& word, std::vector<cp_t>& out)
    {
        std::vector<cp_t> plain;
        plain.reserve(word.size());
        bool styled = false;
        for (cp_t c : word)
        {
            if (fold_styled(c, plain))
            {
                styled = true;
            }
            else
            {
                plain.push_back(c);
            }
        }

        int ascii = 0, accented = 0, greek = 0, cyrillic = 0, combining = 0;
        for (cp_t c : plain)
        {
            if (is_ascii_letter(c)) { ++ascii; }
            else if (is_accented_latin(c)) { ++accented; }
            else if (is_greek(c)) { ++greek; }
            else if (is_cyrillic(c)) { ++cyrillic; }
            else if (is_combining(c)) { ++combining; }
        }
        const int letters = ascii + accented + greek + cyrillic;
        const bool latin = ascii + accented > 0;
        const bool decorated = styled
            || (latin && (greek > 0 || cyrillic > 0))
            || (greek > 0 && cyrillic > 0)
            || (accented >= 3 && accented * 5 >= letters * 3)
            || (combining >= 2 && combining * 2 >= letters);

        if (!decorated)
        {
            out.insert(out.end(), plain.begin(), plain.end());
            return;
        }
        for (cp_t c : plain)
        {
            if (!is_combining(c))
            {
                out.push_back(fold_decorated(c));
            }
        }
    }
}

namespace PrestigeNameFold
{
    std::string fold(const std::string& utf8)
    {
        // plain ASCII: nothing to do
        bool ascii = true;
        for (char ch : utf8)
        {
            if ((unsigned char)ch >= 0x80)
            {
                ascii = false;
                break;
            }
        }
        if (ascii)
        {
            return utf8;
        }

        std::vector<cp_t> in;
        decode_utf8(utf8, in);

        std::vector<cp_t> folded;
        folded.reserve(in.size());
        std::vector<cp_t> word;
        for (size_t i = 0; i <= in.size(); ++i)
        {
            const bool end = i == in.size();
            // the ideographic space folds to a plain one: it separates words too
            if (end || is_space(in[i]) || in[i] == 0x3000)
            {
                if (!word.empty())
                {
                    fold_word(word, folded);
                    word.clear();
                }
                if (!end)
                {
                    folded.push_back(in[i] == 0x3000 ? (cp_t)' ' : in[i]);
                }
            }
            else
            {
                word.push_back(in[i]);
            }
        }

        std::string out;
        out.reserve(utf8.size());
        for (cp_t c : folded)
        {
            encode_utf8(c, out);
        }
        return out;
    }

    bool isFancy(const std::string& utf8)
    {
        return fold(utf8) != utf8;
    }
}
