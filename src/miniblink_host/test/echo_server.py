#!/usr/bin/env python3
# Minimal httpbin-shaped echo server for verifying mb_smoke's network cases locally.
#   /cookies/set?<n>=<v>  -> 302 to /cookies, Set-Cookie: n=v
#   /cookies              -> {"cookies": {<from request Cookie header>}}
#   /headers              -> {"headers": {<request headers, original case>}}
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

COUNTS = {}
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
        elif u.path == '/headers':
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
        fields = {k: values[0] for k, values in parse_qs(raw).items() if values}
        body = json.dumps({'data': raw, 'form': fields,
                           'headers': dict(self.headers.items())})
        self.send_body(200, body, 'application/json')

if __name__ == '__main__':
    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(('127.0.0.1', 8899), H)
    print('listening on 127.0.0.1:8899', flush=True)
    srv.serve_forever()
