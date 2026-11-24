/**
 * @file slfloaterweather.h
 * @brief Weather floater (#783). Prestige: the controls are SLPanelWeather (slpanelweather.h,
 *        panel_sl_weather.xml), hosted here and in the Weather tab in Preferences. Your own local weather -- on / off, rain or snow,
 *        intensity, wind (gusts), thunderstorm, transition speed -- and the region / parcel
 *        weather opt-ins with their status. Every control is a setting (Weather*,
 *        floater_sl_weather.xml); the floater only adds the live status line.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_FLOATERWEATHER_H
#define SL_FLOATERWEATHER_H

#include "llfloater.h"

class SLFloaterWeather : public LLFloater
{
    friend class LLFloaterReg;
public:
    void onOpen(const LLSD& key) override;

private:
    SLFloaterWeather(const LLSD& key);
    ~SLFloaterWeather() override = default;
};

#endif // SL_FLOATERWEATHER_H
