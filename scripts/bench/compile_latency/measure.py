#!/usr/bin/env python3
"""Single-run wall time and child peak RSS without requiring GNU time on the host."""
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import time

started = time.monotonic()
family_peak = None
if os.environ.get('ASTRA_MEASURE_PROCESS_TREE'):
    child = subprocess.Popen(sys.argv[2:])
    family_peak = 0
    while child.poll() is None:
        pending, seen, rss = [child.pid], set(), 0
        while pending:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
            try:
                base = Path('/proc') / str(pid)
                for line in (base / 'status').read_text().splitlines():
                    if line.startswith('VmRSS:'):
                        rss += int(line.split()[1])
                # Children can belong to any thread (the viewer's worker host).
                for task in (base / 'task').iterdir():
                    pending.extend(map(int, (task / 'children').read_text().split()))
            except (FileNotFoundError, ProcessLookupError):
                pass
        family_peak = max(family_peak, rss)
        time.sleep(.25)
    code = child.returncode
else:
    code = subprocess.call(sys.argv[2:])
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
Path(sys.argv[1]).write_text(json.dumps({
    'exit_code': code, 'wall_s': time.monotonic() - started,
    'peak_rss_kib': usage.ru_maxrss, 'user_s': usage.ru_utime,
    'system_s': usage.ru_stime, 'major_faults': usage.ru_majflt,
    'sampled_process_tree_peak_rss_kib': family_peak,
}, indent=2) + '\n')
sys.exit(code)
