#!/usr/bin/env python3
"""Run one owned build at four jobs and absolute nice19; never adjust foreign processes."""
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: build_owned.py LOG COMMAND [ARGS...]')
    log, *command = sys.argv[1:]
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    env = dict(os.environ, TMPDIR='/home/bryan/.cache/slipstream-build-tmp',
               CMAKE_BUILD_PARALLEL_LEVEL='4', NINJAFLAGS='-j4', NINJA_JOBS='4',
               PYTHON_CPU_COUNT='4', PYTHONUNBUFFERED='1')
    Path(log).parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w') as stream:
        while True:
            pids = subprocess.run(['pgrep', '-x', 'ninja'], text=True, capture_output=True).stdout.split()
            if len(pids) <= 2:
                break
            stream.write('waiting: %d existing Ninja processes\n' % len(pids))
            stream.flush()
            time.sleep(3)
        stream.write('existing_ninja=%s command=%r effective_jobs=4 absolute_nice=19\n' % (pids, command))
        stream.flush()
        proc = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
        corrected = 0
        while proc.poll() is None:
            rows = [r.split() for r in subprocess.check_output(['ps', '-eo', 'pid=,ppid=,ni=,comm='], text=True).splitlines()]
            owned = {proc.pid}
            for _ in range(16):
                owned.update(int(r[0]) for r in rows if int(r[1]) in owned)
            for pid, parent, priority, comm in rows:
                if int(pid) not in owned or priority == '-':
                    continue
                if int(priority) != 19:
                    try:
                        os.setpriority(os.PRIO_PROCESS, int(pid), 19)
                        corrected += 1
                    except ProcessLookupError:
                        pass
            time.sleep(.3)
        stream.write('exit=%d priority_resets_corrected=%d\n' % (proc.returncode, corrected))
    return proc.returncode


if __name__ == '__main__':
    sys.exit(main())
