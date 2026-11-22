"""A locally configured NVIDIA application ID never reaches the repository (#1517).

Driver pacing for DLSS Frame Generation can use an application ID NVIDIA issues; one configured
for local tests (the add-on read nvidia_app_id.txt in its DLSS folder before 1.4.0) must stay out of the source
tree and out of every commit. The local IDs: the first line of every *.txt file (any case) directly
in $SLIPSTREAM_DLSS_DIR when it is set, else in every /mnt/c/Users/*/AppData/Roaming/SlipstreamViewer/dlss/
and ~/.slipstreamviewer/dlss/, when that line is a plain decimal number of at least 5 digits (UTF-8,
with or without a BOM, or UTF-16 with its BOM).

scan(number, repo_root) lists where a number appears:
  file <path>         a tracked file's working tree contents (git grep)
  staged <path>       the index (git grep --cached): what the next commit takes
  tip HEAD <path>     HEAD's tree (git grep HEAD)
  tip <ref> <path>    every other branch / remote-tracking tip: its files that differ from HEAD's
                      (git diff-tree against HEAD, then those blobs read through git cat-file)
  commit <hash>       a commit since HISTORY_SINCE whose diff adds or removes it (git log --all -S),
                      or whose message holds it (git log --all --grep -F)
A number only in older history that no tip, the index or the working tree holds any more is not
found (the self-test asserts that too). Never the number in a hit: a path or ref name holding it is
masked. A self-test runs first, every time, on a temporary repository. The number is never printed:
not in a failure, a warning, the summary or an exception's text (git's argv carries it; nothing of a
failed call is shown).
"""
import concurrent.futures
import glob
import os
import re
import subprocess
import tempfile

NAME = "framegen-local-id"
STAGE = "source"
DESCRIPTION = "a locally configured NVIDIA application ID is in no tracked file, branch tip, commit or commit message"
GUARDS = ("A local NVIDIA application ID (configured for the user's own frame pacing tests) committed "
          "in a file, staged, on a branch tip, in a commit's diff or in a commit message.")

# The history searches (git log -S / --grep, by committer date) start here: the local ID first
# reached this machine on 2026-11-09, so no older commit can hold it, and a rebase or cherry-pick
# gives a commit a new committer date. Older history in full takes minutes (-S reads every diff);
# whatever older history a tip still holds, the tip searches find.
HISTORY_SINCE = "2026-11-01"

_ID = re.compile(r"[0-9]{5,64}")   # (a longer line is not an ID, nor cut down to one)
_HASH = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
_GIT_TIMEOUT = 300   # seconds
_MASK = "<ID>"
_READERS = 8   # git cat-file processes reading the tips' blobs side by side


class ScanError(Exception):
    """A git call failed. Its text is fixed: never the argv, never git's output."""


def _folders():
    env = os.environ.get("SLIPSTREAM_DLSS_DIR")
    if env:
        return [env]
    return sorted(glob.glob("/mnt/c/Users/*/AppData/Roaming/SlipstreamViewer/dlss/")) + \
        [os.path.expanduser("~/.slipstreamviewer/dlss/")]


def _first_line(path):
    """The file's first line (of its first 260 bytes), BOM and surrounding whitespace removed."""
    with open(path, "rb") as f:
        data = f.read(260)
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = data.decode("utf-16", errors="replace")
    else:
        text = data.decode("utf-8-sig", errors="replace")
    return text.splitlines()[0].strip() if text else ""


def local_ids():
    """(every local ID found, each once, in folder / file order; how many files could not be read)."""
    ids, unreadable = [], 0
    for folder in _folders():
        if not os.path.isdir(folder):
            continue
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            unreadable += 1
            continue
        for name in names:
            path = os.path.join(folder, name)
            if not name.lower().endswith(".txt") or not os.path.isfile(path):
                continue
            try:
                line = _first_line(path)
            except Exception:   # (an odd file never stops the check)
                unreadable += 1
                continue
            if _ID.fullmatch(line) and line not in ids:
                ids.append(line)
    return ids, unreadable


def _env():
    # (no GIT_DIR / GIT_INDEX_FILE / ... from a hook: -C names the repository)
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def _git(repo, args, ok=(0,), env=None, data=None, raw=False):
    """git's stdout (text, or bytes with raw). Any failure raises ScanError with a fixed text (the argv
    may hold the number). env: variables added to the cleaned environment (the self-test's commit
    dates); data: bytes for git's stdin."""
    try:
        p = subprocess.run(["git", "-C", repo] + args, input=data,
                           stdin=None if data is not None else subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, env=dict(_env(), **(env or {})), timeout=_GIT_TIMEOUT)
    except (OSError, subprocess.SubprocessError):
        raise ScanError("git could not run") from None
    if p.returncode not in ok:
        raise ScanError("git %s failed (exit %d)" % (args[0], p.returncode))
    return p.stdout if raw else p.stdout.decode("utf-8", errors="replace")


def _grep(repo, number, where):
    """Paths git grep finds the number in; where: [] (working tree), ["--cached"] or ["HEAD"]."""
    out = _git(repo, ["grep", "-l", "-z", "-F", "-e", number] + where + ["--"], ok=(0, 1))
    paths = [p for p in out.split("\0") if p]
    if where == ["HEAD"]:
        paths = [p[len("HEAD:"):] if p.startswith("HEAD:") else p for p in paths]
    return paths


def _other_tips(repo, number, head):
    """["<ref> <path>"]: every branch / remote-tracking tip's files that differ from HEAD's and hold the
    number. One git diff-tree for all tips, one git cat-file for the blobs they name."""
    out = _git(repo, ["for-each-ref", "--format=%(objectname) %(objecttype) %(refname:short)",
                      "refs/heads", "refs/remotes"])
    tips = {}
    for line in out.splitlines():
        h, kind, name = (line.split(" ", 2) + ["", ""])[:3]
        if _HASH.fullmatch(h) and kind == "commit" and h != head and h not in tips:
            tips[h] = name
    if not tips:
        return []
    # "<tip> <HEAD>": as a commit and its parent -- the diff goes from HEAD to the tip (the tip's blobs
    # are the new side; D: only in HEAD), the tip's id heads its records.
    stdin = "".join("%s %s\n" % (t, head) for t in tips).encode()
    out = _git(repo, ["diff-tree", "--stdin", "-r", "-z", "--no-renames"], data=stdin, raw=True)
    tokens = out.split(b"\0")
    blobs = {}   # blob -> (tip, path): its first place
    tip, i = None, 0
    while i < len(tokens):
        tok = tokens[i].decode("utf-8", errors="replace")
        if tok.startswith(":") and i + 1 < len(tokens):
            meta = tok[1:].split()
            path = tokens[i + 1].decode("utf-8", errors="replace")
            i += 2
            if tip and len(meta) >= 5 and meta[4] != "D" and meta[1].startswith(("100", "120")) \
                    and meta[3] not in blobs:
                blobs[meta[3]] = (tip, path)
            continue
        if _HASH.fullmatch(tok):
            tip = tok
        i += 1
    if not blobs:
        return []
    # (the tips' blobs can be gigabytes, a few large binaries most of it: read in parallel, one blob at a
    # time, the work shared out by size)
    sizes = {}
    for line in _git(repo, ["cat-file", "--batch-check=%(objectname) %(objectsize)"],
                     data="".join(b + "\n" for b in blobs).encode()).splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            sizes[parts[0]] = int(parts[1])
    buckets = [[0, []] for _ in range(_READERS)]
    for b in sorted(sizes, key=sizes.get, reverse=True):
        least = min(buckets, key=lambda x: x[0])
        least[0] += sizes[b]
        least[1].append(b)
    needle = number.encode()
    with concurrent.futures.ThreadPoolExecutor(max_workers=_READERS) as pool:
        found = [f.result() for f in [pool.submit(_blobs_holding, repo, ids, needle) for _, ids in buckets if ids]]
    return ["%s %s" % (tips[blobs[b][0]], blobs[b][1]) for part in found for b in part]


def _blobs_holding(repo, ids, needle):
    """The blobs among ids whose contents hold needle (git cat-file --batch, read one blob at a time)."""
    try:
        p = subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=_env())
    except (OSError, subprocess.SubprocessError):
        raise ScanError("git could not run") from None
    def feed():
        try:
            p.stdin.write("".join(b + "\n" for b in ids).encode())
            p.stdin.close()
        except OSError:
            pass
    writer = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    writer.submit(feed)
    holding = []
    try:
        for _ in ids:
            header = p.stdout.readline().split()
            if len(header) == 2:   # ("<blob> missing")
                continue
            if len(header) != 3 or not header[2].isdigit():
                raise ScanError("git cat-file printed something unexpected")
            size = int(header[2])
            if needle in p.stdout.read(size):
                holding.append(header[0].decode())
            p.stdout.read(1)
    except ScanError:
        raise
    except Exception:
        raise ScanError("git cat-file could not be read") from None
    finally:
        if p.poll() is None:
            p.kill()   # (a failed read: the writer's next write fails and it stops)
        p.stdout.close()
        p.wait()
        writer.shutdown(wait=True)
    return holding


def _history(repo, number):
    """Commits since HISTORY_SINCE whose diff adds or removes the number, then those whose message holds it."""
    commits = []
    for args in (["log", "--all", "--no-show-signature", "--since=" + HISTORY_SINCE, "--format=%H", "-S" + number],
                 ["log", "--all", "--no-show-signature", "--since=" + HISTORY_SINCE, "--format=%H", "-F",
                  "--grep=" + number]):
        for h in _git(repo, args).split():
            if not _HASH.fullmatch(h):
                raise ScanError("git log printed something other than a commit")
            if h not in commits:
                commits.append(h)
    return commits


def scan(number, repo_root):
    """Where number appears in repo_root: ["file <path>", "staged <path>", "tip <ref> <path>",
    "commit <hash>", ...] (empty: nowhere). The searches run side by side."""
    head = _git(repo_root, ["rev-parse", "--verify", "-q", "HEAD"], ok=(0, 1)).strip()
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        work = pool.submit(_grep, repo_root, number, [])
        staged = pool.submit(_grep, repo_root, number, ["--cached"])
        at_head = pool.submit(_grep, repo_root, number, ["HEAD"]) if _HASH.fullmatch(head) else None
        tips = pool.submit(_other_tips, repo_root, number, head) if _HASH.fullmatch(head) else None
        history = pool.submit(_history, repo_root, number)
        hits = ["file " + p for p in work.result()]
        hits += ["staged " + p for p in staged.result()]
        hits += ["tip HEAD " + p for p in (at_head.result() if at_head else [])]
        hits += ["tip " + t for t in (tips.result() if tips else [])]
        hits += ["commit " + h for h in history.result()]
    return [h.replace(number, _MASK) for h in hits]   # (a path or ref name may hold it)


def _self_test():
    """None when scan works on a temporary repository, else what went wrong (no number of the real scan).
    Each test number sits where exactly one of scan's searches must find it; one sits only in old,
    deleted history (outside HISTORY_SINCE and on no tip) and is, by design, not found."""
    old = {"GIT_AUTHOR_DATE": "2020-01-01T12:00:00 +0000", "GIT_COMMITTER_DATE": "2020-01-01T12:00:00 +0000"}
    with tempfile.TemporaryDirectory(prefix="framegen-local-id-") as tmp:
        repo = os.path.join(tmp, "repo")
        hooks = os.path.join(tmp, "hooks")   # (empty: no hook of the user's runs here)
        os.mkdir(repo)
        os.mkdir(hooks)
        cfg = ["-c", "user.name=check", "-c", "user.email=check@example.invalid", "-c", "commit.gpgsign=false",
               "-c", "core.hooksPath=" + hooks]

        def write(name, text):
            with open(os.path.join(repo, name), "w", encoding="utf-8") as f:
                f.write(text)

        def commit(message, env=None):
            _git(repo, cfg + ["commit", "-q", "-m", message], env=env)
            return _git(repo, ["rev-parse", "HEAD"]).strip()

        _git(repo, ["init", "-q"])
        _git(repo, ["symbolic-ref", "HEAD", "refs/heads/main"])
        # Old history (before HISTORY_SINCE): only what a tip still holds can be found.
        write("keep.txt", "id 987654321\n")
        _git(repo, ["add", "keep.txt"])
        commit("keep", old)
        write("gone.txt", "222333444\n")
        _git(repo, ["add", "gone.txt"])
        commit("add gone", old)
        _git(repo, ["rm", "-q", "gone.txt"])
        commit("remove gone", old)
        _git(repo, ["checkout", "-q", "-b", "side"])
        write("side.txt", "444555666\n")
        _git(repo, ["add", "side.txt"])
        commit("side", old)
        _git(repo, ["checkout", "-q", "main"])
        # Recent history: the commits that add or remove a number, and a message.
        write("recent.txt", "777888999\n")
        _git(repo, ["add", "recent.txt"])
        added = commit("add recent")
        _git(repo, ["rm", "-q", "recent.txt"])
        removed = commit("remove recent")
        write("other.txt", "nothing\n")
        _git(repo, ["add", "other.txt"])
        message = commit("a message with 555666777")
        # Not committed: edited out of HEAD and staged; staged then edited out; tracked, changed, unstaged.
        write("keep.txt", "id removed\n")
        _git(repo, ["add", "keep.txt"])
        write("staged.txt", "111222333\n")
        _git(repo, ["add", "staged.txt"])
        write("staged.txt", "nothing\n")
        write("other.txt", "666777888\n")
        cases = (("987654321", ["tip HEAD keep.txt"]),
                 ("444555666", ["tip side side.txt"]),
                 ("222333444", []),
                 ("777888999", ["commit " + added, "commit " + removed]),
                 ("555666777", ["commit " + message]),
                 ("111222333", ["staged staged.txt"]),
                 ("666777888", ["file other.txt"]),
                 ("123456789", []))
        for number, want in cases:
            got = scan(number, repo)
            if sorted(got) != sorted(want):
                return "scan of a test number found %d hit(s), expected %d" % (len(got), len(want))
            if any(number in h for h in got):
                return "a hit carries the number it was searching for"
    return None


def run(ctx, result):
    try:
        problem = _self_test()
    except Exception:   # (fixed text only: nothing of the exception is shown)
        problem = "the temporary repository could not be built or scanned"
    if problem:
        result.fail("self-test: " + problem)
        result.summary = "self-test failed"
        return
    try:
        ids, unreadable = local_ids()
    except Exception:   # (fixed text only)
        ids, unreadable = [], 1
    if unreadable:
        result.warn("%d file(s) or folder(s) in the DLSS folders could not be read (skipped)" % unreadable)
    if not ids:
        result.warn("not run (no local application ID)")
        result.summary = "not run"
        return
    found = 0
    for number in ids:
        try:
            hits = scan(number, ctx.path("."))
        except Exception:   # (fixed text only: an exception's text may hold git's argv)
            result.fail("a local application ID could not be checked (git failed)")
            continue
        if hits:
            found += 1
            result.fail("a local application ID appears in the repository: " + ", ".join(hits))
    result.summary = "%d local application ID(s) found in the repository" % found if found else \
        "clean (%d local application ID(s))" % len(ids)
