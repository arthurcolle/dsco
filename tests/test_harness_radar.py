#!/usr/bin/env python3
"""No-token contract tests for the public-source change monitor."""
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import io
import tarfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SPEC = importlib.util.spec_from_file_location("radar", Path(__file__).resolve().parents[1] / "scripts/harness_radar.py")
radar = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(radar)


class RadarTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.db = radar.connect(self.root)
        self.p = {"id": "example", "name": "Example", "url": "https://example.org", "category": "coding-cli-and-workspace"}
        self.s = {"url": "https://example.org/README.md", "kind": "readme", "interval_seconds": 900}
        self.p["sources"] = [self.s]
        self.db.execute("INSERT INTO sources(url,project,kind) VALUES (?,?,?)", (self.s["url"], "example", "readme"))

    def tearDown(self):
        self.db.close(); self.tmp.cleanup()

    def old(self):
        return dict(self.db.execute("SELECT * FROM sources").fetchone())

    def save(self, body, at):
        return radar.record(self.db, self.p, self.s, self.old(), {"status": 200, "body": body, "bytes": len(body), "etag": '"v1"'}, at)

    def test_baseline_is_not_a_feature_change(self):
        self.assertEqual(self.save("Supports MCP and permissions", 100), 0)
        self.assertEqual(self.db.execute("SELECT count(*) FROM events").fetchone()[0], 0)

    def test_same_body_does_not_alert_even_with_new_http_response(self):
        self.save("Supports MCP", 100)
        self.assertEqual(self.save("Supports MCP", 200), 0)

    def test_addition_and_removal_create_pending_candidates(self):
        self.save("No tools\n", 100)
        self.save("Supports MCP\n", 200)
        self.save("No tools\n", 300)
        rows = self.db.execute("SELECT * FROM events").fetchall()
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(r["status"] == "pending" for r in rows))
        self.assertTrue(all("interop" in json.loads(r["axes"]) for r in rows))

    def test_error_preserves_last_good_observation_and_backs_off(self):
        self.save("Known baseline", 100); old = self.old()
        radar.record(self.db, self.p, self.s, old, {"status": 429, "error": "HTTP 429", "retry_seconds": 5000}, 200)
        after = self.old()
        self.assertEqual(after["sha"], old["sha"])
        self.assertEqual(after["success"], 100)
        self.assertGreaterEqual(after["due"], 5200)

    def test_304_refreshes_success_without_losing_body(self):
        self.save("Known baseline", 100)
        radar.record(self.db, self.p, self.s, self.old(), {"status": 304}, 300)
        self.assertEqual(self.old()["body"], "Known baseline")
        self.assertEqual(self.old()["success"], 300)
        self.assertEqual(self.old()["due"], 1200)

    def test_change_and_review_survive_database_reopen(self):
        self.save('baseline', 100); self.save('MCP added', 200); self.db.commit()
        self.db.close(); self.db = radar.connect(self.root)
        row = self.db.execute('SELECT * FROM events').fetchone()
        self.assertEqual(row['status'], 'pending')
        self.assertIn('MCP added', row['diff'])
        self.db.execute("UPDATE events SET status='deferred',note='needs fixture'")
        self.db.commit(); self.db.close(); self.db = radar.connect(self.root)
        self.assertEqual(self.db.execute('SELECT note FROM events').fetchone()[0], 'needs fixture')

    def test_atom_ignores_feed_level_timestamp_churn(self):
        one = '<feed xmlns="http://www.w3.org/2005/Atom"><updated>1</updated><entry><id>v1</id><title>Release</title><updated>1</updated><content>MCP</content></entry></feed>'
        two = one.replace("<updated>1</updated>", "<updated>2</updated>", 1)
        self.assertEqual(radar.normalize(one, "releases"), radar.normalize(two, "releases"))

    def test_metadata_does_not_alert_on_stars(self):
        self.assertEqual(radar.normalize('{"full_name":"a/b","stargazers_count":1}', "metadata"),
                         radar.normalize('{"full_name":"a/b","stargazers_count":2}', "metadata"))

    def test_discovery_uses_identities_not_editorial_descriptions(self):
        data = radar.normalize('- **[Example](https://github.com/a/b)** claimed fastest ever\n| Tool | [GitHub](https://github.com/c/d) |\n', 'directory_md')
        self.assertEqual(json.loads(data), ['a/b', 'c/d'])
        self.assertNotIn('fastest', data)

    def test_acp_archive_is_parsed_without_extracting_paths(self):
        stream = io.BytesIO()
        payload = json.dumps({'id':'test','name':'Test','version':'1','repository':'https://github.com/a/b'}).encode()
        with tarfile.open(fileobj=stream, mode='w:gz') as archive:
            member = tarfile.TarInfo('../../outside/agent.json'); member.size = len(payload)
            archive.addfile(member, io.BytesIO(payload))
            link = tarfile.TarInfo('evil/agent.json'); link.type = tarfile.SYMTYPE; link.linkname = '/etc/passwd'
            archive.addfile(link)
        entries = json.loads(radar.normalize(stream.getvalue(), 'directory_acp'))
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['repository'], 'https://github.com/a/b')

    def test_sync_enforces_total_and_api_request_budgets(self):
        projects = []
        for i in range(8):
            projects.append({'id':f'p{i}', 'sources':[
                {'url':f'https://api.github.com/repos/a/{i}', 'kind':'metadata', 'interval_seconds':900},
                {'url':f'https://example.org/{i}', 'kind':'readme', 'interval_seconds':900}]})
        calls = []
        def fake(source, old):
            calls.append(source['kind'])
            return {'status':200, 'body':'{}' if source['kind']=='metadata' else 'baseline', 'bytes':8}
        args = SimpleNamespace(only='', force=False, max_api=1, max_requests=9, workers=2)
        with patch.object(radar, 'fetch', side_effect=fake), patch('builtins.print'):
            radar.sync(self.db, {'projects':projects}, args)
        self.assertEqual(len(calls), 9)
        self.assertEqual(calls.count('metadata'), 1)

    def test_unknown_project_filter_is_an_error(self):
        args = SimpleNamespace(only='missing', force=False, max_api=0, max_requests=4, workers=2)
        with self.assertRaises(ValueError): radar.sync(self.db, {'projects':[self.p]}, args)

    def test_render_escapes_remote_html_and_preserves_unknowns(self):
        self.p["name"] = '<script>alert("x")</script>'
        radar.render(self.db, {"projects": [self.p]}, self.root / "out")
        page = (self.root / "out/index.html").read_text()
        self.assertNotIn(self.p["name"], page)
        self.assertIn("&lt;script&gt;", page)
        catalog = json.loads((self.root / "out/catalog.json").read_text())
        self.assertEqual(catalog["projects"][0]["last_success"], "never")
        self.assertEqual(catalog["projects"][0]["archived"], "unknown")

    def test_public_https_registry_boundary(self):
        self.s["url"] = "https://user:password@example.com/private"
        path = self.root / "registry.json"; path.write_text(json.dumps({"projects": [self.p]}))
        with self.assertRaises(ValueError): radar.read_registry(path)

    def test_interop_probe_records_missing_binary(self):
        output = self.root / "out"
        with patch.object(radar.os, "access", return_value=False), \
             patch.object(radar.shutil, "which", return_value=None):
            self.assertFalse(radar.run_interop_conformance(output))
        report = json.loads((output / "interop-conformance.json").read_text())
        self.assertFalse(report["ok"])
        self.assertEqual(report["error"], "dsco_executable_not_found")
        self.assertFalse(report["real_invocation"])

    def test_http_conditional_request_end_to_end(self):
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.headers.get("If-None-Match") == '"v1"':
                    self.send_response(304); self.end_headers(); return
                self.send_response(200); self.send_header("ETag", '"v1"'); self.end_headers()
                self.wfile.write(b"Supports MCP\n")
            def log_message(self, *args): pass
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            source = {**self.s, "url": f"http://127.0.0.1:{server.server_port}/README"}
            first = radar.fetch(source, {})
            second = radar.fetch(source, {"etag": first["etag"]})
            self.assertEqual(first["status"], 200)
            self.assertEqual(second["status"], 304)
            self.assertEqual(second["bytes"], 0)
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == "__main__":
    unittest.main()
