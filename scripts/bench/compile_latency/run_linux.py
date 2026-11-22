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
import signal
import subprocess
import sys
import time

TOKEN = 'astra-shader-compile-ae1f063eb7-20261119'


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def running():
    comm = subprocess.check_output(['ps', '-eo', 'comm='], text=True).lower()
    return any(s in comm for s in ('slipstream', 'do-not-directly', 'alchemy'))


def cache_manifest(cache):
    return {p.name: {'bytes': p.stat().st_size, 'sha256': sha(p)}
            for p in sorted(cache.glob('vk_*')) if p.is_file()}


def driver_cache_fds(package):
    """Only the owned executable; never inspect unrelated processes' descriptors."""
    paths = set()
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if not str((proc / 'exe').readlink()).startswith(str(package) + '/'):
                continue
            for fd in (proc / 'fd').iterdir():
                try:
                    path = str(fd.readlink())
                    if '/GLCache/' in path or '/driver-cache/' in path:
                        paths.add(path)
                except OSError:
                    pass
        except OSError:
            pass
    return sorted(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('list', type=Path)
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    home = Path.home()
    root = home / 'slipstream/astra-shader-compile'
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
    env['__GL_SHADER_DISK_CACHE'] = '0'
    env['XDG_CACHE_HOME'] = str(iso / '.cache')
    env['__GL_SHADER_DISK_CACHE_PATH'] = str(iso / 'driver-cache')
    (iso / 'driver-cache').mkdir(parents=True, exist_ok=True)
    env['MESA_SHADER_CACHE_DISABLE'] = 'true'
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
            cache = iso / '.slipstreamviewer/cache'
            if run.get('cold') or run.get('seed_directory'):
                if cache.exists():
                    cache.rename(args.out / (name + '.cache-before'))
            if run.get('seed_directory'):
                seed = (root / run['seed_directory']).resolve()
                if not seed.is_relative_to(root):
                    raise RuntimeError('seed must be inside the owned directory')
                shutil.copytree(seed, cache)
            if run.get('seed_main'):
                seed = (root / run['seed_main']).resolve()
                if not seed.is_relative_to(root):
                    raise RuntimeError('seed must be inside the owned directory')
                shutil.copyfile(seed, cache / 'vk_pipeline_cache.bin')
            (args.out / (name + '.cache-start.json')).write_text(json.dumps(
                cache_manifest(iso / '.slipstreamviewer/cache'), indent=2))
            shutil.copy2(template, us / 'settings.xml')
            log.unlink(missing_ok=True)
            pkg = Path(run['package'])
            exe = pkg / 'bin/do-not-directly-run-slipstream-bin'
            identity = {'source': run['source'], 'executable_sha256': sha(exe),
                        'capture_manifest_sha256': sha(Path(run['capture']) / 'manifest.json'),
                        'path_sha256': sha(Path(run['path'])), 'settings': run['settings'],
                        'settings_template_sha256': sha(template), 'app_cache_start': 'empty' if run.get('cold') else 'retained', 'driver_disk_cache_requested': False}
            run_env = env.copy()
            if run.get('seed_main'):
                identity['app_cache_start'] = 'retained partial seed'
            elif run.get('seed_directory'):
                identity['app_cache_start'] = 'retained seed directory'
            run_env['__GL_SHADER_DISK_CACHE'] = '1' if run.get('driver_cache') else '0'
            identity['driver_disk_cache_requested'] = run.get('driver_cache', False)
            if run.get('force_oversize'):
                shim = root / 'force_oversize.so'
                run_env['LD_PRELOAD'] = str(shim)
                run_env['ASTRA_FORCE_CACHE_OVERSIZE'] = '1'
                identity['synthetic_oversize_shim_sha256'] = sha(shim)
            if run.get('worker'):
                run_env['SL_VK_PIPELINE_WORKER'] = '1'
                run_env['ASTRA_MEASURE_PROCESS_TREE'] = '1'
            identity['worker_requested'] = run.get('worker', False)
            (args.out / (name + '.identity.json')).write_text(json.dumps(identity, indent=2))
            argv = [str(pkg / 'slipstream'), '--bench', run['capture'], '--bench-path', run['path'],
                    '--bench-loops', '1', '--bench-report', str(report)]
            for key, value in sorted(run['settings'].items()):
                argv += ['--set', key, str(value)]
            note('launch ' + name)
            with (args.out / (name + '.stdout')).open('w') as stdout:
                proc = subprocess.Popen([sys.executable, str(root / 'measure.py'), str(args.out / (name + '.memory.json'))] + argv, cwd=pkg, env=run_env, stdout=stdout, stderr=subprocess.STDOUT, start_new_session=True)
                ctl_done = False
                fds_done = False
                deadline = time.monotonic() + 1200
                while proc.poll() is None:
                    if run.get('driver_cache') and not fds_done:
                        paths = driver_cache_fds(pkg)
                        if paths:
                            (args.out / (name + '.driver-fds.json')).write_text(json.dumps(
                                {'paths': paths, 'all_isolated': all(p.startswith(str(iso) + '/') for p in paths)}, indent=2))
                            fds_done = True
                    if time.monotonic() > deadline:
                        os.killpg(proc.pid, signal.SIGTERM)
                        try:
                            proc.wait(15)
                        except subprocess.TimeoutExpired:
                            os.killpg(proc.pid, signal.SIGKILL); proc.wait()
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
            (args.out / (name + '.cache-end.json')).write_text(json.dumps(
                cache_manifest(iso / '.slipstreamviewer/cache'), indent=2))
            (args.out / (name + '.driver-cache-end.json')).write_text(json.dumps(
                {str(p.relative_to(iso)): p.stat().st_size for folder in (iso / 'driver-cache', iso / '.cache/nvidia')
                 for p in folder.rglob('*') if p.is_file()}, indent=2))
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
