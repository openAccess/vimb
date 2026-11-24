#!/usr/bin/env python3
"""Archives a page that talks over a websocket, then replays it with the
websocket's server gone: the page has the same conversation, from the
archive. Uses the test site (tests/archive/site, on 127.0.0.1:9445) and
ws-server.py (on 9444, needing the websockets package).

    python3 tests/archive/websocket-test.py [path/to/vimb]

vimb needs a display (DISPLAY); set XDG_CONFIG_HOME, XDG_DATA_HOME and
XDG_CACHE_HOME to keep it off your own profile. Exits 0 if all is well."""

import gzip
import json
import os
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
VIMB = sys.argv[1] if len(sys.argv) > 1 else "src/vimb"
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")
PYTHON = os.environ.get("VIMB_WS_PYTHON", sys.executable)
failures = []

RECORD = """
local a = assert(vimb.archive.start{name = "websocket"})
local t = assert(a:open("%(site)sws.html"))
for _ = 1, 50 do if t.title == "Websocket done" then break end vimb.sleep(0.1) end
vimb.log("RECORDED " .. t:eval("[...document.querySelectorAll('li')].map(l => l.textContent).join('|')"))
t:close()
vimb.log("SAVED " .. assert(a:stop()))
"""

REPLAY = """
local t = assert(vimb.replay("%(wacz)s", "%(site)sws.html"))
for _ = 1, 50 do if t.title == "Websocket done" then break end vimb.sleep(0.1) end
vimb.log("REPLAYED " .. t:eval("[...document.querySelectorAll('li')].map(l => l.textContent).join('|')"))
"""


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def run_lua(tmp, name, source):
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write(source)
    run = subprocess.run([VIMB, "--window-size", "1024x768", "-C", "set archive-allow-internal=true",
                          "--script", path], capture_output=True, text=True, timeout=120)
    lines = dict(line.split(" ", 1) for line in run.stdout.splitlines() if " " in line)
    return run, lines


with tempfile.TemporaryDirectory() as tmp:
    server = subprocess.Popen([PYTHON, "-I", os.path.join(HERE, "ws-server.py"), "9444"])
    time.sleep(0.8)
    try:
        run, out = run_lua(tmp, "record.lua", RECORD % {"site": SITE})
    finally:
        server.terminate()
        server.wait()
    want = "open vimb-test|got hello|got echo one|got echo two|done"
    check("recorded the conversation", out.get("RECORDED") == want, out.get("RECORDED"))
    wacz = out.get("SAVED", "")
    check("saved", wacz.endswith(".wacz") and os.path.exists(wacz), wacz)
    if os.path.exists(wacz):
        with zipfile.ZipFile(wacz) as z:
            index = gzip.decompress(z.read("indexes/index.cdx.gz")).decode()
        mimes = [json.loads(l.split(" ", 2)[2]).get("mime") for l in index.splitlines()]
        check("the messages are archived", "application/x-vimb-websocket-messages" in mimes,
              ", ".join(m or "" for m in mimes))
        # the server is gone: replay alone answers
        run, out = run_lua(tmp, "replay.lua", REPLAY % {"site": SITE, "wacz": wacz})
        check("replayed the conversation with the server gone", out.get("REPLAYED") == want,
              out.get("REPLAYED") or run.stderr[-500:])

print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
sys.exit(1 if failures else 0)
