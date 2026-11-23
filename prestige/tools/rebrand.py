"""Rename the Firestorm viewer to "Prestige Viewer".
Usage: python3 -I rebrand.py <repo_root>

Display name : "Prestige Viewer"
Technical id : "PrestigeViewer" (channel base, exe name, settings/cache folders)

Deliberately NOT renamed (functional or legal):
  * copyright / license notices ("The Phoenix Firestorm Project, Inc.")
  * URLs, Firestorm support groups / wiki / tutorial island references
  * the "#Firestorm" inventory folder and "#Firestorm LSL Bridge" (scripts/inventory rely on them)
  * internal identifiers (setting, colour, class, file names)
"""
import sys, os, re, glob, io

ROOT = sys.argv[1]
NV = os.path.join(ROOT, 'indra/newview')
DISPLAY = 'Prestige Viewer'
TECH = 'PrestigeViewer'
log = {}


def note(path, n):
    if n:
        rel = os.path.relpath(path, ROOT)
        log[rel] = log.get(rel, 0) + n


# ---------------------------------------------------------------- text rules
PROTECT = re.compile(
    r'(?:https?://|www\.)\S+'                   # URLs
    r'|\S+\.(?:org|com|net)\S*'                 # bare domains
    r'|Phoenix[ /-]?Firestorm[\w ]*?(?:Project|Viewer)?(?:,? Inc\.?)?'  # legal entity / Phoenix history
    r'|#Firestorm[\w ]*'                         # protected inventory folder / bridge
    r'|Firestorm LSL Bridge'
    r'|Firestorm[ -]Support[\w ]*'               # real support groups / portal / team
    r'|Firestorm (?:Wiki|wiki|Social Island|Tutorial|Gateway|Preview Group|Beta Testers?)'
    r'|Firestorm%20\S*'
    r'|\[[A-Z0-9_ ]*FIRESTORM[A-Z0-9_ ]*\]'      # [FIRESTORM] substitution tokens
    r'|Firestorm (?:Blog|Primfeed|Flickr|YouTube|Twitter|Plurk|Classes|Events|Discord|Facebook|Instagram|Mastodon|Bluesky|Group)'
    r'|[A-Za-z0-9_./\\-]+Firestorm[A-Za-z0-9_./\\-]*'   # identifiers / paths with Firestorm inside a word
    r'|Firestorm[A-Z0-9_][A-Za-z0-9_./\\-]*'
)
# ASCII-only boundaries so CJK text ("Firestormを") and Polish/Czech inflections
# ("Firestorma", "Firestormem") are matched too.
WORD = re.compile(r'(?:the )?(?<![A-Za-z0-9_])Firestorm(?:a|em|ie|u|owi)?(?: [Vv]iewer)?(?![A-Za-z0-9_])'
                  r'|(?<![A-Za-z0-9_])FIRESTORM(?: VIEWER)?(?![A-Za-z0-9_])')


def brand(text):
    if 'Firestorm' not in text and 'FIRESTORM' not in text:
        return text, 0
    text = text.replace('Firestorm.log', TECH + '.log')
    out, n, pos = [], 0, 0
    for m in PROTECT.finditer(text):
        seg, k = _brand_plain(text[pos:m.start()])
        out.append(seg); n += k
        out.append(m.group(0)); pos = m.end()
    seg, k = _brand_plain(text[pos:])
    out.append(seg); n += k
    return ''.join(out), n


def _brand_plain(s):
    n = 0

    def rep(m):
        nonlocal n
        n += 1
        w = m.group(0)
        if w.isupper():
            return DISPLAY.upper()
        lead = w[:4] if w.startswith('the ') else ''
        return lead + DISPLAY
    return WORD.sub(rep, s), n


# ---------------------------------------------------------------- XUI files
ATTRS = ('label', 'label_selected', 'label_unselected', 'tool_tip', 'title', 'short_title',
         'text', 'ignoretext', 'default_text', 'help_text', 'notext', 'yestext', 'canceltext')
ATTR_RE = re.compile(r'(\s(?:%s)\s*=\s*")([^"]*)(")' % '|'.join(ATTRS))
TAG_OR_COMMENT = re.compile(r'<!--.*?-->|<!\[CDATA\[.*?\]\]>|<[^>]*>', re.S)


LINK_ITEM = re.compile(r'\bname="Firestorm (?:Blog|Classes Schedule|Events Calendar|Flickr|MoTD|Plurk|Primfeed|Twitter|Wiki|YouTube)"')
KEEP_ELEMENTS = re.compile(r'<string name="SUPPORT_SITE">.*?</string>', re.S)


def brand_xml(src):
    keep = KEEP_ELEMENTS.findall(src)
    for i, k in enumerate(keep):
        src = src.replace(k, '\x00KEEP%d\x00' % i, 1)
    new, total = _brand_xml(src)
    for i, k in enumerate(keep):
        new = new.replace('\x00KEEP%d\x00' % i, k, 1)
    return new, total


def _brand_xml(src):
    total = 0
    out, pos = [], 0
    for m in TAG_OR_COMMENT.finditer(src):
        txt, k = brand(src[pos:m.start()]); out.append(txt); total += k
        tok = m.group(0)
        if tok.startswith('<![CDATA['):
            inner, k = brand(tok[9:-3]); tok = '<![CDATA[' + inner + ']]>'; total += k
        elif LINK_ITEM.search(tok):
            pass  # menu items that open Firestorm's own blog/wiki/socials keep their label
        elif not tok.startswith('<!--'):
            def a(mm):
                nonlocal total
                v, k = brand(mm.group(2)); total += k
                return mm.group(1) + v + mm.group(3)
            tok = ATTR_RE.sub(a, tok)
        out.append(tok); pos = m.end()
    txt, k = brand(src[pos:]); out.append(txt); total += k
    return ''.join(out), total


for p in glob.glob(os.path.join(NV, 'skins/*/xui/*/*.xml')):
    if os.path.basename(p) == 'floater_about.xml':
        continue  # credits: handled by hand below so the Firestorm team stays credited
    raw = open(p, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    s = raw.decode('utf-8-sig')
    new, n = brand_xml(s)
    if n:
        open(p, 'wb').write((b'\xef\xbb\xbf' if bom else b'') + new.encode('utf-8'))
        note(p, n)

# APP_NAME family in every language's strings.xml
for p in glob.glob(os.path.join(NV, 'skins/default/xui/*/strings.xml')):
    s = open(p, encoding='utf-8-sig').read()
    s2 = re.sub(r'(<string name="APP_NAME_ABBR">)[^<]*(</string>)', r'\1PV\2', s)
    s2 = re.sub(r'(<string name="APP_NAME">)[^<]*(</string>)', r'\g<1>%s\2' % DISPLAY, s2)
    s2 = re.sub(r'(<string name="CAPITALIZED_APP_NAME">)[^<]*(</string>)', r'\g<1>%s\2' % DISPLAY.upper(), s2)
    if s2 != s:
        raw = open(p, 'rb').read()
        open(p, 'wb').write((b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b'') + s2.encode('utf-8'))
        note(p, 1)

# ---------------------------------------------------------------- About floater (en)
p = os.path.join(NV, 'skins/default/xui/en/floater_about.xml')
s = open(p, encoding='utf-8').read()
s2 = s.replace('For the latest information about Firestorm, visit',
               'Prestige Viewer is based on the Firestorm viewer. For the latest information about Firestorm, visit')
s2 = s2.replace('Firestorm is a community development project',
                'Prestige Viewer is built on Firestorm. Firestorm is a community development project')
if s2 != s:
    open(p, 'w', encoding='utf-8').write(s2); note(p, 2)

# ---------------------------------------------------------------- NSIS language files (UTF-16LE)
for p in glob.glob(os.path.join(NV, 'installers/windows/lang_*.nsi')):
    raw = open(p, 'rb').read()
    bom = raw[:2] == b'\xff\xfe'
    enc = 'utf-16-le' if bom else 'utf-8'
    s = raw[2:].decode(enc) if bom else raw.decode('utf-8')
    lines, total = [], 0
    for line in s.split('\n'):
        if line.startswith('LangString'):
            head, sep, rest = line.partition('"')
            rest, k = brand(rest); total += k
            line = head + sep + rest
        lines.append(line)
    if total:
        out = '\n'.join(lines)
        open(p, 'wb').write((b'\xff\xfe' if bom else b'') + out.encode(enc))
        note(p, total)

# ---------------------------------------------------------------- exact edits (code + build)
EDITS = [
    ('indra/lib/python/indra/util/llmanifest.py', "CHANNEL_VENDOR_BASE = 'Firestorm'", "CHANNEL_VENDOR_BASE = '%s'" % TECH),
    ('scripts/configure_firestorm.sh', 'CHANNEL="Firestorm-$CHANNEL"', 'CHANNEL="%s-$CHANNEL"' % TECH),
    ('indra/newview/viewer_manifest.py', 'channel_base = "Phoenix-" + CHANNEL_VENDOR_BASE', 'channel_base = CHANNEL_VENDOR_BASE'),
    ('indra/newview/viewer_manifest.py', "    def friendly_app_name(self):\n        global CHANNEL_VENDOR_BASE\n        return CHANNEL_VENDOR_BASE",
     "    def friendly_app_name(self):\n        return \"%s\"" % DISPLAY),
    ('indra/newview/fs_viewer_manifest.py', 'return "Phoenix-%(app_name)s_', 'return "%(app_name)s_'),
    ('indra/llcommon/indra_constants.h', 'const std::string APP_NAME = "Firestorm";', 'const std::string APP_NAME = "%s";' % TECH),
    ('indra/llfilesystem/lldir_mac.cpp', 'secondLifeString = "Firestorm";', 'secondLifeString = "%s";' % TECH),
    ('indra/llfilesystem/tests/lldiskcache_test.cpp', '/ "Firestorm";', '/ "%s";' % TECH),
    ('indra/llcrashlogger/llcrashlogger.cpp', '"FirestormCrashReport"', '"%sCrashReport"' % TECH),
    ('indra/llcrashlogger/llcrashlogger.cpp', 'initAppDirs( "Firestorm_x64" )', 'initAppDirs( "%s_x64" )' % TECH),
    ('indra/llcrashlogger/llcrashlogger.cpp', 'initAppDirs("Firestorm")', 'initAppDirs("%s")' % TECH),
    ('indra/llcrashlogger/llcrashlogger.cpp', 'mProductName = "Firestorm";', 'mProductName = "%s";' % DISPLAY),
    ('indra/newview/llappviewerwin32.cpp', '"Firestorm.log"', '"%s.log"' % TECH, 2),
    ('indra/newview/llappviewerwin32.cpp', '"Firestorm.dmp"', '"%s.dmp"' % TECH),
    ('indra/newview/llvelopack.cpp', 'std::wstring prefix = L"Firestorm";', 'std::wstring prefix = L"%s";' % TECH),
    ('indra/newview/llvelopack.cpp', '"\\\\Firestorm_x64\\\\user_settings', '"\\\\%s_x64\\\\user_settings' % TECH),
    ('indra/newview/llvelopack.cpp', '(BYTE*)L"The Phoenix Firestorm Project, Inc.", 72);',
     '(BYTE*)L"%s", %d);' % (DISPLAY, (len(DISPLAY) + 1) * 2)),
    ('indra/newview/llvoavatarself.cpp', 'const char* tag_client = "Firestorm";', 'const char* tag_client = "%s";' % DISPLAY),
    ('indra/newview/llfloaterflickr.cpp', 'DEFAULT_TAG_TEXT = "Firestorm ";', 'DEFAULT_TAG_TEXT = "%s ";' % TECH),
    ('indra/newview/fsprimfeedconnect.h', '"Firestorm-PF"', '"%s-PF"' % TECH),
    ('indra/newview/desktopnotifierlinux.cpp', '"Firestorm Viewer"', '"%s"' % DISPLAY),
    ('indra/newview/desktopnotifierlinux.cpp', 'mNotificationNew( "Firestorm",', 'mNotificationNew( "%s",' % DISPLAY),
    ('indra/newview/growlmanager.cpp', '"Firestorm Viewer"', '"%s"' % DISPLAY),
    ('indra/newview/llappviewerlinux.cpp', '"Firestorm closed unexpectedly."', '"%s closed unexpectedly."' % DISPLAY),
    ('indra/newview/llappviewer.cpp', 'but Firestorm has crashed', 'but %s has crashed' % DISPLAY),
    ('indra/linux_crash_logger/linux_crash_logger.cpp', '"Firestorm crash report', '"%s crash report' % DISPLAY),
    ('indra/linux_crash_logger/linux_crash_logger.cpp', '"Firestorm closed unexpectedly.', '"%s closed unexpectedly.' % DISPLAY),
    ('indra/llui/lltransutil.cpp', '"Firestorm couldn\'t access', '"%s couldn\'t access' % DISPLAY),
    ('indra/llcommon/llerror.cpp', '"Firestorm couldn\'t access', '"%s couldn\'t access' % DISPLAY),
    ('indra/llxml/llcontrol.cpp', '"Firestorm failed to initialize', '"%s failed to initialize' % DISPLAY),
    ('indra/llwindow/llwindowmacosx.cpp', 'which Firestorm does not support', 'which %s does not support' % DISPLAY),
    ('indra/llwindow/llwindowmacosx.cpp', '"Firestorm requires', '"%s requires' % DISPLAY),
    ('indra/llwindow/llwindowmacosx.cpp', 'try to run Firestorm,', 'try to run %s,' % DISPLAY),
    # macOS bundle display name (bundle/executable file names stay internal)
    ('indra/newview/CMakeLists.txt', 'set(MACOSX_BUNDLE_INFO_STRING "Firestorm Viewer")', 'set(MACOSX_BUNDLE_INFO_STRING "%s")' % DISPLAY),
    ('indra/newview/CMakeLists.txt', 'set(MACOSX_BUNDLE_BUNDLE_NAME "Firestorm")', 'set(MACOSX_BUNDLE_BUNDLE_NAME "%s")' % DISPLAY),
    ('indra/newview/CMakeLists.txt', 'set(product Firestorm-${ARCH}', 'set(product %s-${ARCH}' % TECH),
    # Windows installer: own registry key / publisher, clean up our own folders on uninstall
    ('indra/newview/installers/windows/installer_template.nsi', '"SOFTWARE\\The Phoenix Firestorm Project"', '"SOFTWARE\\%s"' % DISPLAY),
    ('indra/newview/installers/windows/installer_template.nsi', '"Publisher" "The Phoenix Firestorm Project, Inc."', '"Publisher" "%s"' % DISPLAY),
    # Linux desktop entries
    ('indra/newview/installers/linux/firestorm.desktop', 'Name=Firestorm', 'Name=%s' % DISPLAY),
    ('indra/newview/linux_tools/refresh_desktop_app_entry.sh', 'Name=Firestorm Viewer', 'Name=%s' % DISPLAY, 2),  # Name= and GenericName=
]
for e in EDITS:
    rel, old, new = e[:3]
    want = e[3] if len(e) > 3 else 1
    p = os.path.join(ROOT, rel)
    s = open(p, encoding='utf-8', newline='').read()
    c = s.count(old)
    if c != want:
        sys.exit('EDIT MISMATCH %s: found %d of %r' % (rel, c, old))
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
    note(p, c)

# NSIS uninstall cleanup paths: AppData\...\Firestorm -> PrestigeViewer (and _x64 / OS variants)
p = os.path.join(NV, 'installers/windows/installer_template.nsi')
s = open(p, encoding='utf-8', newline='').read()
s2, n = re.subn(r'(AppData\\(?:Roaming|Local)\\|ProgramData\\|\$0\\)Firestorm(OS)?(_x64)?(?=[\\"\s])',
                lambda m: m.group(1) + TECH + (m.group(2) or '') + (m.group(3) or ''), s)
open(p, 'w', encoding='utf-8', newline='').write(s2); note(p, n)

# macOS menu bar titles in the nib source (title= only; autosave names stay)
p = os.path.join(NV, 'Firestorm.xib')
s = open(p, encoding='utf-8', newline='').read()
s2, n = re.subn(r'(\btitle=")([^"]*)Firestorm([^"]*")', lambda m: m.group(1) + m.group(2) + DISPLAY + m.group(3), s)
open(p, 'w', encoding='utf-8', newline='').write(s2); note(p, n)

# Linux installer messages / readme
for rel in ('indra/newview/linux_tools/install.sh', 'indra/newview/linux_tools/FIRESTORM_DESKTOPINSTALL.txt'):
    p = os.path.join(ROOT, rel)
    s = open(p, encoding='utf-8', newline='').read()
    out, n = [], 0
    for line in s.split('\n'):
        if 'Firestorm' in line and not line.lstrip().startswith(('# Phoenix', '#!/')):
            line, k = brand(line); n += k
        out.append(line)
    open(p, 'w', encoding='utf-8', newline='').write('\n'.join(out)); note(p, n)

for k in sorted(log):
    print('%5d  %s' % (log[k], k))
print('files:', len(log), ' replacements:', sum(log.values()), file=sys.stderr)
