/**
 * @file slfloaterweather.cpp
 * @brief Weather floater (#783): see slfloaterweather.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slfloaterweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloaterweather.h"


SLFloaterWeather::SLFloaterWeather(const LLSD& key)
:   LLFloater(key)
{
}

// #1224: never start taller than the area the floaters may use (the saved rect may come from a
// bigger window or UI scale); the scroll container makes the shortened floater fully reachable.
void SLFloaterWeather::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!gFloaterView) return;
    const LLRect avail = gFloaterView->getSnapRect();
    LLRect rect = getRect();
    if (rect.getHeight() > avail.getHeight())
    {
        reshape(rect.getWidth(), llmax(getMinHeight(), avail.getHeight()));
        rect = getRect();
    }
    if (rect.mTop > avail.mTop)
    {
        translate(0, avail.mTop - rect.mTop);
    }
    else if (rect.mBottom < avail.mBottom)
    {
        translate(0, avail.mBottom - rect.mBottom);
    }
}

