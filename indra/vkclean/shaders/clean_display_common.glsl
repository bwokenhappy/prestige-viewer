// Shared display transfer helpers for the live HDR output and SDR snapshot path.
// The input is the already tone-mapped display image in absolute Rec.709 nits.
const float kDisplayReferenceWhiteMaxNits = 1000.0; // vk_display_limits.h
vec3 displayNitsToSdrLinear(vec3 nits, float paperWhiteNits, float peakNits)
{
    float white = (isnan(paperWhiteNits) || isinf(paperWhiteNits)) ? 203.0 :
        clamp(paperWhiteNits, 80.0, kDisplayReferenceWhiteMaxNits);
    // Match vkdisplay::sdrSnapshotLinear: this input already passed the scene
    // tone curve. Applying a second shoulder dims SDR UI white to roughly 0.5.
    // SDR copies retain the reference-white range; HDR-only values clip here.
    // peakNits is kept in the shared call contract for display-output callers.
    for (int channel = 0; channel < 3; ++channel)
        if (isnan(nits[channel]) || isinf(nits[channel])) nits[channel] = 0.0;
    return clamp(nits, vec3(0.0), vec3(white)) / white;
}

vec3 displayLinearToSrgb(vec3 linearValue)
{
    linearValue = clamp(linearValue, 0.0, 1.0);
    return mix(1.055 * pow(linearValue, vec3(1.0 / 2.4)) - 0.055,
               12.92 * linearValue, lessThanEqual(linearValue, vec3(0.0031308)));
}

// The sRGB curve continued past 1 (the same formula, unclamped) and mirrored below 0: the
// space the HDR UI layer is composited in. Up to white it is exactly the SDR swapchain's
// encoding; above it it keeps rising, so a translucent UI edge over a highlight stays smooth.
vec3 encodeExt(vec3 x)
{
    vec3 a = abs(x);
    return sign(x) * mix(1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055, 12.92 * a,
                         lessThanEqual(a, vec3(0.0031308)));
}

vec3 decodeExt(vec3 e)
{
    vec3 a = abs(e);
    return sign(e) * mix(pow((a + 0.055) / 1.055, vec3(2.4)), a / 12.92,
                         lessThanEqual(a, vec3(0.04045)));
}

// The HDR UI layer over the world (#1451). ui: what the SDR UI pipelines drew into a transparent
// RGBA8 layer -- premultiplied sRGB-encoded colour, coverage in alpha. The blend is the SDR
// swapchain's, done in encodeExt space at paper white: over world at or below white the result
// is exactly what SDR shows (clamped at white as the swapchain is); above white an additive UI
// colour adds nothing past the world's own value. Returns absolute nits.
vec3 displayComposeUi(vec3 nits, vec4 ui, float paperWhiteNits)
{
    float white = (isnan(paperWhiteNits) || isinf(paperWhiteNits)) ? 203.0 :
        clamp(paperWhiteNits, 80.0, kDisplayReferenceWhiteMaxNits);
    vec3 world = encodeExt(max(nits, vec3(0.0)) / white);
    vec3 blended = min(vec3(ui) + world * (1.0 - clamp(ui.a, 0.0, 1.0)), max(world, vec3(1.0)));
    return decodeExt(max(blended, vec3(0.0))) * white;
}

// #1536 SDR frame generation: the same blend in SDR, where the world is already in the space the
// swapchain blends in (sRGB-encoded, 8-bit): no transfer functions, the swapchain's own blend clamped
// at white as UNORM is. A texel with neither colour nor coverage leaves the world as it is. Mirrored by
// vkdisplay::composeLayerSdr (vk_display_hdr.h).
vec3 displayComposeUiSdr(vec3 encodedWorld, vec4 ui)
{
    return min(ui.rgb + encodedWorld * (1.0 - clamp(ui.a, 0.0, 1.0)), vec3(1.0));
}

// A snapshot frame under HDR (#1451 G5) renders its world through the SDR post chain: the
// display image then holds SDR-encoded values. This is that world at paper white (the encode
// shows the frame as SDR does; the shot's composite writes it back with encodeExt).
vec3 displaySdrWorldNits(vec3 encoded, float paperWhiteNits)
{
    float white = (isnan(paperWhiteNits) || isinf(paperWhiteNits)) ? 203.0 :
        clamp(paperWhiteNits, 80.0, kDisplayReferenceWhiteMaxNits);
    return decodeExt(clamp(encoded, vec3(0.0), vec3(1.0))) * white;
}
