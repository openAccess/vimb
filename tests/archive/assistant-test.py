#!/usr/bin/env python3
"""Runs vimb's assistant against a stand-in for the Claude API
(mock-claude.py): a scripted task that archives two pages of the test site
(tests/archive/site, served on 127.0.0.1:9445), with a made-up URL refused,
broken tool input reported, and a 529 tried again.

    python3 tests/archive/assistant-test.py [path/to/vimb]

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
PORT = int(os.environ.get("VIMB_MOCK_PORT", "9446"))
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory() as tmp:
    log = os.path.join(tmp, "mock.log")
    mock = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mock-claude.py"),
                             str(PORT), SITE, log])
    try:
        time.sleep(0.5)
        env = dict(os.environ, ANTHROPIC_BASE_URL=f"http://127.0.0.1:{PORT}",
                   ANTHROPIC_API_KEY="test-key")
        run = subprocess.run([VIMB, "--window-size", "1280x900",
                              "-C", "set archive-allow-internal=true",
                              "--assistant-task",
                              f"Archive {SITE}index.html and the page it links to as A."],
                             env=env, capture_output=True, text=True, timeout=180)
    finally:
        mock.terminate()
        mock.wait()
    check("vimb exits 0", run.returncode == 0, f"exit {run.returncode}")
    check("no CRITICALs", "CRITICAL" not in run.stderr)
    check("the reply is shown", "Saved the archive to" in run.stderr)
    check("the made-up URL is refused in the pane", "didn't come from the person" in run.stderr)

    entries = [json.loads(line) for line in open(log)]
    check("the 529 was answered by trying again",
          entries and entries[0].get("answer") == 529 and len(entries) == 8, f"{len(entries)} requests")
    problems = [p for e in entries for p in e["problems"]]
    check("the mock found nothing wrong", not problems, "; ".join(problems))

    saved = entries[-1].get("saved") if entries else None
    check("an archive was saved", bool(saved) and os.path.exists(saved), str(saved))
    if saved and os.path.exists(saved):
        with zipfile.ZipFile(saved) as z:
            index = gzip.decompress(z.read("indexes/index.cdx.gz")).decode()
        urls = [json.loads(line.split(" ", 2)[2])["url"] for line in index.splitlines()]
        check("both pages archived", SITE + "index.html" in urls and SITE + "a.html" in urls,
              ", ".join(urls))
        check("the made-up URL never loaded", not any("collect" in u for u in urls))

print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
sys.exit(1 if failures else 0)
