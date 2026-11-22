"""Source guards: File > Upload > Script (.lsl / .txt)... is wired end to end (#1531).

  MENU-WIRED    menu_viewer.xml and menu_inventory_add.xml each have a File.UploadScriptFile item;
                llviewermenufile.cpp registers File.UploadScriptFile and starts the picker with
                FFLOAD_LSL_FILE, multi-select, calling start_script_file_upload.
  PICKER        FFLOAD_LSL_FILE is declared and handled in all three filter switches of
                llfilepicker.cpp (SDL, Windows, macOS) with .lsl and .txt.
  BUILD         llscriptfileupload.cpp / .h are listed in indra/newview/CMakeLists.txt.
  STRINGS       Every string and notification the uploader names exists in the en skin.
  COMPILE-PATH  The upload goes through LLScriptAssetUpload with the UpdateScriptAgent
                capability and the "mono" target (what the script editor's Save sends); a failed
                compile is reported with the server's errors, not dropped.
  INPUT-SAFETY  UTF-8 validated, BOM stripped, CRLF converted, an over-limit file refused
                (never truncated), the capability checked before the item is created, and the
                uploader never deletes or overwrites an inventory item.
"""
import re

NAME = "script-file-upload"
STAGE = "source"
DESCRIPTION = "script file upload: menu + picker wiring, UpdateScriptAgent compile path, input safety"
GUARDS = ("the File > Upload > Script menu entry or its picker filter falling out of sync, scripts uploaded "
          "without compiling or with compile errors swallowed, and silent truncation or overwrite (#1531).")

CPP = "indra/newview/llscriptfileupload.cpp"
HDR = "indra/newview/llscriptfileupload.h"
MENUFILE = "indra/newview/llviewermenufile.cpp"
PICKER = "indra/newview/llfilepicker.cpp"
PICKER_H = "indra/newview/llfilepicker.h"
CMAKE = "indra/newview/CMakeLists.txt"
VIEWER_MENU = "indra/newview/skins/default/xui/en/menu_viewer.xml"
INV_MENU = "indra/newview/skins/default/xui/en/menu_inventory_add.xml"
STRINGS = "indra/newview/skins/default/xui/en/strings.xml"
NOTIFS = "indra/newview/skins/default/xui/en/notifications.xml"


def _read(ctx, rel):
    return open(ctx.path(rel), encoding="utf-8").read()


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    cpp = _strip_comments(_read(ctx, CPP))
    menufile = _strip_comments(_read(ctx, MENUFILE))
    picker = _strip_comments(_read(ctx, PICKER))
    picker_h = _strip_comments(_read(ctx, PICKER_H))
    strings = _read(ctx, STRINGS)
    notifs = _read(ctx, NOTIFS)
    cmake = _read(ctx, CMAKE)
    bad = []

    # MENU-WIRED
    for rel in (VIEWER_MENU, INV_MENU):
        if 'function="File.UploadScriptFile"' not in _read(ctx, rel):
            bad.append("MENU-WIRED: %s has no File.UploadScriptFile item" % rel)
    if '"File.UploadScriptFile"' not in menufile:
        bad.append("MENU-WIRED: File.UploadScriptFile is not registered in llviewermenufile.cpp")
    m = re.search(r"class LLFileUploadScriptFile.*?\n};", menufile, flags=re.S)
    if not m:
        bad.append("MENU-WIRED: LLFileUploadScriptFile listener not found")
    elif not re.search(r"startPicker\([^;]*start_script_file_upload[^;]*FFLOAD_LSL_FILE\s*,\s*true\)", m.group(0)):
        bad.append("MENU-WIRED: the listener must start a multi-select FFLOAD_LSL_FILE picker calling "
                   "start_script_file_upload")

    # PICKER
    if "FFLOAD_LSL_FILE" not in picker_h:
        bad.append("PICKER: FFLOAD_LSL_FILE is not declared in llfilepicker.h")
    if len(re.findall(r"FFLOAD_LSL_FILE", picker)) < 3:
        bad.append("PICKER: FFLOAD_LSL_FILE must be handled in the SDL, Windows and macOS filter switches")
    for chunk in picker.split("FFLOAD_LSL_FILE")[1:4]:
        if "LSL_FILE_FILTER" in chunk[:80]:
            continue        # Windows: the macro, checked below
        for ext in ("lsl", "txt"):
            if ext not in chunk[:200]:
                bad.append("PICKER: a script file filter does not accept .%s" % ext)
    if not re.search(r"define LSL_FILE_FILTER[^\n]*\*\.lsl[^\n]*\*\.txt", picker):
        bad.append("PICKER: the Windows LSL_FILE_FILTER does not accept .lsl and .txt")

    # BUILD
    for rel in ("llscriptfileupload.cpp", "llscriptfileupload.h"):
        if not re.search(r"^\s*%s\s*$" % re.escape(rel), cmake, flags=re.M):
            bad.append("BUILD: %s is not in indra/newview/CMakeLists.txt" % rel)

    # STRINGS
    for sid in sorted(set(re.findall(r'LLTrans::getString\("(Script[A-Za-z]+)"', cpp))):
        if 'name="%s"' % sid not in strings:
            bad.append("STRINGS: string %s is missing from strings.xml" % sid)
    for nid in sorted(set(re.findall(r'report\("(ScriptFile[A-Za-z]+)"', cpp))):
        if 'name="%s"' % nid not in notifs:
            bad.append("STRINGS: notification %s is missing from notifications.xml" % nid)

    # COMPILE-PATH
    if 'getRegionCapability("UpdateScriptAgent")' not in cpp:
        bad.append("COMPILE-PATH: UpdateScriptAgent capability is not used")
    if not re.search(r"make_shared<LLScriptAssetUpload>\(\s*new_item_id\s*,\s*kCompileTarget", cpp):
        bad.append("COMPILE-PATH: the upload must be an LLScriptAssetUpload on the new item")
    if 'kCompileTarget = "mono"' not in cpp:
        bad.append("COMPILE-PATH: the compile target must be mono (the editor's default)")
    if 'response["compiled"]' not in cpp or 'response["errors"]' not in cpp or "ScriptFileCompileFailed" not in cpp:
        bad.append("COMPILE-PATH: a failed compile must be reported with the server's errors")
    if "EnqueueInventoryUpload" not in cpp:
        bad.append("COMPILE-PATH: the upload must go through EnqueueInventoryUpload (progress bar)")

    # INPUT-SAFETY
    if "0xEF" not in cpp or "0xBB" not in cpp or "0xBF" not in cpp:
        bad.append("INPUT-SAFETY: the UTF-8 BOM is not stripped")
    if "valid_script_utf8" not in cpp or "ScriptFileNotUtf8" not in cpp:
        bad.append("INPUT-SAFETY: UTF-8 is not validated")
    if "'\\r'" not in cpp:
        bad.append("INPUT-SAFETY: CRLF is not converted to LF")
    if "kMaxScriptSource" not in cpp or "ScriptFileTooLarge" not in cpp or "resize(" in cpp.split("read_script_text", 1)[-1]:
        bad.append("INPUT-SAFETY: an over-limit file must be refused, never truncated")
    start = cpp.split("void start_script_file_upload", 1)[-1]
    if start.find('"UpdateScriptAgent"') < 0 or start.find('"UpdateScriptAgent"') > start.find("create_script_with_source"):
        bad.append("INPUT-SAFETY: the capability must be checked before the item is created")
    for forbidden in ("removeItem", "purgeObject", "removeObject", "moveToTrash", "deleteObject", "updateServer"):
        if forbidden in cpp:
            bad.append("INPUT-SAFETY: the uploader must never delete or rewrite items (%s)" % forbidden)
    if "start_script_file_upload" not in _read(ctx, HDR):
        bad.append("BUILD: start_script_file_upload is not declared in llscriptfileupload.h")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = ("File > Upload > Script wired (viewer + inventory menus, picker, CMake, strings); "
                          "mono compile via UpdateScriptAgent with errors reported; UTF-8/BOM/CRLF, no truncation")
