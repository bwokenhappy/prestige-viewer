"""The new interface's floating windows (#703): their geometry, saved state and wiring.

Compiles scripts/check/test_newui_window_layout.cpp with the host C++ compiler and runs it: windows
clamped on screen (the UI scale and the window size change under them), snapping to the screen and to
other windows, the saved-state string round trip (UINewWindowState), z order, the pull-up tool grid's
open / grace / lock / hold rule.

Source rules:
  LAYOUT    llnewuiwindow.cpp moves, resizes, clamps and saves through llnewuiwindowlayout.h (one
            geometry for the tests and the windows); the tool grid opens through slwin::PullUp.
  KINDS     every window kind registered in llnewuiwindowkinds.h is defined, names a document in
            llnewuiwindowdocs.h and a data model the newui-kit check maps to its controller.
  SETTINGS  UINewWindowState persists (Persist 1); UINewWindowsDev is development only (Persist 0,
            off: the classic windows stay until a native one reaches parity); UINewToolGridLocked
            exists; the retired UINewDocksDev / UINewToolGridCollapsed are gone.
  WIDTH     the tool grid's bound width is never empty ('width: ;' warnings before it was set).
  DOCKS     the retired docks (llnewuipanels) are gone.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "newui-windows"
STAGE = "source"
DESCRIPTION = "new interface windows: clamp / snap / saved state / pull-up tests; kinds, settings and wiring"
GUARDS = "Windows lost off screen or overlapping after a UI scale change; saved places lost; a window kind without its document."

HEADER = "indra/newview/llnewuiwindowlayout.h"
TEST = "scripts/check/test_newui_window_layout.cpp"
FRAMEWORK = "indra/newview/llnewuiwindow.cpp"
KINDS = "indra/newview/llnewuiwindowkinds.h"
DOCS = "indra/newview/llnewuiwindowdocs.h"
TOOLS = "indra/newview/llnewuitools.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
NEWVIEW = "indra/newview"


def _read(ctx, rel):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _code(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def _setting(xml, name):
    m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % re.escape(name), xml, flags=re.S)
    if not m:
        return None
    body = m.group(1)
    persist = re.search(r"<key>Persist</key>\s*<integer>(\d)</integer>", body)
    value = re.search(r"<key>Value</key>\s*<(\w+)\s*/?>([^<]*)", body)
    return {"persist": persist.group(1) if persist else None, "value": value.group(2).strip() if value else None}


def source_rules(ctx):
    bad = []
    fw = _code(_read(ctx, FRAMEWORK))
    for fn in ("slwin::clampToScreen(", "slwin::snapMove(", "slwin::snapResize(", "slwin::encode(", "slwin::decode(", "slwin::raise("):
        if fn not in fw:
            bad.append("LAYOUT: %s does not use %s" % (FRAMEWORK, fn))
    if "slwin::PullUp" not in _code(_read(ctx, TOOLS)):
        bad.append("LAYOUT: %s does not open the grid through slwin::PullUp" % TOOLS)

    kinds = _code(_read(ctx, KINDS))
    docs = _read(ctx, DOCS)
    registers = re.findall(r"void (slNewUIRegister\w+)\(LLNewUIWindows& windows\);\s*\S*\s*(\w+\.cpp)?", _read(ctx, KINDS))
    if not registers:
        bad.append("KINDS: no window kinds declared in %s" % KINDS)
    for fn, _ in registers:
        if fn + "(windows)" not in kinds:
            bad.append("KINDS: %s is declared but slNewUIRegisterWindows does not call it" % fn)
        defined = None
        for root, _, files in os.walk(ctx.path(NEWVIEW)):
            for f in files:
                if f.endswith(".cpp"):
                    text = _read(ctx, os.path.relpath(os.path.join(root, f), ctx.path(".")))
                    if re.search(r"void %s\(LLNewUIWindows& windows\)\s*\{" % fn, text):
                        defined = text
        if defined is None:
            bad.append("KINDS: %s is defined nowhere" % fn)
            continue
        for doc in re.findall(r"LLNewUIWindowDocs::(k\w+Rml)", defined):
            if not re.search(r"%s = R\"rml\(" % doc, docs):
                bad.append("KINDS: %s uses %s, which is not in %s" % (fn, doc, DOCS))
        for model in re.findall(r'const char\* model\(\) const override \{ return "(\w+)"; \}', defined):
            if not re.search(r'data-model="%s"' % model, docs):
                bad.append("KINDS: model %r has no document with data-model=\"%s\"" % (model, model))

    xml = _read(ctx, SETTINGS)
    state = _setting(xml, "UINewWindowState")
    if not state or state["persist"] != "1":
        bad.append("SETTINGS: UINewWindowState must exist with Persist 1 (windows keep their places)")
    dev = _setting(xml, "UINewWindowsDev")
    if not dev or dev["persist"] != "0" or dev["value"] != "0":
        bad.append("SETTINGS: UINewWindowsDev must exist, Persist 0, off (classic windows until parity)")
    if not _setting(xml, "UINewToolGridLocked"):
        bad.append("SETTINGS: UINewToolGridLocked is missing (the pull-up grid's lock)")
    for gone in ("UINewDocksDev", "UINewToolGridCollapsed"):
        if _setting(xml, gone):
            bad.append("SETTINGS: %s was retired; remove it" % gone)

    if not re.search(r'std::string\s+width\s*=\s*"\d+dp"', _read(ctx, TOOLS)):
        bad.append("WIDTH: the tool grid's width must start as a length (an empty one is 'width: ;')")

    for f in ("llnewuipanels.cpp", "llnewuipanels.h"):
        if os.path.exists(ctx.path(os.path.join(NEWVIEW, f))):
            bad.append("DOCKS: %s was retired with the docks; remove it" % f)
    return bad


def navigation_fixture(ctx, cxx, directory):
    source = _read(ctx, "indra/newview/llnewui.cpp")
    functions = []
    for signature in ("void runNavigationChoice(size_t index)", "bool navigationKey(KEY key)"):
        start = source.find(signature + "\n{")
        if start < 0:
            return "missing native navigation function: " + signature
        opening = source.find("{", start)
        depth = 1
        end = opening + 1
        while depth and end < len(source):
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if depth:
            return "unterminated native navigation function"
        functions.append(source[start:end])
    chat_source = _read(ctx, "indra/newview/llnewuichat.cpp")
    signature = "void sessionIDUpdated(const LLUUID& old_session_id, const LLUUID& new_session_id) override"
    start = chat_source.find(signature)
    if start < 0:
        return "missing native session rebind observer"
    opening = chat_source.find("{", start)
    depth, end = 1, opening + 1
    while depth and end < len(chat_source):
        depth += (chat_source[end] == "{") - (chat_source[end] == "}")
        end += 1
    if depth:
        return "unterminated native session rebind observer"
    rekey = chat_source[start:end]
    fixture = r"""
#include <vector>
#include <functional>
#include <cstdio>
#include <cstdint>
#include <string>
using KEY=int;
constexpr KEY KEY_ESCAPE=1,KEY_RETURN=2,KEY_UP=3,KEY_DOWN=4;
struct Model { void DirtyVariable(const char*) {} };
struct State { bool navOpen=true; int navSelected=-1,navMode=1;
    std::vector<std::function<void()>> navActions; std::vector<int> navEnabled; Model barModel; };
State* gState=nullptr;
int submitted=0,executed=0;
bool closedBeforeAction=false;
void closeNavigation() { gState->navOpen=false; gState->navActions.clear(); }
void submitAddress() { ++submitted; closeNavigation(); }
void markChanged() {}
// FUNCTIONS
using LLUUID=int;
struct SessionObserver { virtual void sessionIDUpdated(const LLUUID&,const LLUUID&)=0; };
struct SessionBinding:SessionObserver
{
    struct Conv { LLUUID session; };
    std::vector<Conv> mConvs;
    std::vector<std::pair<LLUUID,std::string>> mQueued;
    uint64_t mRevision=4;
    bool mForce=false,mMessagesPending=false;
    // SESSION_REKEY
};
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main()
{
    State state;
    const auto reset=[&] { state=State{};gState=&state;submitted=executed=0;closedBeforeAction=false; };
    const auto action=[] { ++executed;closedBeforeAction=!gState->navOpen; };
    CHECK(!navigationKey(KEY_ESCAPE));
    reset(); CHECK(!navigationKey(90)); CHECK(state.navOpen);
    CHECK(navigationKey(KEY_ESCAPE)); CHECK(!state.navOpen && !submitted && !executed);
    reset(); CHECK(navigationKey(KEY_RETURN)); CHECK(submitted==1 && !executed && !state.navOpen);
    reset(); state.navMode=2;state.navActions={action,action,action};state.navEnabled={0,1,0};
    CHECK(navigationKey(KEY_DOWN));CHECK(state.navSelected==1);
    CHECK(navigationKey(KEY_UP));CHECK(state.navSelected==1);
    CHECK(navigationKey(KEY_RETURN));CHECK(executed==1 && !submitted && closedBeforeAction);
    reset();state.navActions={action,action};state.navEnabled={0,0};
    CHECK(navigationKey(KEY_DOWN));CHECK(state.navSelected==-1 && !executed && state.navOpen);
    runNavigationChoice(100);CHECK(!executed && state.navOpen);
    runNavigationChoice(0);CHECK(!executed && state.navOpen);
    SessionBinding binding;
    binding.mConvs={{10},{20}}; binding.mQueued={{10,"Synthetic queued line"},{20,"Other queued line"}};
    binding.sessionIDUpdated(10,40);
    CHECK(binding.mConvs[0].session==40 && binding.mConvs[1].session==20);
    CHECK(binding.mQueued[0].first==40 && binding.mQueued[0].second=="Synthetic queued line" && binding.mQueued[1].first==20);
    CHECK(binding.mForce && binding.mMessagesPending && binding.mRevision==~uint64_t(0));
    binding.mForce=binding.mMessagesPending=false;
    binding.sessionIDUpdated(99,50);
    CHECK(!binding.mForce && !binding.mMessagesPending && binding.mQueued.size()==2);
    return 0;
}
""".replace("// FUNCTIONS", "\n".join(functions)).replace("// SESSION_REKEY", rekey)
    src, exe = os.path.join(directory, "navigation.cpp"), os.path.join(directory, "navigation")
    with open(src, "w", encoding="utf-8") as stream:
        stream.write(fixture)
    built = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", src, "-o", exe], capture_output=True, text=True)
    if built.returncode:
        return built.stdout + built.stderr
    ran = subprocess.run([exe], capture_output=True, text=True)
    return ran.stdout + ran.stderr if ran.returncode else ""


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    tested = False
    if not cxx:
        result.warn("no C++ compiler: window layout tests not run")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_newui_window_layout")
            c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                                ctx.path(TEST), "-o", exe], capture_output=True, text=True)
            if c.returncode:
                result.fail("compile failed:\n" + c.stdout + c.stderr)
                return
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode:
                result.fail(r.stdout + r.stderr)
                return
            tested = True
    if cxx:
        with tempfile.TemporaryDirectory() as tmp:
            failure = navigation_fixture(ctx, cxx, tmp)
        if failure:
            result.fail("actual navigation handler regression:\n" + failure)
            return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = ("geometry + actual navigation/session tests pass; " if tested else "") + "kinds, documents, settings and wiring consistent"
