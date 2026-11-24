#!/usr/bin/env python3
"""A site with sign-ins, for testing vimb's archiving signed in: /login
signs in (a session cookie); /private says whether the visitor is signed in;
/rotate, signed in, sets another cookie.

    python3 tests/archive/signin-server.py PORT"""

import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SESSION = "session=secret-0123456789"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def page(self, title, body, cookie=None):
        html = f"<!doctype html><meta charset=utf-8><title>{title}</title><p>{body}</p>".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(html)))
        if cookie:
            self.send_header("Set-Cookie", cookie + "; Path=/; HttpOnly")
        self.end_headers()
        self.wfile.write(html)

    def do_GET(self):
        signed_in = SESSION in (self.headers.get("Cookie") or "")
        if self.path == "/login":
            self.page("Signed in", "Welcome back.", SESSION)
        elif self.path == "/private":
            self.page("Members" if signed_in else "Sign in",
                      "Hello, member." if signed_in else "Please sign in.")
        elif self.path == "/rotate":
            self.page("Rotated", "A new session.", "rotated=secret-9876543210" if signed_in else None)
        else:
            self.send_error(404)


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
