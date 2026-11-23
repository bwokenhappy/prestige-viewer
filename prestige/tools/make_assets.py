"""Replace every Firestorm logo in the repo with the Prestige 'P' logo.
Usage: python3 -I make_assets.py <repo_root> <logo.png>
File names are kept (the build scripts reference them); only contents change.
"""
import sys, os, glob, subprocess
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT, LOGO = sys.argv[1], sys.argv[2]
NV = os.path.join(ROOT, 'indra/newview')
BASE = Image.open(LOGO).convert('RGB')
BG = (30, 30, 34)
ORANGE = (255, 140, 26)
FONT_P = '/usr/share/fonts/truetype/google-fonts/ofl/montserrat/Montserrat-Italic[wght].ttf'
FONT_T = '/usr/share/fonts/truetype/google-fonts/ofl/montserrat/Montserrat[wght].ttf'
changed = []


def font(path, size, weight=800):
    f = ImageFont.truetype(path, size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass
    return f


def small_tile(size, rounded=True):
    """At 32px and below the black-on-charcoal P disappears, so draw an orange P on the dark tile."""
    s = 16
    big = size * s
    im = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if rounded:
        d.rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * 0.2), fill=BG + (255,))
    else:
        d.rectangle((0, 0, big, big), fill=BG + (255,))
    f = font(FONT_P, int(big * 0.9), 900)
    d.text((big * 0.47, big * 0.52), 'P', font=f, fill=ORANGE + (255,), anchor='mm')
    return im.resize((size, size), Image.LANCZOS)


def tile(size, rounded=True, os_badge=False):
    """The P logo as a square app tile (rounded corners transparent)."""
    if size <= 32:
        return small_tile(size, rounded)
    big = 1024
    im = BASE.resize((big, big), Image.LANCZOS).convert('RGBA')
    if rounded:
        mask = Image.new('L', (big, big), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * 0.2), fill=255)
        im.putalpha(mask)
    if os_badge:
        d = ImageDraw.Draw(im)
        w, h = int(big * 0.42), int(big * 0.24)
        x0, y0 = big - w - int(big * 0.04), big - h - int(big * 0.04)
        d.rounded_rectangle((x0, y0, x0 + w, y0 + h), radius=int(h * 0.3), fill=ORANGE + (255,),
                            outline=(20, 20, 22, 255), width=int(big * 0.012))
        f = font(FONT_T, int(h * 0.72), 900)
        d.text((x0 + w / 2, y0 + h / 2), 'OS', font=f, fill=(20, 20, 22, 255), anchor='mm')
    return im.resize((size, size), Image.LANCZOS)


def flat(im, bg=BG):
    out = Image.new('RGB', im.size, bg)
    out.paste(im, (0, 0), im)
    return out


def glyph(size, color):
    """Monochrome italic 'P' for small toolbar icons (replaces the phoenix glyph)."""
    w, h = size
    s = 8
    im = Image.new('L', (w * s, h * s), 0)
    d = ImageDraw.Draw(im)
    f = font(FONT_P, int(h * s * 1.05), 900)
    d.text((w * s / 2, h * s / 2), 'P', font=f, fill=255, anchor='mm')
    a = im.resize((w, h), Image.LANCZOS)
    out = Image.new('RGBA', (w, h), tuple(color) + (0,))
    out.putalpha(a)
    return out


def save(img, path, fmt=None, **kw):
    img.save(path, fmt, **kw)
    changed.append(os.path.relpath(path, ROOT))


def save_ico(path, sizes, osb):
    imgs = [tile(n, os_badge=osb and n >= 48) for n in sizes]
    save(imgs[-1], path, 'ICO', sizes=[(n, n) for n in sizes], append_images=imgs[:-1])


def save_icns(path, osb):
    imgs = [tile(n, os_badge=osb and n >= 48) for n in (16, 32, 64, 128, 256, 512)]
    save(tile(1024, os_badge=osb), path, 'ICNS', append_images=imgs)


def is_os(path):
    p = path.replace('\\', '/')
    return '-os/' in p or 'firestormos' in p or '_os.ico' in p


# 1. Channel icon sets: icons/<channel>/firestorm_*
for d in sorted(glob.glob(os.path.join(NV, 'icons', '*'))):
    osb = d.endswith('-os')
    for n in (16, 32, 48, 128, 256, 512):
        p = os.path.join(d, 'firestorm_%d.png' % n)
        if os.path.exists(p):
            save(tile(n, os_badge=osb), p)
    p = os.path.join(d, 'firestorm_256.bmp')
    if os.path.exists(p):
        save(flat(tile(256, rounded=False, os_badge=osb)), p, 'BMP')
    p = os.path.join(d, 'firestorm_icon.ico')
    if os.path.exists(p):
        save_ico(p, [16, 32, 48, 128, 256], osb)
    p = os.path.join(d, 'firestorm_icon.icns')
    if os.path.exists(p):
        save_icns(p, osb)

# 2. Mac bundle + DMG volume icons
for p in [os.path.join(NV, 'firestorm_icon.icns'), os.path.join(NV, 'firestormos_icon.icns')] + \
        glob.glob(os.path.join(NV, 'installers/darwin/*-dmg/_VolumeIcon.icns')):
    save_icns(p, is_os(p))

# 3. Windows installer + crash logger
win = os.path.join(NV, 'installers/windows')
ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
save_ico(os.path.join(win, 'firestorm_icon.ico'), [16, 32, 48, 64, 128, 256], False)
save_ico(os.path.join(win, 'firestorm_icon_os.ico'), [16, 32, 48, 64, 128, 256], True)
save(flat(tile(256, rounded=False)), os.path.join(win, 'firestorm_icon.BMP'), 'BMP')
save(flat(tile(64, rounded=False)), os.path.join(win, 'firestorm_icon_small.BMP'), 'BMP')
save_ico(os.path.join(ROOT, 'indra/win_crash_logger/firestorm_icon.ico'), [16, 32, 48, 256], False)

# 4. Linux / runtime window icons
save(tile(512), os.path.join(NV, 'res/firestorm_icon.png'))
save(tile(512), os.path.join(NV, 'res-sdl/firestorm_icon.png'))
save(tile(128), os.path.join(NV, 'res-sdl/firestorm_icon128.png'))
save(tile(256), os.path.join(ROOT, 'doc/firestorm_256.png'))

# 5. In-viewer skin textures
save(tile(128), os.path.join(NV, 'skins/default/textures/firestorm_icon.png'))
save(tile(444), os.path.join(NV, 'skins/default/textures/windows/login_fs_logo.png'))

for p in glob.glob(os.path.join(NV, 'skins/**/inv_folder_firestorm.tga'), recursive=True) + \
        glob.glob(os.path.join(NV, 'skins/**/inv_folder_phoenix.tga'), recursive=True):
    sz = Image.open(p).size
    save(tile(sz[0]).resize(sz, Image.LANCZOS), p, 'TGA')

# SL_Logo.png is the chat system-message icon; in Firestorm skins it's the red
# Firestorm ball, in others the Second Life hand (left alone).
for p in glob.glob(os.path.join(NV, 'skins/**/SL_Logo.png'), recursive=True):
    im = Image.open(p).convert('RGBA')
    px = [c for c in im.get_flattened_data() if c[3] > 128]
    r = sum(c[0] for c in px) / len(px); g = sum(c[1] for c in px) / len(px); b = sum(c[2] for c in px) / len(px)
    if r > g * 1.6 and r > b * 1.6:  # red/orange Firestorm ball
        save(tile(im.size[0]).resize(im.size, Image.LANCZOS), p)

# Phoenix glyph used on the quick-prefs toolbar button -> P glyph, same color/mode
for p in glob.glob(os.path.join(NV, 'skins/**/toolbar_icons/phoenix.png'), recursive=True) + \
        glob.glob(os.path.join(NV, 'skins/**/bottomtray/phoenix_18.png'), recursive=True):
    src = Image.open(p)
    rgba = src.convert('RGBA')
    px = [c for c in rgba.get_flattened_data() if c[3] > 128] or [(255, 255, 255, 255)]
    col = tuple(int(sum(c[i] for c in px) / len(px)) for i in range(3))
    g = glyph(src.size, col)
    if src.mode == 'LA':
        g = g.convert('LA')
    save(g, p)

# 6. macOS DMG backgrounds (300x180): light panel like the original (Finder draws
#    dark icon labels over it), logo top-left, product name top and bottom-right
for p in glob.glob(os.path.join(NV, 'installers/darwin/*-dmg/background.png')):
    src = Image.open(p)
    W, H = src.size
    s = 4
    bg = Image.new('RGBA', (W * s, H * s), (255, 255, 255, 255))
    d = ImageDraw.Draw(bg)
    logo = tile(30 * s, os_badge=is_os(p))
    bg.paste(logo, (5 * s, 5 * s), logo)
    d.text((W * s / 2 + 10 * s, 14 * s), 'Prestige Viewer', font=font(FONT_T, 12 * s, 800), fill=(25, 25, 28, 255), anchor='mm')
    chan = os.path.basename(os.path.dirname(p)).split('-')[1]
    sub = chan.capitalize() + (' \u00b7 OpenSim' if is_os(p) else '')
    d.text((W * s / 2 + 10 * s, 27 * s), sub, font=font(FONT_T, 8 * s, 600), fill=(214, 110, 0, 255), anchor='mm')
    d.rectangle((0, (H - 3) * s, W * s, H * s), fill=ORANGE + (255,))
    d.text(((W - 6) * s, (H - 8) * s), 'PRESTIGE VIEWER', font=font(FONT_T, 7 * s, 800), fill=(25, 25, 28, 255), anchor='rs')
    out = bg.resize((W, H), Image.LANCZOS).convert(src.mode if src.mode in ('RGB', 'RGBA') else 'RGB')
    save(out, p)

print('\n'.join(changed))
print(len(changed), 'files written', file=sys.stderr)
