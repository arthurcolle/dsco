#!/usr/bin/env python3
"""Benchmark production native transport reuse with TLS loopback; no inference.

Framing is a fixture; actual parser/stream behavior is covered separately by
native-provider-transport and stream-completion regressions.
"""
import argparse
import http.server
import json
import socket
import ssl
import statistics
import subprocess
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT = r'''
#include "provider_transport.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static bool terminal;
static bool graceful;
static double now_ms(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1000000.0; }
static size_t receive(void *p,size_t z,size_t n,void *ctx) {
 (void)ctx;size_t len=z*n;
 if(memmem(p,len,"response.completed",18))terminal=true;
 return terminal&&!graceful?0:len;
}
int main(int argc,char**argv){
 if(argc<3)return 2;graceful=atoi(argv[2]);
 curl_global_init(CURL_GLOBAL_DEFAULT);CURLM *multi=NULL;CURL *easy=NULL;
 for(int i=0;i<30;i++){
  terminal=false;if(!easy)easy=curl_easy_init();curl_easy_reset(easy);
  curl_easy_setopt(easy,CURLOPT_URL,argv[1]);curl_easy_setopt(easy,CURLOPT_POSTFIELDS,"{}");
  curl_easy_setopt(easy,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(easy,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(easy,CURLOPT_SSL_VERIFYPEER,0L);curl_easy_setopt(easy,CURLOPT_SSL_VERIFYHOST,0L);
  curl_easy_setopt(easy,CURLOPT_TIMEOUT_MS,strstr(argv[1],"/silent")?40L:3000L);curl_easy_setopt(easy,CURLOPT_TCP_NODELAY,1L);
  double begin=now_ms();CURLcode rc=graceful?provider_transport_perform(&multi,easy,&terminal):curl_easy_perform(easy);double elapsed=now_ms()-begin;
  long connects=0,status=0;curl_easy_getinfo(easy,CURLINFO_NUM_CONNECTS,&connects);curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);
  printf("{\"request\":%d,\"ms\":%.3f,\"connects\":%ld,\"status\":%ld,\"code\":%d,\"terminal\":%s}\n",i,elapsed,connects,status,rc,terminal?"true":"false");
  if(status!=200)return 3;
  if(strstr(argv[1],"/silent")){if(terminal||rc!=CURLE_OPERATION_TIMEDOUT)return 4;}
  else if(!terminal)return 3;
  if(rc!=CURLE_OK){curl_easy_cleanup(easy);easy=NULL;}
 }
 if(easy)curl_easy_cleanup(easy);curl_multi_cleanup(multi);curl_global_cleanup();
}
'''

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path)
    ap.add_argument("--sanitize", action="store_true")
    args = ap.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix="dsco-native-reuse-") as tmp:
        folder = Path(tmp)
        (folder / "client.c").write_text(CLIENT)
        subprocess.run(["cc", "-O2", "-D_GNU_SOURCE", *(["-fsanitize=address,undefined", "-g"] if args.sanitize else []),
            "-I" + str(ROOT / "include"), str(folder / "client.c"),
            str(ROOT / "src/provider_transport.c"), "-lcurl", "-o", str(folder / "client")], check=True)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(folder / "key.pem"), "-out", str(folder / "cert.pem"),
            "-days", "1", "-subj", "/CN=localhost"], check=True, capture_output=True)

        class Server(http.server.ThreadingHTTPServer):
            daemon_threads = True
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def setup(self):
                super().setup()
                self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            def log_message(self, *_): pass
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                if self.path == "/silent": return
                body = b'data: {"type":"response.completed"}\n\n'
                self.wfile.write(("%x\r\n" % len(body)).encode() + body + b"\r\n" +
                    (b"0\r\n\r\n" if self.path == "/ended" else b""))
                self.wfile.flush()
        server = Server(("127.0.0.1", 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(folder / "cert.pem", folder / "key.pem")
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for path in ("ended", "held", "silent"):
                for candidate in (False, True):
                    cp = subprocess.run([str(folder / "client"),
                        f"https://127.0.0.1:{server.server_port}/{path}", str(int(candidate))],
                        capture_output=True, text=True, timeout=15)
                    assert cp.returncode == 0, (path, candidate, cp.stdout, cp.stderr)
                    rows = [json.loads(line) for line in cp.stdout.splitlines()]
                    connections = sum(row["connects"] for row in rows)
                    assert connections == (1 if candidate and path == "ended" else 30), (path, candidate, rows)
                    results.append({"case": path, "native_reuse": candidate, "new_connections": connections,
                        "median_ms": statistics.median(row["ms"] for row in rows),
                        "p95_ms": sorted(row["ms"] for row in rows)[28], "requests": rows})
        finally:
            server.shutdown()
            server.server_close()
    report = {"passed": True, "scope": "TLS loopback transport only; fixture framing", "results": results}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "results": [{key: value for key, value in row.items()
          if key != "requests"} for row in results]}, indent=2))

if __name__ == "__main__":
    main()
