/**
 * @file slcameraexposure_test.cpp
 * @brief Camera mode (#1575 / #1576): the 1/3-stop scales, the P / Av / Tv / M solve, focal
 *        length <-> field of view and the white balance map.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slcameraexposure.h"

#include "../test/lltut.h"

#include <cmath>

namespace SLCE = SLCameraExposure;

namespace tut
{
    struct cameraexposure_data {};
    typedef test_group<cameraexposure_data> cameraexposure_test;
    typedef cameraexposure_test::object cameraexposure_object;
    tut::cameraexposure_test cameraexposure("SLCameraExposure");

    // The scales step in thirds and stop at their ends.
    template<> template<>
    void cameraexposure_object::test<1>()
    {
        ensure_equals("f/8 + 1/3 stop", SLCE::stepStop(SLCE::apertureStops(), 8.f, 1), 9.f);
        ensure_equals("f/8 + 1 stop", SLCE::stepStop(SLCE::apertureStops(), 8.f, 3), 11.f);
        ensure_equals("f/22 is the end", SLCE::stepStop(SLCE::apertureStops(), 22.f, 2), 22.f);
        ensure_equals("f/1.4 is the start", SLCE::stepStop(SLCE::apertureStops(), 1.4f, -1), 1.4f);
        ensure_equals("an off-scale f/8.4 snaps to f/8", SLCE::snapStop(SLCE::apertureStops(), 8.4f), 8.f);
        ensure_equals("1/125 faster by a stop", SLCE::stepStop(SLCE::shutterStops(), 1.f / 125.f, 3), 1.f / 250.f);
        ensure_equals("ISO 100 + 1 stop", SLCE::stepStop(SLCE::isoStops(), 100.f, 3), 200.f);
        ensure_equals("EV +1/3", SLCE::stepThirds(0.f, 1, -1.f, 2.f), 1.f / 3.f);
        ensure_equals("EV clamps", SLCE::stepThirds(2.f, 3, -1.f, 2.f), 2.f);
    }

    // Av: the shutter follows (EV 13 at ISO 100: f/8 -> 1/125); more ISO, a faster shutter;
    // +1 EV, a slower one. Auto exposure stays on with the compensation as the exposure.
    template<> template<>
    void cameraexposure_object::test<2>()
    {
        SLCE::Settings s;
        s.mode = SLCE::MODE_APERTURE;
        s.fnumber = 8.f;
        SLCE::Result r = SLCE::solve(s);
        ensure_equals("Av f/8 keeps the aperture", r.fnumber, 8.f);
        ensure_equals("Av f/8 -> 1/125", r.shutter, 1.f / 125.f);
        ensure("Av: auto exposure", r.autoExposure);

        s.iso = 400.f;
        ensure_equals("Av ISO 400 -> 1/500", SLCE::solve(s).shutter, 1.f / 500.f);

        s.iso = 100.f;
        s.evComp = 1.f;
        r = SLCE::solve(s);
        ensure_equals("Av +1 EV -> 1/60", r.shutter, 1.f / 60.f);
        ensure_equals("Av +1 EV exposure", r.exposureStops, 1.f);
    }

    // Tv: the aperture follows, clamped to the lens.
    template<> template<>
    void cameraexposure_object::test<3>()
    {
        SLCE::Settings s;
        s.mode = SLCE::MODE_SHUTTER;
        s.shutter = 1.f / 125.f;
        ensure_equals("Tv 1/125 -> f/8", SLCE::solve(s).fnumber, 8.f);
        s.shutter = 1.f;
        ensure_equals("Tv 1 s -> f/22 (the lens stops there)", SLCE::solve(s).fnumber, 22.f);
    }

    // M: no auto exposure; the meter is the difference to the scene.
    template<> template<>
    void cameraexposure_object::test<4>()
    {
        SLCE::Settings s;
        s.mode = SLCE::MODE_MANUAL;
        s.fnumber = 8.f;
        s.shutter = 1.f / 125.f;
        SLCE::Result r = SLCE::solve(s);
        ensure("M: auto exposure off", !r.autoExposure);
        ensure("M f/8 1/125 ISO 100 is on the scene", std::fabs(r.exposureStops) < 0.05f);
        s.shutter = 1.f / 60.f;
        ensure("M one stop slower: +1 over", std::fabs(SLCE::solve(s).exposureStops - 1.f) < 0.1f);
        s.shutter = 1.f / 125.f;
        s.iso = 200.f;
        ensure("M ISO 200: +1 over", std::fabs(SLCE::solve(s).exposureStops - 1.f) < 0.05f);
    }

    // P: f/5.6 at 1/250 on the scene; the program shift trades a stop of each.
    template<> template<>
    void cameraexposure_object::test<5>()
    {
        SLCE::Settings s;
        s.mode = SLCE::MODE_PROGRAM;
        SLCE::Result r = SLCE::solve(s);
        ensure_equals("P -> f/5.6", r.fnumber, 5.6f);
        ensure_equals("P -> 1/250", r.shutter, 1.f / 250.f);
        s.programShift = 1.f;
        r = SLCE::solve(s);
        ensure_equals("P shifted -> f/4", r.fnumber, 4.f);
        ensure_equals("P shifted -> 1/500", r.shutter, 1.f / 500.f);
    }

    // Labels.
    template<> template<>
    void cameraexposure_object::test<6>()
    {
        ensure_equals("f/5.6", SLCE::apertureLabel(5.6f), std::string("f/5.6"));
        ensure_equals("f/8", SLCE::apertureLabel(8.f), std::string("f/8"));
        ensure_equals("1/125", SLCE::shutterLabel(1.f / 125.f), std::string("1/125"));
        ensure_equals("0.5\"", SLCE::shutterLabel(0.5f), std::string("0.5\""));
        ensure_equals("1\"", SLCE::shutterLabel(1.f), std::string("1\""));
        ensure_equals("+0.7", SLCE::evLabel(2.f / 3.f), std::string("+0.7"));
        ensure_equals("0.0", SLCE::evLabel(0.f), std::string("0.0"));
    }

    // Focal length: a 3:2 frame filling a 3:2 view 24 mm high at 2 atan(12/50) is 50 mm, and
    // back again.
    template<> template<>
    void cameraexposure_object::test<7>()
    {
        const F32 vfov = 2.f * std::atan(12.f / 50.f);
        const F32 f = SLCE::focalLengthForFov(vfov, 1000.f, 1500.f, 1000.f);
        ensure("50 mm", std::fabs(f - 50.f) < 0.1f);
        const F32 back = SLCE::fovForFocalLength(50.f, 1000.f, 1500.f, 1000.f);
        ensure("and back", std::fabs(back - vfov) < 1e-4f);
        ensure_equals("zoom ring: 50 -> 56", SLCE::stepFocalLength(50.f, 1), 56.f);
        ensure_equals("zoom ring moves at short lengths", SLCE::stepFocalLength(8.f, 1), 9.f);
    }

    // White balance: Daylight is the image as rendered; tungsten cools, shade warms.
    template<> template<>
    void cameraexposure_object::test<8>()
    {
        ensure_equals("daylight", SLCE::whiteBalanceOffsetForKelvin(5500.f), 0.f);
        ensure("tungsten cools (positive offset)", SLCE::whiteBalanceOffsetForKelvin(3200.f) > 0.f);
        ensure("shade warms (negative offset)", SLCE::whiteBalanceOffsetForKelvin(7500.f) < 0.f);
        ensure_equals("round trip", SLCE::kelvinForWhiteBalanceOffset(SLCE::whiteBalanceOffsetForKelvin(4200.f)), 4200.f);
        ensure_equals("next preset after daylight", SLCE::nextWhiteBalancePreset(5500.f, 1), 6500.f);
        ensure_equals("next preset wraps", SLCE::nextWhiteBalancePreset(7500.f, 1), 3200.f);
        ensure_equals("kelvin clamps", SLCE::stepKelvin(10000.f, 3), 10000.f);
    }

    // Output size and crop.
    template<> template<>
    void cameraexposure_object::test<9>()
    {
        S32 w = 0, h = 0;
        SLCE::outputSize(SLCE::ASPECT_3_2, 1920, 1080, 10240, w, h);
        ensure_equals("3:2 keeps the long side", w, 1920);
        ensure_equals("3:2 height", h, 1280);
        SLCE::outputSize(SLCE::ASPECT_4_5, 1920, 1080, 10240, w, h);
        ensure_equals("4:5 is portrait: long side is the height", h, 1920);
        ensure_equals("4:5 width", w, 1536);
        SLCE::outputSize(SLCE::ASPECT_OUTPUT, 800, 600, 10240, w, h);
        ensure("output: as is", w == 800 && h == 600);
        F32 cw = 0.f, ch = 0.f;
        SLCE::cropRect(1920, 1080, 1080, 1080, cw, ch);
        ensure("square crop of a 16:9 view", std::fabs(cw - 1080.f) < 0.5f && std::fabs(ch - 1080.f) < 0.5f);
    }
}
