"""New tiered graphics settings must declare their upgrade-default policy."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

NAME = "graphics-feature-defaults"
STAGE = "source"
DESCRIPTION = "new persisted tier settings registered/exempted; per-setting startup policy and saved-value preservation"
GUARDS = "upgraded Low/custom profiles inherit new high-end defaults; explicit factory-equal choices overwritten"
BASELINE_HASH = "12be1d86718dc63fdc5db6616167f8a5b418009aeeb8c6c6db98f9618f967b15"


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def setting_maps(path):
    children = list(ET.parse(path).getroot().find('map'))
    out = {}
    for key, value in zip(children[::2], children[1::2]):
        fields = list(value)
        out[key.text] = {k.text: v for k, v in zip(fields[::2], fields[1::2])}
    return out


def tier_settings(path):
    section, out = '', set()
    for line in path.read_text().splitlines():
        words = line.split('//')[0].split()
        if words and words[0] == 'list':
            section = words[1]
        elif len(words) == 3 and section not in ('', 'all'):
            out.add(words[0])
    return out


def run(ctx, result):
    root = Path(ctx.root) if hasattr(ctx, 'root') else Path(ctx.path(''))
    nv = root / 'indra/newview'
    header = (nv / 'slgraphicsfeaturedefaults.h').read_text()
    source = (nv / 'slgraphicsfeaturedefaults.cpp').read_text()
    app = (nv / 'llappviewer.cpp').read_text()
    feature = (nv / 'llfeaturemanager.cpp').read_text()
    controls = (root / 'indra/llxml/llcontrol.cpp').read_text()
    schema = setting_maps(nv / 'app_settings/settings.xml')
    baseline = json.loads((root / 'scripts/check/data/graphics_feature_defaults_baseline.json').read_text())
    names = baseline['settings']
    if hashlib.sha256('\n'.join(names).encode()).hexdigest() != BASELINE_HASH:
        result.fail('the Beta 3 baseline is frozen; register new features, do not refresh the baseline')
    registered = re.findall(r'"(Render\w+)"', header.split('settingNames = {', 1)[1].split('};', 1)[0])
    exemptions = baseline['exemptions']
    if len(registered) != len(set(registered)) or len(registered) > 64:
        result.fail('duplicate/oversized feature registry')
    if set(registered) & set(exemptions) or any(not isinstance(v, str) or len(v.strip()) < 12 for v in exemptions.values()):
        result.fail('each explicit exemption needs a reason and cannot also be registered')
    tiered = set().union(*(tier_settings(nv / f) for f in ('featuretable.txt', 'featuretable_linux.txt', 'featuretable_mac.txt')))
    persisted = {k for k, v in schema.items() if v.get('Persist') is None or v['Persist'].text.strip().lower() not in ('0', 'false')}
    if set(exemptions) - ((tiered & persisted) - set(names)):
        result.fail('stale exemptions must be removed or reviewed for the current tier settings')
    missing = (tiered & persisted) - set(names) - set(registered) - set(exemptions)
    if missing:
        result.fail('new persisted tier settings need registration or reasoned exemption: ' + ', '.join(sorted(missing)))
    # Mutation oracle: adding a new tiered persistent feature must fail this policy.
    assert {'RenderFutureCostlyFeature'} == ((tiered | {'RenderFutureCostlyFeature'}) & (persisted | {'RenderFutureCostlyFeature'})) - set(names) - set(registered) - set(exemptions)
    for name in registered:
        if name not in persisted or name not in tiered or schema[name]['Type'].text not in ('Boolean','F32','U32','S32'):
            result.fail(name + ': registry entry must be a persisted numeric tier setting')
    marker = schema.get('GraphicsFeatureDefaultsApplied', {})
    if marker.get('Persist') is None or marker['Persist'].text != '1' or marker['Type'].text != 'LLSD':
        result.fail('completion map must be persistent LLSD')
    call = 'SLGraphicsFeatureDefaults::initialize(gSavedSettings, *fm, gSavedSettings.getString("ClientSettingsFile"));'
    if app.count(call) != 1 or not app.index('fm->applyRecommendedSettings();') < app.index(call) < app.index('const U32 tc_marker'):
        result.fail('initialize once after GPU/preset choice and before renderer startup')
    if 'SLGraphicsFeatureDefaults::noteOverride(group_part.empty() ? name : name_part);' not in app or 'SLGraphicsFeatureDefaults::noteOverrideFile(full_settings_path);' not in app:
        result.fail('equal-default CLI/session overrides must be recorded too')
    for token in ('saved.has(name) && std::as_const(saved)[name].has("Value")', 'PERSIST_ALWAYS', 'if (!pending)', 'return; // no settings-file read', 'fileLimit = 16u * 1024u * 1024u', 'std::string text(bytes + 1u', 'done.size() >= 64', 'catch (const std::bad_alloc&)'):
        if token not in source:
            result.fail('startup safety/preservation contract missing: ' + token)
    if 'applyRecommendedSettings' in source or 'setGraphicsLevel(' in source or 'LastFeatureVersion' in source:
        result.fail('new defaults must not reset a preset/feature-table version')
    cxx = os.environ.get('CXX') or shutil.which('c++')
    if not cxx:
        result.fail('C++ compiler required for actual initializer fixture')
        return
    fixture = (root / 'scripts/check/test_graphics_feature_defaults.cpp').read_text()
    fixture += '\n' + function(controls, 'bool LLControlVariable::shouldSave(')
    for signature in ('void LLFeatureList::addFeature(', 'bool LLFeatureList::isFeatureAvailable(', 'F32 LLFeatureList::getRecommendedValue(', 'bool LLFeatureList::maskList('):
        fixture += '\n' + function(feature, signature)
    fixture += '''\nnamespace SLGraphicsFeatureDefaults {
std::bitset<settingNames.size()> overrides; bool unreadableOverride=false;
LLSD savedInput; bool readOK=true; int readCalls=0;
bool readSaved(const std::string&,LLSD& output){++readCalls;output=savedInput;return readOK;}
'''
    for signature in ('bool completed(', 'bool validMarker(', 'bool numericValue(', 'void noteOverride(', 'void initialize('):
        fixture += '\n' + function(source, signature)
    fixture += '\n}\n' + (root / 'scripts/check/test_graphics_feature_defaults_main.inc').read_text()
    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp) / 'check.cpp', Path(tmp) / 'check'
        cpp.write_text(fixture)
        run = subprocess.run([cxx,'-std=c++20','-O1','-Wall','-Wextra','-Werror','-I',str(nv),str(cpp),'-o',str(exe)],capture_output=True,text=True)
        if run.returncode:
            result.fail(run.stdout + run.stderr)
            return
        run = subprocess.run([str(exe)],capture_output=True,text=True)
        if run.returncode:
            result.fail(run.stdout + run.stderr + 'fixture exit ' + str(run.returncode))
            return
    result.summary = '3 tiered settings; production initializer/masking/saver: all7 tiers, explicit/default equality, once-only, transient overrides, invalid data/OOM/bounds; future-feature mutation caught'
