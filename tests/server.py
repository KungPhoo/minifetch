"""Run the native regression executable against an isolated HTTP fixture."""
import http.server
import subprocess
import sys
import threading
import urllib.parse

barrier = threading.Barrier(8, timeout=10)

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def handle_request(self):
        path = urllib.parse.urlsplit(self.path).path
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length)
        status, payload, headers = 200, b'ok', []
        if path == '/echo':
            payload = body
        elif path == '/query':
            payload = self.path.encode()
        elif path == '/method':
            payload = self.command.encode()
        elif path == '/header':
            payload = self.headers.get('X-Test', '').encode()
        elif path == '/cookie':
            payload = self.headers.get('Cookie', '').encode()
        elif path == '/private/cookie':
            payload = self.headers.get('Cookie', '').encode()
        elif path == '/set':
            headers = [('Set-Cookie', 'session=a%2Fb; Path=/'),
                       ('Set-Cookie', 'private=secret; Path=/private'),
                       ('Set-Cookie', 'secure=secret; Secure; Path=/'),
                       ('Set-Cookie', 'expired=old; Expires=Wed, 01 Jan 2020 00:00:00 GMT; Path=/')]
        elif path == '/delete':
            headers = [('Set-Cookie', 'session=; Path=/; Max-Age=0')]
        elif path == '/redirect':
            status, payload, headers = 302, b'redirect-body', [('Location', '/target')]
        elif path == '/status599':
            status, payload = 599, b'custom-error'
        elif path == '/error':
            status, payload = 418, b'error-body'
        elif path == '/parallel':
            barrier.wait()
            identity = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['id'][0]
            headers = [('Set-Cookie', f'thread{identity}=yes; Path=/')]
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(payload)

    do_GET = do_POST = do_PUT = do_PATCH = do_HEAD = handle_request

servers = [http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) for _ in range(2)]
for server in servers:
    threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    command = [sys.argv[1]] + [f'http://127.0.0.1:{s.server_port}' for s in servers]
    subprocess.run(command, check=True, timeout=40)
finally:
    for server in servers:
        server.shutdown()
        server.server_close()
