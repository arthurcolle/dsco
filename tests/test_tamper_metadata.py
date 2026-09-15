#!/usr/bin/env python3
"""Real macOS vnode tests for timestamp-only updates versus security metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ('st_mode', 'st_uid', 'st_gid', 'st_nlink', 'st_size', 'st_ino',
          'st_dev', 'st_flags', 'st_mtime_ns', 'st_ctime_ns', 'st_atime_ns')
HELPER = r'''
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include TAMPER_IMPLEMENTATION
static void wiped(void *unused) { (void)unused; (void)write(1,"wiped\n",6); }
int main(int argc,char **argv) {
    assert(argc==2); snprintf(g.exe_path,sizeof(g.exe_path),"%s",argv[1]);
    pthread_mutex_init(&g.wiper_lock,NULL); assert(setup_kqueue_watch());
    tamper_register_wiper(wiped,NULL); g.watch_running=true;
    assert(pthread_create(&g.watch_thread,NULL,watcher_thread,NULL)==0);
    pthread_detach(g.watch_thread); puts("ready"); fflush(stdout);
    for(;;) pause();
}
'''


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    a = p.parse_args()
    assert os.uname().sysname == 'Darwin'
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    helper = out / 'metadata-helper.c'; helper.write_text(HELPER)
    source = ROOT / 'src/tamper.c'; executable = out / 'metadata-helper'
    sodium = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libsodium'], text=True))
    command = ['cc', '-O2', '-g', '-std=c11', '-D_DARWIN_C_SOURCE', '-DHAVE_LIBSODIUM',
               f'-DTAMPER_IMPLEMENTATION="{source}"', '-I', str(ROOT / 'include'),
               str(helper), *sodium, '-lpthread', '-o', str(executable)]
    subprocess.run(command, check=True, timeout=30)
    env = dict(os.environ, DSCO_DEBUG='1', DSCO_TAMPER_DESTRUCT='0')

    def snapshot(fd):
        s = os.fstat(fd)
        return {field: getattr(s, field) for field in FIELDS}

    def monitor(path):
        fd = os.open(path, os.O_RDONLY)
        kq = select.kqueue()
        kq.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
            fflags=select.KQ_NOTE_ATTRIB | select.KQ_NOTE_WRITE | select.KQ_NOTE_DELETE |
                   select.KQ_NOTE_RENAME | select.KQ_NOTE_LINK)], 0)
        return fd, kq

    def cleanup(child):
        # Every test has its own process group, including any sleeping tool.
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL); child.wait()

    def benign(child, path, fd, kq):
        records = []
        for action in ['read_stat_hash', 'atime_only']:
            before = snapshot(fd)
            if action == 'read_stat_hash':
                for _ in range(5):
                    digest = hashlib.sha256(path.read_bytes()).hexdigest(); path.stat()
            else:
                os.utime(path, ns=(before['st_atime_ns'] - 10_000_000_000, before['st_mtime_ns']))
            time.sleep(.12)
            after = snapshot(fd)
            events = [event.fflags for event in kq.control(None, 10, 0)]
            assert child.poll() is None, (action, child.returncode)
            assert all(before[key] == after[key] for key in FIELDS if key not in ['st_atime_ns', 'st_ctime_ns'])
            if action == 'atime_only':
                assert before['st_atime_ns'] != after['st_atime_ns']
                assert any(flags & select.KQ_NOTE_ATTRIB for flags in events)
            records.append({'action': action, 'before': before, 'after': after,
                            'vnode_flags': events, 'running': True})
        return records

    results = []
    groups = [gid for gid in os.getgroups() if gid != os.getgid()]
    cases = ['chmod', 'acl', 'hardlink', 'flags', 'write', 'rename', 'delete', 'replace', 'replace_chmod', 'replace_hardlinked']
    if groups:
        cases.append('group_owner')
    for case in cases:
        path = out / f'fixture-{case}'; path.write_text('original\n'); path.chmod(0o644)
        if case == 'replace_hardlinked': os.link(path, path.with_suffix('.link'))
        fd, kq = monitor(path)
        child = subprocess.Popen([str(executable), str(path)], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=env, start_new_session=True)
        try:
            first = child.stdout.readline(); assert first.strip() == 'ready', first
            checks = benign(child, path, fd, kq)
            before = snapshot(fd); start = time.perf_counter()
            if case == 'chmod': path.chmod(0o600)
            elif case == 'acl': subprocess.run(['/bin/chmod', '+a', 'everyone deny write', str(path)], check=True)
            elif case == 'hardlink': os.link(path, path.with_suffix('.link'))
            elif case == 'flags': os.chflags(path, before['st_flags'] ^ stat.UF_NODUMP)
            elif case == 'group_owner': os.chown(path, -1, groups[0])
            elif case == 'write':
                with path.open('ab') as f: f.write(b'changed\n'); f.flush()
            elif case == 'rename': path.rename(path.with_suffix('.renamed'))
            elif case == 'delete': path.unlink()
            elif case.startswith('replace'):
                staged = path.with_suffix('.new'); staged.write_text('replacement\n')
                os.replace(staged, path)
                if case == 'replace_chmod': os.fchmod(fd, 0o600)
            if case in ['rename', 'delete', 'replace', 'replace_hardlinked']:
                time.sleep(.3)
                assert child.poll() is None, (case, child.returncode, child.stderr.read())
                results.append({'case': case, 'response_ms': None, 'running': True,
                                'wiper_invoked': False, 'before': before, 'after': snapshot(fd)})
                # The old inode is still protected after its pathname is replaced.
                os.fchmod(fd, 0o600)
            child.wait(timeout=1)
            elapsed = (time.perf_counter() - start) * 1000
            stdout, stderr = child.stdout.read(), child.stderr.read()
            assert child.returncode == 1 and 'wiped' in stdout and '[TAMPER]' in stderr, (case, child.returncode, stdout, stderr)
            results.append({'case': case, 'benign_checks': checks, 'before': before, 'after': snapshot(fd),
                            'vnode_flags': [e.fflags for e in kq.control(None, 10, 0)],
                            'response_ms': elapsed, 'wiper_invoked': True, 'exit_code': 1, 'stderr': stderr})
        finally:
            cleanup(child); kq.close(); os.close(fd)
            if case in ['hardlink', 'replace_hardlinked']: path.with_suffix('.link').unlink(missing_ok=True)
            if case == 'rename': path.with_suffix('.renamed').unlink(missing_ok=True)
            if path.exists():
                os.chflags(path, 0)
                if case == 'acl': subprocess.run(['/bin/chmod', '-N', str(path)], check=True)
                if case == 'group_owner': os.chown(path, -1, os.getgid())

    # Copy and operate on the full built executable; never mutate the frozen source.
    binary_source = a.binary.resolve(); binary = out / 'dsco-after-copy'
    shutil.copyfile(binary_source, binary); binary.chmod(0o755)
    binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
    fd, kq = monitor(binary)
    argv = [str(binary), '--profile', 'full', '--gov-model', 'none', '--tool-exec-raw',
            'bash', json.dumps({'command': 'sleep 10'})]
    real_env = dict(env, DSCO_MCP_HEADLESS='0', DSCO_TOOLMGMT='0', DSCO_PERF='1',
                    DSCO_NO_AUTO_INTERACTIVE='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1')
    stdout_file = (out / 'real-after.stdout').open('w')
    stderr_file = (out / 'real-after.stderr').open('w')
    child = subprocess.Popen(argv, stdout=stdout_file, stderr=stderr_file, env=real_env,
                             cwd=out, start_new_session=True)
    try:
        # Full-startup local tool path installs the actual tamper watcher before the tool.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            assert child.poll() is None, child.returncode
            if 'ready' in (out / 'real-after.stderr').read_text(): break
            time.sleep(.05)
        else: raise AssertionError('full runtime did not reach ready')
        time.sleep(.1); kq.control(None, 10, 0)
        records = benign(child, binary, fd, kq)
        # Replace the executable while its tool call remains active, then verify
        # the loaded inode is still monitored independently of the pathname.
        before = snapshot(fd)
        staged = out / 'dsco-staged'; shutil.copyfile(binary_source, staged); staged.chmod(0o755)
        os.replace(staged, binary)
        time.sleep(.4)
        assert child.poll() is None, child.returncode
        assert os.fstat(fd).st_ino != binary.stat().st_ino
        start = time.perf_counter(); os.fchmod(fd, 0o700)
        child.wait(timeout=1)
        elapsed = (time.perf_counter() - start) * 1000
        assert child.returncode == 1
        stderr_file.flush()
        stderr = (out / 'real-after.stderr').read_text()
        assert '[TAMPER] binary security attributes modified' in stderr, stderr
        real = {'argv': argv, 'source_binary': str(binary_source), 'binary_sha256': binary_sha,
                'atomic_replacement_survived': True, 'benign_checks': records, 'chmod_before': before, 'chmod_after': snapshot(fd),
                'chmod_response_ms': elapsed, 'exit_code': child.returncode}
    finally:
        cleanup(child); stdout_file.close(); stderr_file.close(); kq.close(); os.close(fd)
    (out / 'real-after.json').write_text(json.dumps(real, indent=2) + '\n')
    summary = {'passed': True, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
               'compiler_argv': command, 'mutations': results, 'real_binary': real,
               'limits': 'macOS only. Owner UID and revocation not mutated. Invariant checks retain UID and NOTE_REVOKE handling. ACL mutation and group-owner change tested when current group memberships permit.'}
    (out / 'metadata-results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'passed': True, 'mutations': [(r['case'], r['response_ms']) for r in results],
                      'real_binary_chmod_ms': real['chmod_response_ms'], 'binary_sha256': binary_sha}, indent=2))


if __name__ == '__main__':
    main()
