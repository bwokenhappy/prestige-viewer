#!/usr/bin/env python3
"""Run offline SOL depth-prepass replays on reserved garbage; never autologin.

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

TOKEN = 'sol-depth-prepass-ae1f063eb7-20261119'


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
    root = home / 'slipstream/sol-depth-prepass-ae1f063eb7'
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
    template = root / 'backup/settings.xml'
    log = iso / '.slipstreamviewer/logs/Slipstream.log'

    def note(msg):
        with (args.out / 'progress.txt').open('a') as f:
            f.write(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()) + ' ' + msg + '\n')

    proc = None
    pkg = None
    try:
        for run in json.loads(args.list.read_text()):
            name = run['name']
            if (root / 'STOP').exists() or lock.read_text().strip() != TOKEN or running():
                raise RuntimeError('STOP, reservation changed, or unexpected viewer')
            pkg = Path(run['package'])
            ready_deadline = time.monotonic() + 7200
            while not (pkg / 'READY').exists():
                if lock.read_text().strip() != TOKEN or running() or (root / 'STOP').exists():
                    raise RuntimeError('reservation/viewer/STOP changed while waiting for package')
                if time.monotonic() > ready_deadline:
                    raise RuntimeError('package not ready within 2h')
                time.sleep(3)
            run['source'] = (pkg / 'source.txt').read_text().strip()
            report = args.out / (name + '.json')
            if report.exists():
                raise RuntimeError('output exists; choose fresh directory')
            shutil.copy2(template, us / 'settings.xml')
            log.unlink(missing_ok=True)
            exe = pkg / 'bin/do-not-directly-run-slipstream-bin'
            identity = {'source': run['source'], 'executable_sha256': sha(exe),
                        'capture_manifest_sha256': sha(Path(run['capture']) / 'manifest.json'),
                        'path_sha256': sha(Path(run['path'])), 'settings': run['settings'],
                        'settings_template_sha256': sha(template)}
            (args.out / (name + '.identity.json')).write_text(json.dumps(identity, indent=2))
            argv = [str(pkg / 'slipstream'), '--bench', run['capture'], '--bench-path', run['path'],
                    '--bench-loops', '1', '--bench-report', str(report)]
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
                            ready = subprocess.run([str(home / 'bin/slctl.py'), 'raw', json.dumps({'cmd':'bench','op':'status'})],
                                env=env, capture_output=True, text=True, timeout=3)
                            state = json.loads(ready.stdout)
                            if not state.get('ok') or state.get('run',{}).get('state') not in ('warmup','running','still'):
                                raise ValueError('replay not ready for settings receipt')
                            applied = {}
                            for key in ('RenderVKOcclusion2Phase','RenderVKBlendOpaqueGBuffer','RenderVKUpscaler',
                                        'RenderVKDepthOnlyVS','RenderVKPrepassDepthOnlyDeferred','RenderVKRayTracing',
                                        'RenderVKGI','RenderVKFrameGen','AutoTuneLock'):
                                result = subprocess.run([str(home / 'bin/slctl.py'), 'get', key],
                                    env=env, capture_output=True, text=True, timeout=3)
                                value = json.loads(result.stdout)
                                if result.returncode or not value.get('ok'):
                                    raise ValueError('control not ready')
                                applied[key] = value['value']
                            (args.out / (name + '.ctl.json')).write_text(json.dumps(applied))
                            # Some enum controls intentionally store numeric strings.
                            def number(value):
                                try:
                                    return float(value)
                                except (TypeError, ValueError):
                                    return value
                            mismatches = {k: [run['settings'][k], applied[k]] for k in applied
                                          if number(applied[k]) != number(run['settings'][k])}
                            if mismatches:
                                raise RuntimeError('actual setting differs from request: ' + json.dumps(mismatches))
                            ctl_done = True
                        except (subprocess.TimeoutExpired, ValueError):
                            pass
                    time.sleep(1)
            if log.exists():
                shutil.copy2(log, args.out / (name + '.log'))
            note('exit %s code=%s ctl=%s' % (name, proc.returncode, ctl_done))
            if not ctl_done:
                raise RuntimeError('actual settings receipt absent ' + name)
            if proc.returncode or not report.exists() or not json.loads(report.read_text()).get('ok'):
                raise RuntimeError('failed or incomplete replay ' + name)
        (args.out / 'DONE').write_text('complete\n')
    except Exception as exc:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        # Clean up only an executable in this task's own package, never a foreign viewer.
        if pkg is not None:
            import signal
            owned_exe = (pkg / 'bin/do-not-directly-run-slipstream-bin').resolve()
            for entry in Path('/proc').iterdir():
                if not entry.name.isdecimal():
                    continue
                try:
                    if (entry / 'exe').resolve() == owned_exe:
                        os.kill(int(entry.name), signal.SIGTERM)
                except (PermissionError, ProcessLookupError, FileNotFoundError):
                    pass
        note('BLOCKED ' + str(exc))
        (args.out / 'BLOCKED').write_text(str(exc) + '\n')
        raise


if __name__ == '__main__':
    main()
