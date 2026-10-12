import importlib.util
import json
from pathlib import Path
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

spec = importlib.util.spec_from_file_location('readiness', Path(__file__).resolve().parents[1] / 'scripts/check_mcp_readiness.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class ReadinessTests(unittest.TestCase):
    def test_selection_and_disabled(self):
        self.assertEqual(m.classify({'enabled': False}, 'a', 'a'), 'disabled')
        self.assertEqual(m.classify({'disabled': True}, 'a', '*'), 'disabled')
        self.assertEqual(m.classify({}, 'a', 'b'), 'excluded_by_filter')
        self.assertEqual(m.classify({}, 'a', 'b,a'), 'probe')
        self.assertEqual(m.classify({'command': 'node'}, 'a', '*'), 'stdio_not_probed')

    def test_sse_and_json(self):
        body = {'jsonrpc': '2.0', 'id': 2, 'result': {'tools': []}}
        self.assertEqual(m.parse_response(json.dumps(body), 2), body)
        self.assertEqual(m.parse_response('event: message\ndata: ' + json.dumps(body) + '\n\n', 2), body)
        self.assertEqual(m.parse_response(json.dumps(body), 3), {})

    def serve(self, denied):
        methods = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                methods.append(request['method'])
                if denied:
                    self.send_response(401)
                    self.end_headers()
                    self.wfile.write(b'{"error":"secret-must-not-appear"}')
                    return
                if request['method'] == 'notifications/initialized':
                    self.send_response(204)
                    self.end_headers()
                    return
                result = {'protocolVersion': '2024-11-05'} if request['method'] == 'initialize' else {'tools': [{'name': 'example'}]}
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}).encode())
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            result = m.probe({'url': f'http://127.0.0.1:{server.server_port}/mcp'})
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
        return result, methods

    def test_authenticated_protocol_not_just_tcp(self):
        result, methods = self.serve(False)
        self.assertEqual(result['state'], 'connected')
        self.assertEqual(result['listed_tools'], 1)
        self.assertEqual(methods, ['initialize', 'notifications/initialized', 'tools/list'])

    def test_auth_failure_is_explicit_and_secret_free(self):
        result, methods = self.serve(True)
        self.assertEqual(result, {'state': 'authentication_required', 'http_status': 401})
        self.assertEqual(methods, ['initialize'])
        self.assertNotIn('secret', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
