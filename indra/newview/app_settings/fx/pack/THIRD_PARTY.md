# Bundled starter effects -- third-party notices (#957)

Slipstream ships a small, curated set of ReShade FX effects so a fresh install has
something useful in Photo Studio > Effects. The files are copied **unmodified** from
the upstream releases below. They are read-only here: put a file of the same name in
your own effects folder and yours is used instead (first folder wins by file name).

| Pack | Source | Pinned commit | Licence |
|---|---|---|---|
| SweetFX | https://github.com/CeeJayDK/SweetFX | `93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361` (master, 2026-09-26; the repo has no tags) | MIT, Copyright (c) 2014 CeeJayDK -- `Shaders/SweetFX/LICENSE.txt` |
| prod80 | https://github.com/prod80/prod80-ReShade-Repository | `1c2ed5b093b03c558bfa6aea45c2087052e99554` (master, 2023-01-05; the repo has no tags) | MIT, Copyright (c) 2020 prod80 -- `Shaders/prod80/LICENSE.txt` |

The same notices are in the About box (licenses.txt).

## Audit rule

The repo LICENSE applies to a file unless its header names a different licence, or a
different author without stating terms, or credits code under another licence. Nothing
under `Textures/` is shipped (no texture terms to clear), so anything that needs an
external texture, or a shared header that does, is out. `ReShade.fxh` (CC0) and
`ReShadeUI.fxh` (Slipstream's own) are in `../Shaders`, not in this pack.

## Shipped (10 files)

| File | Evidence |
|---|---|
| SweetFX `Curves.fx`, `Levels.fx`, `Vibrance.fx`, `Tonemap.fx`, `LumaSharpen.fx`, `Vignette.fx`, `FilmGrain.fx` | header: "by Christian Cann Schuldt Jensen ~ CeeJay.dk"; no other author or licence named; repo MIT |
| SweetFX `Monochrome.fx` | header: "Author: CeeJay.dk, License: MIT" |
| prod80 `PD80_04_Color_Balance.fx`, `PD80_05_Sharpening.fx` | header: "Author: prod80 (Bas Veth), License: MIT, Copyright (c) 2020 prod80" with the MIT text; no `PD80_*.fxh` include, no textures, no other credits |

## Excluded

SweetFX:
- `CRT.fx`: GPL-2.0-or-later (cgwg, Themaister, DOLLS)
- `FXAA.fx`, `FXAA.fxh`: NVIDIA all-rights-reserved notice
- `SMAA.fx`, `SMAA.fxh`, `AreaTex.png`, `SearchTex.png`: other authors (Jimenez et al.), textures; redundant with the built-in AA
- `CAS.fx`: Copyright AMD (MIT, but a separate notice; not needed for the starter set)
- `Layer.fx`, `Layer.png`: texture
- `DPX.fx` (Loadus), `Technicolor.fx` (DKT70), `Technicolor2.fx` (Prod80 original), `LiftGammaGain.fx` (3an), `Border.fx` (Oomek): other authors, no terms in the file
- `Template.fx`: blank licence field
- `ASCII`, `Cartoon`, `ChromaticAberration`, `ColorMatrix`, `Compare`, `FakeHDR`, `Nostalgia`, `Sepia`, `Splitscreen`: left out to keep the set modest (novelty or redundant); not audited for this pack

prod80:
- `PD80_04_Color_Temperature.fx`: credits CC-BY-3.0 code (shadertoy, Renaud Bedard)
- `PD80_04_Technicolor.fx`: credits a StackExchange snippet (CC-BY-SA)
- `PD80_04_BlacknWhite.fx`: credits shadertoy 'iq' without terms
- every effect that includes `PD80_00_Noise_Samplers.fxh` (needs `pd80_bluenoise*.png`, `pd80_gaussnoise.png`), `PD80_06_Film_Grain.fx` (`pd80_permtexture.png`), and the LUT effects / textures: textures without stated terms
- every effect that includes `PD80_00_Color_Spaces.fxh` (collected from lolengine / Bruce Lindbloom with mixed provenance), `PD80_00_Blend_Modes.fxh` or `PD80_00_Base_Effects.fxh`
- the rest: left out to keep the set modest

Not bundled by policy (user-installed only): qUINT, iMMERSE, Lilium HDR, crosire `reshade-shaders` samples.

## Prestige effect library (added by Prestige)

Same audit rule as above: permissive licences only, unmodified files, nothing whose header names other terms or authors without terms. Not bundled: qUINT and AstrayFX (all rights reserved / no terms), fubax effects under NC / ND or OCL terms (or that include BlueNoiseDither.fxh), AlucardDH (GPL-2), LUT effects, PandaFX (large texture), Daltonize / DisplayDepth (no terms).

| Pack | Source | Commit | Licence | Effects |
|---|---|---|---|---|
| SweetFX (more effects) | https://github.com/CeeJayDK/SweetFX | `93ddf39b357f` | MIT, Copyright (c) 2014 CeeJayDK (text above). CAS.fx: MIT, Copyright (c) 2017-2019 Advanced Micro Devices, Inc. (notice in the file) | ASCII, Cartoon, ChromaticAberration, ColorMatrix, FakeHDR, Nostalgia, Sepia, Splitscreen, CAS |
| OtisFX | https://github.com/FransBouma/OtisFX | `193aa0bf07ee` | MIT / BSD-2-Clause, Copyright (c) 2018-2023 Frans Bouma (notices in the files and LICENSE.txt); textures from the same repository | AdaptiveFog, CinematicDOF, DepthDarkness, DepthHaze, DirectionalDepthBlur, Emphasize, GoldenRatio, Heightfog, MagicBorder |
| potatoFX | https://github.com/GimleLarpes/potatoFX | `f55a02212168` | MIT, Copyright (c) 2023 Gimle Larpes; textures from the same repository | pCamera, pColorNoise, pColors, pPalettePosterize |
| FXShaders | https://github.com/luluco250/FXShaders | `76365e35c48e` | MIT, Copyright (c) 2017 Lucas Melo (CRT_Lottes.fxh: public domain, Timothy Lottes); lens dirt texture from the same repository | AdaptiveTonemapper, ArtisticVignette, AspectRatioSuite, CRT_Lottes, ColorLab, Dither, FlexibleCA, FocalDOF, GrainSpread, LiquidLens, MagicHDR, MinimalColorGrading, MultiFX, NeoBloom, PiecewiseFilmicTonemap, RetroFog, SCurve, UnrealLens, Unsharp |
| CShade | https://github.com/papadanku/CShade | `2b220380477c` | BSD-3-Clause, Copyright (c) 2026 papadanku; files crediting others keep their notices (AMD, Microsoft, Keijiro Takahashi: MIT; kDatamosh: public domain) | cAutoExposure, cBloom, cCAS, cDots, cEnsor, cLens, cLetterBox, cMotionBlur, cNoiseBlur, cQuantize, cThreshold, kContour, kDatamosh, kMirror |
| Glamayre Fast Effects | https://github.com/rj200/Glamarye_Fast_Effects_for_ReShade | `9dd9b826fa2c` | MIT, Copyright (c) 2021 Robert Jessop, Alex Tuderan | Glamayre_Fast_Effects, fastBlur, rj_sharpen |
| Daodan317081 reshade-shaders | https://github.com/Daodan317081/reshade-shaders | `f01ddb6f3dce` | BSD-3-Clause, Copyright (c) 2018-2019 Alexander Federwisch | AspectRatioComposition, ColorIsolation, ColorfulPoster, Comic, MultiTonePoster, RemoveTint, RetroTint |
| reshade-shaders Deband | https://github.com/crosire/reshade-shaders | `fd0022170615` | MIT, Copyright (c) 2015 Niklas Haas (notice in the file) | Deband |
| fubax-shaders | https://github.com/Fubaxiusz/fubax-shaders | `d902cc2e8216` | by Jakub Maksymilian Fober: CC BY-SA 4.0 (BeforeAfter, Chromakey, FilmicAnamorphSharpen, SimpleGrain, SunsetFilter; https://creativecommons.org/licenses/by-sa/4.0/), CC0 (Letterbox, LinearWorkflow.fxh, ColorConversion.fxh); unmodified | BeforeAfter, Chromakey, FilmicAnamorphSharpen, Letterbox, SimpleGrain, SunsetFilter |
| brussell1 Shaders | https://github.com/brussell1/Shaders | `3869218ea004` | CC BY 4.0, by brussell (https://creativecommons.org/licenses/by/4.0/); unmodified | EyeAdaption |

### More packs (Prestige 1.8.6)

Same audit rule; files unmodified (line endings normalised by git). Every bundled effect is
compiled and SPIR-V-validated by `.github/workflows/prestige-fx-check.yml` on each push.

| Pack | Source | Commit | Licence | Effects |
|---|---|---|---|---|
| AcerolaFX | https://github.com/GarrettGunnell/AcerolaFX | `c33f779b093f` | MIT, Copyright (c) 2023 Garrett Gunnell (`Shaders/AcerolaFX/LICENSE.md`); textures from the same repository | every effect except XeGTAO (Intel-derived; left out) |
| Insane-Shaders | https://github.com/LordOfLunacy/Insane-Shaders | `19397d503e2f` | CC0-1.0 (`Shaders/InsaneShaders/LICENSE`); Bessel_Bloom.fx keeps Keijiro Takahashi's MIT notice | Bessel_Bloom, BilateralComic, BilateralCS, ContrastStretch, DeepFry, Dehaze, GaussianBlurCS, Halftone, LocalContrastCS, Oilify, ReVeil, SharpContrast (not CMAA_2: Apache-2.0 + textures; not VRS_Map, Mouse, UIMaskCreator: utilities) |
| CobraFX | https://github.com/LordKobra/CobraFX | `bf9295a7bb8a` | MIT, Copyright (c) SirCobra (`Shaders/CobraFX/LICENSE`); HSV conversion in CobraUtility.fxh by Sam Hocevar (lolengine, WTFPL) | CobraMask, Droste, Frequency_CS, Greyscale, RealLongExposure (not ColorSort_CS, Gravity, Gravity_CS: adapted code without stated terms) |
| CorgiFX | https://github.com/originalnicodr/CorgiFX | `c4cfbf9dd37f` | CC0-1.0 (`Shaders/CorgiFX/LICENSE`) | AspectRatioMultiGrid (+ customGrid.png), CanvasMask, Flip, FreezeShot (not MagnifyingGlass: shadertoy; not StageDepthPlus, CanvasFog, ColorMask: other authors' code) |
| NiceGuy-Shaders | https://github.com/mj-ehsan/NiceGuy-Shaders | `2fa6bfc15a8b` | CC0-1.0 (`Shaders/NiceGuy/LICENSE`; "License: CC0" in the files) | DynamicRaindrops (+ NGbokeh.jpg), FastSharp, SlowSharp, HoleFiller, Rim, VolumetricFog, Volumetric Fog V2.0 (not NGLighting, Scatter, Lamps: shadertoy / third-party code) |

Not bundled: dh-reshade-shaders (GPL-2.0), AstrayFX and RSRetroArch (no licence).

<!-- <Prestige> first-party photo filters -->
## Prestige photo filters (first-party, not third-party)

`Shaders/Prestige/` (`Prestige*.fx`, `PrestigeCommon.fxh`) is original Prestige Viewer
code written for the viewer (LGPL-2.1-only, as the viewer); nothing in it is copied
from a shader pack, and it uses no textures. Listed here only so the folder is not
mistaken for an unaudited pack.
<!-- </Prestige> -->
