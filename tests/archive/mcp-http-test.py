#!/usr/bin/env python3
"""Drives vimb --mcp-http with the official MCP Python SDK (pip install mcp)
over its streamable HTTP transport, and checks by hand what the transport
refuses. Archives part of the test site (tests/archive/site, served on
127.0.0.1:9445); one crawl is slow on purpose, to outlast a keepalive.

    python3 tests/archive/mcp-http-test.py [path/to/vimb]

vimb needs a display (DISPLAY); set XDG_CONFIG_HOME, XDG_DATA_HOME and
XDG_CACHE_HOME to keep it off your own profile. Exits 0 if all is well."""

import asyncio
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

from mcp import Client

VIMB = sys.argv[1] if len(sys.argv) > 1 else "src/vimb"
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")
PORT = int(os.environ.get("VIMB_MCP_PORT", "9444"))
URL = f"http://127.0.0.1:{PORT}/mcp"
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def value(result):
    if result.is_error:
        raise RuntimeError(result.content[0].text)
    return result.structured_content or json.loads(result.content[0].text)


def post(body, headers=None, method="POST"):
    """A request by hand: (status, headers, body)."""
    data = body if isinstance(body, bytes) or body is None else json.dumps(body).encode()
    h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    h.update(headers or {})
    request = urllib.request.Request(URL, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(request, timeout=30) as r:
            return r.status, r.headers, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read().decode()


def initialize_by_hand():
    status, headers, body = post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "by hand", "version": "1"}}})
    return status, headers.get("Mcp-Session-Id"), body


def by_hand():
    status, sid, body = initialize_by_hand()
    check("initialize gives a session", status == 200 and sid and len(sid) == 32,
          f"{status} {sid} {body[:80]}")
    session = {"Mcp-Session-Id": sid, "MCP-Protocol-Version": "2025-06-18"}
    status, _, _ = post({"jsonrpc": "2.0", "method": "notifications/initialized"}, session)
    check("a notification is accepted with 202", status == 202, str(status))
    status, _, body = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    check("no session: 400", status == 400, f"{status} {body[:60]}")
    status, _, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                        {"Mcp-Session-Id": "0" * 32})
    check("an unknown session: 404", status == 404, str(status))
    status, _, body = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                           dict(session, Origin="http://evil.example"))
    check("a web page's Origin: 403", status == 403, f"{status} {body[:60]}")
    status, _, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                        dict(session, Host=f"evil.example:{PORT}"))
    check("another Host: 403", status == 403, str(status))
    status, _, body = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                           dict(session, Origin=f"http://localhost:{PORT}"))
    check("this machine's Origin is served", status == 200 and "crawl" in body, str(status))
    status, _, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                        dict(session, **{"MCP-Protocol-Version": "1999-01-01"}))
    check("an unknown protocol version: 400", status == 400, str(status))
    status, _, _ = post([{"jsonrpc": "2.0", "id": 3, "method": "ping"}], session)
    check("a batch: 400", status == 400, str(status))
    status, _, body = post(b"{not json", session)
    check("not JSON: 400, with a parse error", status == 400 and "-32700" in body, str(status))
    status, headers, _ = post(None, session, method="GET")
    check("GET: 405", status == 405 and "POST" in (headers.get("Allow") or ""), str(status))
    status, headers, body = post({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                                  "params": {"name": "list_tabs", "arguments": {}}}, session)
    check("a call answered at once is JSON", status == 200
          and headers.get("Content-Type", "").startswith("application/json") and "tabs" in body,
          f"{status} {headers.get('Content-Type')}")
    status, _, _ = post(None, session, method="DELETE")
    check("DELETE ends the session", status == 200, str(status))
    status, _, _ = post({"jsonrpc": "2.0", "id": 5, "method": "ping"}, session)
    check("an ended session: 404", status == 404, str(status))


async def sdk():
    async with Client(URL) as client:
        tools = {t.name for t in (await client.list_tools()).tools}
        check("the SDK connects over HTTP", "crawl" in tools, ", ".join(sorted(tools)))
        archive = value(await client.call_tool("start_archive",
                                               {"name": "mcp http test", "allow_internal": True}))
        n = archive["archive"]
        page = value(await client.call_tool("open_page", {"url": SITE + "index.html", "archive": n}))
        check("open_page over HTTP", page.get("status") == 200, json.dumps(page))
        started = time.monotonic()
        crawled = value(await client.call_tool("crawl", {
            "archive": n, "seeds": [SITE + "a.html"], "depth": 1, "scope": "host", "delay": 9,
            "scroll": False}))
        took = time.monotonic() - started
        check("a slow crawl outlasts a keepalive", crawled.get("loaded", 0) >= 2 and took > 15,
              f"{json.dumps(crawled)} in {took:.0f} s")
        saved = value(await client.call_tool("stop_archive", {"archive": n})).get("saved", "")
        check("stop_archive over HTTP", saved.endswith(".wacz") and os.path.exists(saved), saved)


def keepalive_seen():
    """Raw: a slow call's stream carries a keepalive before its answer."""
    status, sid, _ = initialize_by_hand()
    session = {"Mcp-Session-Id": sid, "MCP-Protocol-Version": "2025-06-18"}
    a = json.loads(post({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
        "name": "start_archive", "arguments": {"name": "keepalive", "allow_internal": True}}},
        session)[2])
    n = json.loads(a["result"]["content"][0]["text"])["archive"]
    status, headers, body = post({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
        "name": "crawl", "arguments": {"archive": n, "seeds": [SITE + "a.html"], "depth": 1,
                                       "delay": 9, "scroll": False}}}, session)
    check("a slow call streams, with keepalives",
          headers.get("Content-Type", "").startswith("text/event-stream")
          and ": keepalive" in body and "event: message" in body, body[:120].replace("\n", "|"))
    post({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {
        "name": "stop_archive", "arguments": {"archive": n}}}, session)


vimb = subprocess.Popen([VIMB, "--window-size", "1280x900", "--mcp-http", str(PORT)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
try:
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    by_hand()
    asyncio.run(sdk())
    keepalive_seen()
finally:
    vimb.send_signal(signal.SIGTERM)
    try:
        _, err = vimb.communicate(timeout=int(os.environ.get("VIMB_EXIT_TIMEOUT", "60")))
    except subprocess.TimeoutExpired:
        vimb.kill()
        _, err = vimb.communicate()
check("no CRITICALs", "CRITICAL" not in err, err[-500:] if "CRITICAL" in err else "")
print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
sys.exit(1 if failures else 0)
