#!/usr/bin/env python3
"""Run offline ASTRA replays on reserved garbage; never autologin.

The host's slctl.py verifies settings through an isolated TESTCTL token. valrun.sh
is a live-login test and is deliberately not represented as a replay validator.
Caller restores settings/dialogs/symlink backups and releases BOXLOCK afterwards.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

TOKEN = 'astra-perf-architecture-ae1f063eb7-20261118'


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def running():
    comm = subprocess.check_output(['ps', '-eo', 'comm='], text=True).lower()
    return any(s in comm for s in ('slipstream', 'do-not-directly', 'alchemy'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('list', type=Path)
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    home = Path.home()
    root = home / 'slipstream/astra'
    lock = home / 'slipstream/BOXLOCK.txt'
    if lock.read_text().strip() != TOKEN or running():
        raise SystemExit('reservation not owned or viewer already running')
    iso = root / 'home'
    us = iso / '.slipstreamviewer/user_settings'
    us.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['SLIPSTREAM_HOME'] = str(iso)
    env['SLCTL_TOKEN_FILE'] = str(us / 'testctl.token')
    env['XDG_RUNTIME_DIR'] = '/run/user/' + str(os.getuid())
    sockets = sorted(Path(env['XDG_RUNTIME_DIR']).glob('wayland-[0-9]'))
    if sockets:
        env['WAYLAND_DISPLAY'] = sockets[-1].name
    env.setdefault('DISPLAY', ':0')
    env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + env['XDG_RUNTIME_DIR'] + '/bus'
    template = root / 'bak/settings.xml'
    log = iso / '.slipstreamviewer/logs/Slipstream.log'

    def note(msg):
        with (args.out / 'progress.txt').open('a') as f:
            f.write(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()) + ' ' + msg + '\n')

    try:
        for run in json.loads(args.list.read_text()):
            name = run['name']
            if (root / 'STOP').exists() or lock.read_text().strip() != TOKEN or running():
                raise RuntimeError('STOP, reservation changed, or unexpected viewer')
            report = args.out / (name + '.json')
            if report.exists():
                raise RuntimeError('output exists; choose fresh directory')
            shutil.copy2(template, us / 'settings.xml')
            log.unlink(missing_ok=True)
            pkg = Path(run['package'])
            exe = pkg / 'bin/do-not-directly-run-slipstream-bin'
            identity = {'source': run['source'], 'executable_sha256': sha(exe),
                        'capture_manifest_sha256': sha(Path(run['capture']) / 'manifest.json'),
                        'path_sha256': sha(Path(run['path'])), 'settings': run['settings'],
                        'settings_template_sha256': sha(template)}
            (args.out / (name + '.identity.json')).write_text(json.dumps(identity, indent=2))
            argv = [str(pkg / 'slipstream'), '--bench', run['capture'], '--bench-path', run['path'],
                    '--bench-loops', '2', '--bench-report', str(report)]
            for key, value in sorted(run['settings'].items()):
                argv += ['--set', key, str(value)]
            note('launch ' + name)
            with (args.out / (name + '.stdout')).open('w') as stdout:
                proc = subprocess.Popen(argv, cwd=pkg, env=env, stdout=stdout, stderr=subprocess.STDOUT)
                ctl_done = False
                deadline = time.monotonic() + 1200
                while proc.poll() is None:
                    if time.monotonic() > deadline:
                        proc.terminate()
                        try:
                            proc.wait(15)
                        except subprocess.TimeoutExpired:
                            proc.kill(); proc.wait()
                        raise RuntimeError('owned replay timed out')
                    if not ctl_done and (us / 'testctl.token').exists():
                        try:
                            result = subprocess.run([str(home / 'bin/slctl.py'), 'get', 'RenderVKDeferReflectHalfRes'],
                                env=env, capture_output=True, text=True, timeout=3)
                            value = json.loads(result.stdout)
                            if result.returncode == 0 and value.get('ok'):
                                (args.out / (name + '.ctl.json')).write_text(json.dumps(value))
                                ctl_done = True
                        except (subprocess.TimeoutExpired, ValueError):
                            pass
                    time.sleep(1)
            if log.exists():
                shutil.copy2(log, args.out / (name + '.log'))
            note('exit %s code=%s ctl=%s' % (name, proc.returncode, ctl_done))
            if proc.returncode or not report.exists() or not json.loads(report.read_text()).get('ok'):
                raise RuntimeError('failed or incomplete replay ' + name)
        (args.out / 'DONE').write_text('complete\n')
    except Exception as exc:
        note('BLOCKED ' + str(exc))
        (args.out / 'BLOCKED').write_text(str(exc) + '\n')
        raise


if __name__ == '__main__':
    main()
