#!/usr/bin/env python3
# Minimal httpbin-shaped echo server for verifying mb_smoke's network cases locally.
#   /cookies/set?<n>=<v>  -> 302 to /cookies, Set-Cookie: n=v
#   /cookies              -> {"cookies": {<from request Cookie header>}}
#   /headers              -> {"headers": {<request headers, original case>}}
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

COUNTS = {}
AUTH_OBSERVATIONS = {}
COUNTS_LOCK = threading.Lock()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def send_body(self, status, body, content_type='text/html', headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == '/cookies/set':
            self.send_response(302)
            for k, v in q.items():
                self.send_header('Set-Cookie', f'{k}={v[0]}; Path=/')
            self.send_header('Location', '/cookies')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/redirect-to':
            target = q.get('url', ['/'])[0]
            status = int(q.get('status_code', ['302'])[0])
            self.send_response(status)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/auth-chain-start':
            # Record the credential on the redirecting hop. The final endpoint
            # returns both observations, making preservation/re-application tests
            # discriminating instead of checking only the destination request.
            key = q.get('key', ['default'])[0]
            target = q.get('url', ['/auth-chain-final?key=' + key])[0]
            with COUNTS_LOCK:
                AUTH_OBSERVATIONS[key] = self.headers.get('Authorization', '')
            self.send_response(302)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/auth-chain-final':
            key = q.get('key', ['default'])[0]
            with COUNTS_LOCK:
                initial = AUTH_OBSERVATIONS.get(key, '')
            self.send_body(200, json.dumps({
                'initial_authorization': initial,
                'final_authorization': self.headers.get('Authorization', ''),
                'host': self.headers.get('Host', ''),
            }), 'application/json')
            return
        if u.path == '/rewrite-chain-start':
            self.send_response(302)
            self.send_header('Location', '/rewrite-chain-mid')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/rewrite-chain-mid':
            # This response is deliberately wrong. A visible-hop rewrite must
            # replace this transport target with /rewrite-chain-rematched.
            self.send_body(200, '<html><body id="rewrite-chain">UNREWRITTEN</body></html>')
            return
        if u.path == '/rewrite-chain-rematched':
            self.send_response(302)
            self.send_header('Location', '/rewrite-chain-final')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/rewrite-chain-final':
            self.send_body(200, '<html><body id="rewrite-chain">REMATCHED</body></html>')
            return
        if u.path == '/absolute-host-redirect':
            # Model a backend/reverse proxy that constructs an absolute redirect
            # from the transport Host rather than the page-visible public origin.
            host = self.headers.get('Host', '')
            self.send_response(302)
            self.send_header('Location', f'http://{host}/origin?via=absolute')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/scheme-relative-host-redirect':
            # Same reverse-proxy behavior, but with a network-path reference. A
            # public https URL rewritten to this http backend must project the
            # backend authority despite the two URL spaces using different schemes.
            host = self.headers.get('Host', '')
            target = q.get('target', ['/origin?via=scheme-relative'])[0]
            if not target.startswith('/'):
                target = '/'
            self.send_response(302)
            self.send_header('Location', f'//{host}{target}')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/drop-connection':
            # Deterministic curl transport failure: accept the request but produce
            # no HTTP response. Used to verify transparent backend URLs never reach
            # top-level failure callbacks.
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if u.path.startswith('/redirect/'):
            try:
                remaining = int(u.path.rsplit('/', 1)[1])
            except ValueError:
                remaining = 0
            target = '/get' if remaining <= 1 else f'/redirect/{remaining - 1}'
            self.send_response(302)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/empty-redirect':
            key = q.get('key', ['default'])[0]
            with COUNTS_LOCK:
                COUNTS[key] = COUNTS.get(key, 0) + 1
            target = q.get('url', ['/origin'])[0]
            self.send_response(302)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if u.path == '/count':
            key = q.get('key', ['default'])[0]
            with COUNTS_LOCK:
                count = COUNTS.get(key, 0)
            self.send_body(200, json.dumps({'count': count}), 'application/json')
            return
        if u.path == '/reset-count':
            key = q.get('key', ['default'])[0]
            with COUNTS_LOCK:
                COUNTS[key] = 0
            self.send_body(200, json.dumps({'count': 0}), 'application/json')
            return
        if u.path == '/slow':
            delay_ms = min(max(int(q.get('ms', ['300'])[0]), 0), 3000)
            time.sleep(delay_ms / 1000.0)
            marker = q.get('marker', ['SLOW'])[0]
            self.send_body(200, f'<html><body id="slow">{marker}</body></html>')
            return
        if u.path == '/origin':
            host = self.headers.get('Host', '')
            body = ("<html><body id='origin' data-host='" + host + "'>ORIGIN"
                    "<script>document.body.dataset.jsOrigin=location.origin;</script>"
                    "</body></html>")
            self.send_body(200, body)
            return
        if u.path == '/stream-auth':
            key = self.headers.get('X-Stream-Key', '')
            hook_key = self.headers.get('X-Hook-Key', '')
            host = self.headers.get('Host', '')
            self.send_body(200, f'key={key};hook={hook_key};host={host}',
                           'application/octet-stream',
                           {'Content-Disposition': 'attachment; filename="stream.bin"'})
            return
        if u.path == '/sse-auth':
            hook_key = self.headers.get('X-Hook-Key', '')
            self.send_body(200, f'data: hook={hook_key}\n\n',
                           'text/event-stream',
                           {'Access-Control-Allow-Origin': '*',
                            'Cache-Control': 'no-cache'})
            return
        if u.path == '/sse-stream':
            # Deterministic long-lived SSE probe: flush several events while the
            # response remains open, so the client must deliver them before EOF.
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            try:
                for i in range(3):
                    self.wfile.write(f'data: local-{i}\n\n'.encode())
                    self.wfile.flush()
                    time.sleep(0.05)
                # Stay open beyond mb_smoke's 12s deadline. SSE comments keep
                # writes active so the thread notices promptly when the real
                # streaming client closes after its third event. A buffered
                # loader gets no EOF and therefore cannot false-pass this probe.
                deadline = time.monotonic() + 15.0
                while time.monotonic() < deadline:
                    self.wfile.write(b': heartbeat\n\n')
                    self.wfile.flush()
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if u.path == '/head-only':
            self.send_body(405, 'GET was used', 'text/plain')
            return
        if u.path == '/img':
            # 1x1 transparent GIF, so an <img> from the network loads (naturalWidth>0).
            import base64
            gif = base64.b64decode(
                'R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==')
            self.send_body(200, gif, 'image/gif')
            return
        if u.path == '/cookies':
            jar = {}
            ck = self.headers.get('Cookie', '')
            for part in ck.split(';'):
                part = part.strip()
                if '=' in part:
                    n, _, val = part.partition('=')
                    jar[n.strip()] = val.strip()
            body = json.dumps({'cookies': jar})
        elif u.path == '/headers' or u.path.startswith('/headers/'):
            hdrs = {k: v for k, v in self.headers.items()}
            body = json.dumps({'headers': hdrs})
        elif u.path == '/cors-headers':
            hdrs = {k: v for k, v in self.headers.items()}
            self.send_body(200, json.dumps({'headers': hdrs}),
                           'application/json',
                           {'Access-Control-Allow-Origin': '*'})
            return
        elif u.path == '/get':
            body = json.dumps({'url': self.path, 'headers': dict(self.headers.items())})
        elif u.path == '/html':
            self.send_body(200, '<html><body>HTML</body></html>')
            return
        elif u.path.startswith('/status/'):
            try:
                status = int(u.path.rsplit('/', 1)[1])
            except ValueError:
                status = 500
            self.send_body(status, b'', 'text/plain')
            return
        elif u.path == '/response-headers':
            headers = {k: values[0] for k, values in q.items() if values}
            self.send_body(200, json.dumps(headers), 'application/json', headers)
            return
        else:
            body = '<html><body>home</body></html>'
        self.send_body(200, body,
                       'text/html' if u.path == '/' else 'application/json')

    def do_HEAD(self):
        u = urlparse(self.path)
        if u.path == '/head-only':
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', '0')
            self.send_header('X-Observed-Method', 'HEAD')
            self.end_headers()
            return
        self.send_response(404)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_POST(self):
        u = urlparse(self.path)
        length = int(self.headers.get('Content-Length', '0'))
        raw = self.rfile.read(length).decode(errors='replace')
        if u.path == '/redirect-post':
            q = parse_qs(u.query)
            status = int(q.get('status_code', ['307'])[0])
            target = q.get('url', ['/post'])[0]
            self.send_response(status)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        fields = {k: values[0] for k, values in parse_qs(raw).items() if values}
        body = json.dumps({'method': 'POST', 'path': u.path,
                           'data': raw, 'form': fields,
                           'headers': dict(self.headers.items())})
        self.send_body(200, body, 'application/json')

if __name__ == '__main__':
    # Optional argv[1] port; 0 asks the OS for a free one (the smoke harness
    # auto-spawn passes 0 and parses the printed address).
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(('127.0.0.1', port), H)
    print('listening on 127.0.0.1:%d' % srv.server_address[1], flush=True)
    srv.serve_forever()
