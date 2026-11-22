"""Parse scoped VKGpu sub lines without mixing pass, async, gap and worst values."""
import re

_PAIR = re.compile(r"(?P<name>[A-Za-z0-9_]+)=(?P<value>[-+0-9.eE]+)")


def parse_sub_line(line):
    if "GPU sub ms/frame:" not in line:
        return None
    sections = {"graphics": {}, "async": {}, "gaps": {}, "worst": {}}
    active = "graphics"
    for index, part in enumerate(line.split("GPU sub ms/frame:", 1)[1].split("|")):
        if index:
            tag, separator, part = part.partition(":")
            if not separator:
                continue
            active = {"async queue": "async", "gaps": "gaps", "worst": "worst"}.get(tag.strip())
            if active is None:
                continue
        for pair in _PAIR.finditer(part):
            sections[active][pair["name"]] = float(pair["value"])
    return sections
