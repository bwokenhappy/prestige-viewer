#!/usr/bin/env python3
"""#1454 source checks: the model fee request waits only for physics hull jobs that run.

LLMeshUploadThread::generateHulls waits until mFinalDecomp completes before the fee request
is sent. A part whose physics triangles are all filtered out gets a DecompRequest that is never
submitted; if that request were the final one, the wait (and the fee) would never end. So only a
submitted request may become mFinalDecomp, and the model then goes to the server as provided,
which reports any missing physics block.

    python3 indra/newview/tests/mesh_fee_hull_wait_checks.py [--source-root .]
"""
import argparse
import pathlib
import re
import sys


def body(src, signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    raise AssertionError("unbalanced " + signature)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", default=".")
    root = pathlib.Path(ap.parse_args().source_root)
    cpp = (root / "indra/newview/llmeshrepository.cpp").read_text()
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    ctor = body(cpp, "LLMeshUploadThread::DecompRequest::DecompRequest(")
    check("mFinalDecomp" not in ctor, "the DecompRequest constructor does not make itself the final request")
    check("mPhysicsComplete" not in ctor, "the DecompRequest constructor does not reset mPhysicsComplete")

    gh = body(cpp, "void LLMeshUploadThread::generateHulls(")
    valid = gh.find("isValid()")
    final = gh.find("mFinalDecomp")
    submit = gh.find("submitRequest(")
    check(valid >= 0 and final > valid, "generateHulls sets mFinalDecomp only after the isValid() check")
    check(final >= 0 and submit > final, "mFinalDecomp is set before the request is submitted")
    check(re.search(r"mPhysicsComplete\s*=\s*false", gh[valid:submit] if valid >= 0 and submit > valid else ""),
          "mPhysicsComplete is reset for each submitted request, inside the isValid() block")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("mesh_fee_hull_wait_checks: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
