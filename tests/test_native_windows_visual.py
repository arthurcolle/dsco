#!/usr/bin/env python3
"""Owned Kitty graphics/input proof through real DSCO MCP, without inference.

Pointer reports enter the fixture's actual TTY through Kitty, exercising its
normal SGR decoder and production pixel/model handlers. This is terminal input
protocol proof, not a claim of physical mouse automation. Capture only the
explicitly created native window; preserve HOME and close that surface finally.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
from test_harness_surfaces_mcp import MCP


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    parser.add_argument('--fixture', default='build/test_pixel_native_windows')
    parser.add_argument('--output', default='reports/native-windows-20260906')
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    fixture = Path(args.fixture).resolve(strict=True)
    output = Path(args.output).resolve(); output.mkdir(parents=True, exist_ok=True)
    state = Path(tempfile.mkdtemp(prefix='dsco-native-windows-', dir='/tmp'))
    status_path = state / 'status.json'
    surface = None
    captures = []
    stages = {}
    with MCP(binary, state, {'DSCO_SURFACE_DIR': str(state / 'surfaces')}) as client:
        def tool(name, **params): return client.call(name, params)[0]
        def wait(predicate):
            end = time.monotonic() + 8
            current = {}
            while time.monotonic() < end:
                try: current = json.loads(status_path.read_text())
                except (FileNotFoundError, json.JSONDecodeError): pass
                if predicate(current): return current
                time.sleep(.1)
            raise AssertionError({'status': current, 'path': str(status_path)})
        def get(): return wait(lambda s: 'fixture' in s)
        def send(text): tool('surface', action='send_text', surface_id=surface, text=text)
        def remote(command, **params):
            return tool('kitty_remote', command=command, to=started['to'], **params)
        def mouse(button, x, y, released=False):
            f = get()['fixture']
            col = max(1, min(f['cols'], int(x * f['cols'] / f['width']) + 1))
            row = max(1, min(f['rows'], int(y * f['rows'] / f['height']) + 1))
            send(f'\x1b[<{button};{col};{row}{"m" if released else "M"}')
        def click(x, y): mouse(0, x, y); mouse(0, x, y, True)
        def window(s, ident): return next(w for w in s['windows'] if w['id'] == ident)
        def capture(name):
            assert tool('surface', action='focus', surface_id=surface)['verified']
            time.sleep(.4)
            path = output / f'native-windows-{name}.png'
            subprocess.run(['/usr/sbin/screencapture', '-x', '-o', '-l', str(native_id), str(path)],
                           check=True, timeout=8)
            assert path.stat().st_size > 1000
            captures.append(str(path))
        try:
            started = tool('surface', action='start', command='/usr/bin/env', args=[
                'DSCO_NATIVE_WINDOWS_FIXTURE_STATUS=' + str(status_path), str(fixture), '--live'], visible=True)
            assert started['verified'], started
            surface = started['surfaces'][0]['surface_id']
            listing = json.loads(remote('ls')['output'])
            assert len(listing) == 1 and len(listing[0]['tabs']) == 1, listing
            pane = listing[0]['tabs'][0]['windows'][0]
            assert any(str(fixture) in p['cmdline'] for p in pane['foreground_processes']), pane
            native_id = listing[0]['platform_window_id']
            remote('resize-os-window', args=['--match=id:' + str(pane['id']), '--width=160', '--height=48', '--unit=cells'])
            initial = wait(lambda s: len(s.get('windows', [])) == 2 and s.get('fixture', {}).get('cols', 0) > 120
                           and s.get('fixture', {}).get('draft') == 'Draft remains usable: Astra π 🦉')
            stages['initial'] = initial; capture('initial')
            ident = initial['windows'][0]['id']; r = window(initial, ident)['rect']
            x, y = r['x'] + 65, r['y'] + 12
            mouse(0, x, y); mouse(32, x + 48, y + 24); mouse(0, x + 48, y + 24, True)
            moved = wait(lambda s: window(s, ident)['rect']['x'] != r['x'] and s.get('focused_id') == ident)
            stages['moved'] = moved
            r = window(moved, ident)['rect']; x, y = r['x'] + r['width'] - 8, r['y'] + r['height'] - 8
            mouse(0, x, y); mouse(32, x + 70, y + 35); mouse(0, x + 70, y + 35, True)
            resized = wait(lambda s: window(s, ident)['rect']['width'] > r['width'])
            stages['resized'] = resized; capture('resized')
            r = window(resized, ident)['rect']; click(r['x'] + r['width'] - 42, r['y'] + 12)
            zoomed = wait(lambda s: window(s, ident)['zoomed']); stages['zoomed'] = zoomed; capture('zoomed')
            r = window(zoomed, ident)['rect']; click(r['x'] + r['width'] - 42, r['y'] + 12)
            restored = wait(lambda s: not window(s, ident)['zoomed'])
            assert window(restored, ident)['rect'] == window(resized, ident)['rect']
            r = window(restored, ident)['rect']; click(r['x'] + r['width'] / 8, r['y'] + r['height'] - 20)
            action = wait(lambda s: len(s.get('human_events', {}).get('events', [])) == 1)
            receipt = action['human_events']['events'][0]
            assert receipt['window_id'] == ident and receipt['action'] == 'continue' and receipt['source'] == 'terminal_input', receipt
            stages['action'] = action
            send('\x07'); wait(lambda s: s.get('keyboard_focus'))
            send('t')
            tiled = wait(lambda s: 'tiled' in s.get('feedback', '').lower())
            stages['tiled'] = tiled; capture('tiled')
            send('m'); wait(lambda s: s.get('fixture', {}).get('menu', 0) != 0); capture('menu')
            send('m'); wait(lambda s: s.get('fixture', {}).get('menu') == 0)
            before_cols = get()['fixture']['cols']
            remote('resize-os-window', args=['--match=id:' + str(pane['id']), '--width=120', '--height=36', '--unit=cells'])
            smaller = wait(lambda s: s.get('fixture', {}).get('cols', before_cols) < before_cols)
            area = smaller['work_area']
            assert all(w['rect']['y'] + w['rect']['height'] <= area['y'] + area['height'] for w in smaller['windows'])
            a, b = [w['rect'] for w in smaller['windows']]
            assert (a['x'] + a['width'] <= b['x'] or b['x'] + b['width'] <= a['x'] or
                    a['y'] + a['height'] <= b['y'] or b['y'] + b['height'] <= a['y']), 'tiled windows must remain disjoint after terminal resize'
            assert smaller['fixture']['draft'] == initial['fixture']['draft']
            stages['terminal_resize'] = smaller
            r = window(smaller, ident)['rect']
            mouse(65, r['x'] + 60, r['y'] + 75)
            scrolled = wait(lambda s: window(s, ident)['scroll'] > window(smaller, ident)['scroll'])
            assert all(w['scroll'] == 0 for w in scrolled['windows'] if w['id'] != ident)
            stages['independent_scroll'] = scrolled
            send('p'); wait(lambda s: s.get('fixture', {}).get('phase') == 1); capture('phase')
            for expected in (1, 0):
                current = get(); r = current['windows'][-1]['rect']
                click(r['x'] + r['width'] - 14, r['y'] + 12)
                closed = wait(lambda s: len(s.get('windows', [])) == expected)
            assert not closed['visible']; stages['closed'] = closed
            evidence = dict(ok=True, binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(), surface_id=surface,
                native_window_id=native_id, state=str(state), captures=captures, stages=stages,
                input_method='SGR pointer reports through actual owned Kitty TTY; no physical mouse automation')
            (output / 'native-windows-live.json').write_text(json.dumps(evidence, indent=2) + '\n')
            print(json.dumps(dict(ok=True, captures=captures, receipt=receipt, native_window_id=native_id,
                evidence=str(output / 'native-windows-live.json'), checks=list(stages))))
        finally:
            if surface: tool('surface', action='close', surface_id=surface)


if __name__ == '__main__': main()
