#!/usr/bin/env python3
"""IM icons in the top bar (#1489, SLShowIMChiclets, slimchiclet.cpp). Source checks, no viewer:
  - every action the three IM icon menus send is handled for that kind, and nothing else is;
  - the widget's child names are the ones the class and LLIMChiclet look up;
  - the setting exists, defaults off, and the bar shows / removes the icons when it changes;
  - the preference checkbox drives that setting.
Usage: python3 slimchiclet_checks.py [--source-root DIR]
"""
import argparse
import re
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--source-root", default=str(Path(__file__).resolve().parents[3]))
root = Path(ap.parse_args().source_root)
nv = root / "indra/newview"
xui = nv / "skins/default/xui/en"
cpp = (nv / "slimchiclet.cpp").read_text()
bar = (nv / "llchicletbar.cpp").read_text()
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += 0 if ok else 1


def handled(kind):
    m = re.search(r"case slimchiclet::Kind::" + kind + r":(.*?)break;", cpp, re.S)
    return set(re.findall(r'a == "([^"]+)"', m.group(1))) if m else set()


for kind, menu in (("Person", "p2p"), ("Group", "group"), ("Conference", "adhoc")):
    text = (xui / f"menu_imchiclet_{menu}.xml").read_text()
    sent = set(re.findall(r'function="IMChicletMenu\.Action"\s+parameter="([^"]+)"', text))
    check(bool(sent) and sent == handled(kind),
          f"{kind}: menu_imchiclet_{menu}.xml sends {sorted(sent)}, the class handles {sorted(handled(kind))}")

widget = (xui / "widgets/chiclet_im.xml").read_text()
for child in ("chiclet_button", "avatar_icon", "group_icon", "conference_icon"):
    check(f'chiclet_im.{child}' in widget, f"widget defines {child}")
check('name="chiclet_button"' in widget, "the button is named chiclet_button (LLIMChiclet::postBuild finds it)")
check('Register<SLIMChiclet> r_sl_chiclet_im("chiclet_im")' in cpp, "the widget tag is registered")

check("showConversation(session_id)" in cpp and "closeHostedFloater()" in cpp,
      "a click shows the conversation selected in Conversations, or hides it (keeping every conversation)")
check("setShowNewMessagesIcon(mCounter > 0)" in cpp and "new_message_icon" in widget, "unread: a dot on the picture")
check("chiclet_im.speaker" in widget and "setSpeakerId(" in cpp, "a speaking indicator for the conversation's voice")

settings = (root / "indra/newview/app_settings/settings.xml").read_text()
m = re.search(r"<key>SLShowIMChiclets</key>\s*<map>(.*?)</map>", settings, re.S)
check(bool(m) and re.search(r"<key>Value</key>\s*<integer>0</integer>", m.group(1)) is not None,
      "SLShowIMChiclets exists and defaults off")
check('getControl("SLShowIMChiclets")' in bar and "getSignal()->connect" in bar,
      "the bar follows the setting live")
check("removeSessionObserver(this)" in bar and "dynamic_cast<SLIMChiclet*>" in bar,
      "turning it off stops observing and removes only the IM icons")
check("mId2SessionMap" in bar, "turning it on adds the conversations already open")
prefs = (xui / "panel_preferences_chat_im.xml").read_text()
check('control_name="SLShowIMChiclets"' in prefs, "the IM preferences checkbox drives the setting")

print("PASSED (0 failed)" if not failures else f"FAILED ({failures})")
sys.exit(1 if failures else 0)
