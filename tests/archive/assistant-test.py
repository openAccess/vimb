#!/usr/bin/env python3
"""Runs vimb's assistant, on each of its backends, against stand-ins for the
models (mock_assistant.py for Claude and Gemini, fake-pi.py for Pi): a
scripted task that archives two pages of the test site (tests/archive/site,
served on 127.0.0.1:9445), with a made-up URL refused, a call vimb can't
run reported, a screenshot, and an overloaded API tried again.

    python3 tests/archive/assistant-test.py [path/to/vimb [claude gemini pi]]

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
BACKENDS = sys.argv[2:] or ["claude", "gemini", "pi"]
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")
PORT = int(os.environ.get("VIMB_MOCK_PORT", "9446"))
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def run(backend, tmp):
    log = os.path.join(tmp, f"{backend}.log")
    env = dict(os.environ, VIMB_TEST_SITE=SITE)
    mock = None
    if backend == "pi":
        env.update(VIMB_PI_COMMAND=os.path.join(HERE, "fake-pi.py"), VIMB_FAKE_PI_LOG=log)
    else:
        mock = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mock_assistant.py"),
                                 backend, str(PORT), SITE, log])
        base = f"http://127.0.0.1:{PORT}"
        env.update(ANTHROPIC_BASE_URL=base, ANTHROPIC_API_KEY="test-key",
                   GEMINI_BASE_URL=base, GEMINI_API_KEY="test-key")
        time.sleep(0.5)
    try:
        done = subprocess.run([VIMB, "--window-size", "1280x900",
                               "-C", f"set archive-allow-internal=true|set assistant={backend}",
                               "--assistant-task",
                               f"Archive {SITE}index.html and the page it links to as A."],
                              env=env, capture_output=True, text=True, timeout=180)
    finally:
        if mock:
            mock.terminate()
            mock.wait()

    print(f"-- {backend}")
    check(f"{backend}: vimb exits 0", done.returncode == 0, f"exit {done.returncode}")
    check(f"{backend}: no CRITICALs", "CRITICAL" not in done.stderr)
    check(f"{backend}: the reply is shown", "Saved the archive to" in done.stderr)
    check(f"{backend}: the made-up URL is refused in the pane",
          "didn't come from the person" in done.stderr)
    entries = [json.loads(line) for line in open(log)] if os.path.exists(log) else []
    if backend != "pi":
        check(f"{backend}: the overloaded API was tried again",
              entries and entries[0].get("answer") in (503, 529) and len(entries) == 8,
              f"{len(entries)} requests")
    problems = [p for e in entries for p in e["problems"]]
    check(f"{backend}: the stand-in found nothing wrong", entries and not problems,
          "; ".join(problems) or f"{len(entries)} entries")

    saved = entries[-1].get("saved") if entries else None
    check(f"{backend}: an archive was saved", bool(saved) and os.path.exists(saved), str(saved))
    if saved and os.path.exists(saved):
        with zipfile.ZipFile(saved) as z:
            index = gzip.decompress(z.read("indexes/index.cdx.gz")).decode()
        urls = [json.loads(line.split(" ", 2)[2])["url"] for line in index.splitlines()]
        check(f"{backend}: both pages archived",
              SITE + "index.html" in urls and SITE + "a.html" in urls, ", ".join(urls))
        check(f"{backend}: the made-up URL never loaded", not any("collect" in u for u in urls))
    if done.returncode or problems:
        sys.stderr.write(done.stderr[-3000:])


with tempfile.TemporaryDirectory() as tmp:
    for backend in BACKENDS:
        run(backend, tmp)

print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
sys.exit(1 if failures else 0)
