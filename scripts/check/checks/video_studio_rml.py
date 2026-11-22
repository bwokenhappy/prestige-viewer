"""Regression guard for Video Studio's overlay document (indra/newview/slvideostudiodoc.h, kStudioRml, bound in slvideostudioview.cpp, #1668).

The same rules as camera mode's overlay (camera_rml.py): RmlUi tolerates unbalanced markup silently, and a stray
closing tag shows raw {{bindings}}. Every tag closed in order, everything inside the one data-model element, every
{{variable}} and data-* expression naming a bound variable, every event callback bound.
"""
from checks import camera_rml

NAME = "video-studio-rml"
STAGE = "source"
DESCRIPTION = "Video Studio's overlay document: balanced tags, one data model around everything, bound variables"
GUARDS = "Raw {{bindings}} or a dead button in Video Studio's panel (a stray closing tag, an unbound name)."

SOURCE = "indra/newview/slvideostudioview.cpp"
DOC = "indra/newview/slvideostudiodoc.h"


def run(ctx, result):
    problems = camera_rml.document_problems(ctx, SOURCE, "kStudioRml", DOC)
    for p in problems:
        result.fail(p)
    if not problems:
        result.summary = "balanced, one data model, every variable and event callback bound"
