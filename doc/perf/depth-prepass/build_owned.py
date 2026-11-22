#!/usr/bin/env python3
"""Serialize an owned build and maintain absolute nice 19 through recursive shells."""
import os
from pathlib import Path
import subprocess
import sys
import time

log, *command = sys.argv[1:]
os.setpriority(os.PRIO_PROCESS, 0, 19)
while subprocess.run(['pgrep', '-x', 'ninja'], stdout=subprocess.DEVNULL).returncode == 0:
    time.sleep(3)
env = os.environ.copy()
env.update(TMPDIR='/home/bryan/.cache/slipstream-build-tmp',
           CMAKE_BUILD_PARALLEL_LEVEL='12', NINJA_JOBS='12', PYTHON_CPU_COUNT='12')
with open(log, 'w') as f:
    proc = subprocess.Popen(['nice', '-n', '19', *command], env=env, stdout=f, stderr=subprocess.STDOUT)
    corrected = 0
    while proc.poll() is None:
        rows = [r.split() for r in subprocess.check_output(['ps', '-eo', 'pid=,ppid=,ni='], text=True).splitlines()]
        owned = {proc.pid}
        for _ in range(16):
            owned.update(int(r[0]) for r in rows if int(r[1]) in owned)
        for pid, parent, ni in rows:
            if int(pid) in owned and ni != '-' and int(ni) != 19:
                try:
                    os.setpriority(os.PRIO_PROCESS, int(pid), 19)
                    corrected += 1
                except ProcessLookupError:
                    pass
        time.sleep(.2)
print('build exit', proc.returncode, 'priority resets corrected', corrected, flush=True)
sys.exit(proc.returncode)
