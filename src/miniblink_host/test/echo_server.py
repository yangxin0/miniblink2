#!/usr/bin/env python3
# Minimal httpbin-shaped echo server for verifying mb_smoke's network cases locally.
#   /cookies/set?<n>=<v>  -> 302 to /cookies, Set-Cookie: n=v
#   /cookies              -> {"cookies": {<from request Cookie header>}}
#   /headers              -> {"headers": {<request headers, original case>}}
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == '/cookies/set':
            q = parse_qs(u.query)
            self.send_response(302)
            for k, v in q.items():
                self.send_header('Set-Cookie', f'{k}={v[0]}; Path=/')
            self.send_header('Location', '/cookies')
            self.end_headers()
            return
        if u.path == '/cookies':
            jar = {}
            ck = self.headers.get('Cookie', '')
            for part in ck.split(';'):
                part = part.strip()
                if '=' in part:
                    n, _, val = part.partition('=')
                    jar[n.strip()] = val.strip()
            body = json.dumps({'cookies': jar}).encode()
        elif u.path == '/headers':
            hdrs = {k: v for k, v in self.headers.items()}
            body = json.dumps({'headers': hdrs}).encode()
        else:
            body = b'<html><body>home</body></html>'
        self.send_response(200)
        self.send_header('Content-Type',
                         'text/html' if u.path == '/' else 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

if __name__ == '__main__':
    HTTPServer(('127.0.0.1', 8899), H).serve_forever()
