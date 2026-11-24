/**
 * @file prestigenamefold.h
 * @brief Readable chat: folds the fancy letters of decorated display names
 *        (math alphanumerics, enclosed and fullwidth letters, small caps,
 *        decorative Greek/Cyrillic look-alikes) to plain letters.
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

#ifndef PRESTIGENAMEFOLD_H
#define PRESTIGENAMEFOLD_H

#include <string>

namespace PrestigeNameFold
{
    // UTF-8 in, UTF-8 out. Styled letters (U+1D400..1D7FF, letterlike symbols, enclosed
    // alphanumerics, fullwidth forms, small caps, super/subscript letters) always become plain
    // letters. Words that are clearly decorated (they had such letters, mix Latin with Greek or
    // Cyrillic, mix Greek with Cyrillic, or are mostly accented letters) also lose their
    // Greek/Cyrillic look-alikes, accents and hooks ("ĴЄΎƘΊΜ" -> "JEYKIM"). Plain words in any
    // script ("José", "Straße", "Анна") are left as they are.
    std::string fold(const std::string& utf8);

    // true when fold() would change the text
    bool isFancy(const std::string& utf8);
}

#endif // PRESTIGENAMEFOLD_H
