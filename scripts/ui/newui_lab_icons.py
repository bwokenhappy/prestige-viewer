#!/usr/bin/env python3
"""Skin images for slrmluilab's stand-in of the viewer's llimage() decorator (#703).

The lab has no LLUIImage list, so the new interface's documents (llnewuidocs.h) draw their
icons from PNGs this script writes, named after the textures.xml entries:
  <out>/<name>.png        white through the image's alpha (llimage's "solid" mode: the
                          element's image-color tints it, as in the viewer)
  <out>/<name>.color.png  the image as the viewer draws it in "scale" mode (the skin bitmap)
Vector entries (vector_file, the Phosphor icons) are rasterised with rsvg-convert.

Usage: newui_lab_icons.py <out dir> [name ...]   (no names: every texture the lab's scenes use)
"""
import os
import re
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEXTURES = os.path.join(ROOT, "indra/newview/skins/default/textures")

# What indra/rmluilab/rmluilab.cpp's --newui scenes draw: the default toolbars' commands
# (app_settings/toolbars.xml) and a few inventory types.
DEFAULT = [
    "Command_Chat_Icon", "Command_Speak_Icon", "Command_People_Icon", "Command_Profile_Icon", "Command_Map_Icon",
    "Command_Move_Icon", "Command_View_Icon", "Command_Snapshot_Icon", "Command_HowTo_Icon", "Command_Avatar_Icon",
    "Command_Appearance_Icon", "Command_Inventory_Icon", "Command_Search_Icon", "Command_Places_Icon",
    "Command_Destinations_Icon", "Command_MiniMap_Icon", "Command_Environments_Icon", "Command_Stats_Icon",
    "Command_Performance_Icon", "Command_QuickPrefs_Icon", "Command_Photo_Studio_Icon", "Command_Build_Icon",
    "Command_Preferences_Icon", "Inv_FolderClosed", "Inv_FolderOpen", "Inv_Clothing", "Inv_Object", "Inv_Notecard",
    "Inv_Texture", "Inv_Landmark",
    # the status bar (llnewuistatusdocs.h)
    "Parcel_VoiceNo_Dark", "Parcel_FlyNo_Dark", "Parcel_PushNo_Dark", "Parcel_BuildNo_Dark", "Parcel_ScriptsNo_Dark",
    "Parcel_SeeAVsOff_Light", "Parcel_Health_Dark", "Presets_Icon", "Cam_FreeCam_Off", "Inv_Gesture", "Play_Off", "Pause_Off",
    "Audio_Off", "AudioMute_Off", "Icon_File_Upload", "Home_Off", "Favorite_Star_Off",
    "Command_Voice_Icon", "Parcel_Voice_Dark", "Nearby_chat_icon", "Parcel_Build_Dark", "Parcel_ForSale_Light", "TabIcon_People_Off",
]


def entries():
    text = open(os.path.join(TEXTURES, "textures.xml"), encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"<texture\s+([^>]*?)/?>", text, flags=re.S):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        if "name" in attrs:
            out[attrs["name"]] = attrs
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    names = sys.argv[2:] or DEFAULT
    os.makedirs(out, exist_ok=True)
    table = entries()
    missing = []
    for name in names:
        e = table.get(name)
        if not e:
            missing.append(name)
            continue
        bitmap = os.path.join(TEXTURES, e.get("file_name", ""))
        if e.get("file_name") and os.path.isfile(bitmap):
            shutil.copyfile(bitmap, os.path.join(out, name + ".color.png"))
        source = None
        if e.get("vector_file"):
            raster = os.path.join(out, name + ".vector.png")
            subprocess.run(["rsvg-convert", "-w", "96", "-h", "96", "-o", raster, os.path.join(TEXTURES, e["vector_file"])], check=True)
            source = raster
        elif os.path.isfile(bitmap):
            source = bitmap
        if not source:
            missing.append(name)
            continue
        img = Image.open(source).convert("RGBA")
        alpha = img.getchannel("A")
        white = Image.new("RGBA", img.size, (255, 255, 255, 0))
        white.putalpha(alpha)
        white.save(os.path.join(out, name + ".png"))
        if not os.path.isfile(os.path.join(out, name + ".color.png")):
            img.save(os.path.join(out, name + ".color.png"))
    if missing:
        print("missing: " + ", ".join(missing))
        return 1
    print("%d images in %s" % (len(names), out))
    return 0


sys.exit(main())
