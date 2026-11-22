#!/usr/bin/env python3
"""Regenerate indra/newview/app_settings/settings_export_map.xml (#1423).

"Export for another viewer" writes only the settings listed there: the ones that have the same
name, group (global / per-account), type and meaning in the other viewer. Run from the
repository root with the other viewers' declared settings (their indra/newview/app_settings):

    python3 scripts/viewer_import/gen_settings_export_map.py \\
        --secondlife <dir with LL settings.xml + settings_per_account.xml> \\
        --firestorm  <dir with Firestorm settings.xml + settings_per_account.xml> [--report]

A setting is listed for a viewer when
  1. both viewers declare it in the same file, with the same Type, and both save it (Persist != 0);
  2. it is not denied by viewer_import_map.xml (credentials, install / machine / session values);
  3. a Render* setting is on the importer's renderer allowlist (our Vulkan renderer's tuning does
     not mean the same in a GL viewer);
  4. it is not on SAME_NAME_OTHER_MEANING below (units, enums or behaviour that differ).
--report prints the listed settings whose comment or default differs, for review.
Needs the 'llsd' Python package.

Researched against secondlife/viewer develop and FirestormViewer/phoenix-firestorm master
(fetched 2026-11-19).
"""
import argparse
import os
import sys

import llsd

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
APP = os.path.join(ROOT, 'indra', 'newview', 'app_settings')
OUT = os.path.join(APP, 'settings_export_map.xml')

# Same name, different meaning (found by comparing comments, defaults and code).
SAME_NAME_OTHER_MEANING = {
    'SceneLoadRearMaxRadiusFraction',   # ours is a percentage (75), LL's a fraction (0.75)
    'NearbyPeopleSortOrder',            # enum values differ (LL 4 = most recent, ours 5)
    'TargetFPS', 'TuningFPSStrategy', 'AutoTuneLock', 'KeepAutoTuneLock', 'AutoTuneFPS',
    'AutoTuneImpostorByDistEnabled', 'AutoTuneImpostorFarAwayDistance', 'AutoTuneRenderFarClipMin',
    'AutoTuneRenderFarClipTarget',      # #422 auto-tuner semantics
    'BackgroundYieldTime',              # legacy here; we pace background frames differently
    'FullScreen', 'FullScreenMode',     # #919 fullscreen kinds
    'UpdaterWillingToTest', 'UpdaterShowReleaseNotes', 'UpdaterServiceSetting',
    'AllowMultipleViewers',             # we share settings files between instances safely; LL does not
    'QuickBuyCurrency',                 # deprecated in LL
    'always_showable_floaters',         # floater names differ between viewers
    'InventoryOutboxMakeVisible',
    'SkinCurrent', 'SkinCurrentTheme', 'UIRenderer',
    'Mesh2MaxConcurrentRequests', 'MeshMaxConcurrentRequests',   # tuned for our fetcher (#fetch limits)
    'TextureFetchConcurrency', 'AssetFetchConcurrency',
    'FSRadarColumnConfig', 'FSAreaSearchColumnConfig',   # column bitmasks: our radar / area search have other columns
    'JoystickInitialized',              # names the device detected on this machine
    'LastInventoryInboxActivity',       # session state (a timestamp)
}
# Values that are URLs of our services or theirs, or identify this install.
DENY_SUFFIXES = ('Rect', 'URL', 'Url', 'Uri', 'URI', 'Host', 'Port', 'Path', 'Location', 'Dir', 'Directory', 'Version')


def load(path):
    with open(path, 'rb') as f:
        return llsd.parse(f.read())


def persisted(v):
    return int(v.get('Persist', 1)) != 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secondlife', required=True)
    ap.add_argument('--firestorm', required=True)
    ap.add_argument('--report', action='store_true')
    args = ap.parse_args()

    ours = {'global': load(os.path.join(APP, 'settings.xml')),
            'account': load(os.path.join(APP, 'settings_per_account.xml'))}
    imap = load(os.path.join(APP, 'viewer_import_map.xml'))['settings']
    import re
    deny_exact = set(imap['deny_exact'])
    deny_regex = [re.compile(r) for r in imap['deny_regex']]
    deny_contains = [c.lower() for c in imap['deny_contains']] + ['mfahash', 'apikey', 'secret']
    render_allow = set(imap['render_allow'])

    def denied(name):
        low = name.lower()
        if any(c in low for c in deny_contains): return 'secret'
        if name in deny_exact or any(r.search(name) for r in deny_regex): return 'install'
        if name.startswith('Render') and name not in render_allow: return 'renderer'
        if name in SAME_NAME_OTHER_MEANING: return 'meaning'
        if name.endswith(DENY_SUFFIXES): return 'install'
        return None

    out = {'researched': 'secondlife/viewer develop, FirestormViewer/phoenix-firestorm master (2026-11-19)'}
    for target, folder in (('secondlife', args.secondlife), ('firestorm', args.firestorm)):
        theirs = {'global': load(os.path.join(folder, 'settings.xml')),
                  'account': load(os.path.join(folder, 'settings_per_account.xml'))}
        result = {}
        why_counts = {}
        for group in ('global', 'account'):
            names = []
            for name, v in sorted(ours[group].items()):
                if not persisted(v):
                    continue
                t = theirs[group].get(name)
                if t is None or t.get('Type') != v.get('Type') or not persisted(t):
                    continue
                why = denied(name)
                if why:
                    why_counts[why] = why_counts.get(why, 0) + 1
                    continue
                names.append(name)
                if args.report and (t.get('Comment', '').split() != v.get('Comment', '').split() or t.get('Value') != v.get('Value')):
                    print('%s %s: ours %r | theirs %r -- %s' % (target, name, v.get('Value'), t.get('Value'),
                                                              ' '.join(t.get('Comment', '').split())[:100]))
            result[group] = names
        out[target] = result
        print('%s: %d global + %d per-account settings shared; left out: %s'
              % (target, len(result['global']), len(result['account']), why_counts))

    with open(OUT, 'wb') as f:
        f.write(llsd.format_pretty_xml(out))
    print('wrote', os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    sys.exit(main())
