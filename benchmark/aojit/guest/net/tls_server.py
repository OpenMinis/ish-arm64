"""Local HTTPS mock API: JSON endpoints and bulk downloads/uploads. usage: tls_server.py <port> <rsa|ec>"""
import http.server, json, os, ssl, sys
port, kind = int(sys.argv[1]), sys.argv[2]
blob = os.urandom(1 << 20)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def _send(self, body, ctype):
        self.send_response(200); self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith('/blob/'):
            self._send(blob * int(self.path.split('/')[2]), 'application/octet-stream')
        else:
            self._send(json.dumps({'ok': True, 'path': self.path, 'items': list(range(50))}).encode(), 'application/json')
    def do_POST(self):
        data = self.rfile.read(int(self.headers['Content-Length']))
        self._send(json.dumps({'len': len(data)}).encode(), 'application/json')
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(f'/tmp/aojit/net/{kind}.crt', f'/tmp/aojit/net/{kind}.key')
srv = http.server.ThreadingHTTPServer(('127.0.0.1', port), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
open(f'/tmp/aojit/net/server_{port}.ready', 'w').close()
srv.serve_forever()
