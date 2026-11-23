#!/usr/bin/env python3
"""Runs the vimb-archive image (contrib/container) on the test site
(tests/archive/site, served on 127.0.0.1:9445): a crawl, an assistant's
task against a stand-in for Claude (mock_assistant.py, on 9446), MCP over
stdio (mcp-test.py, which needs the mcp package), a timeout, and podman
stop in the middle of a crawl.

    python3 tests/archive/container-test.py [image]

Needs podman, and the image built (podman build -t vimb-archive -f
contrib/container/Containerfile .). Exits 0 if all is well."""

import gzip
import json
import os
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE = sys.argv[1] if len(sys.argv) > 1 else "vimb-archive"
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")
PORT = int(os.environ.get("VIMB_MOCK_PORT", "9446"))
PYTHON_WITH_MCP = os.environ.get("VIMB_MCP_PYTHON", sys.executable)
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def podman(out, env, *args, detach=False, timeout=300):
    """Runs the image with out mounted where it is here, so that paths agree."""
    command = ["podman", "run", "--rm", "--network", "host", "-v", f"{out}:{out}:Z",
               "-e", f"OUT={out}"]
    for k, v in env.items():
        command += ["-e", f"{k}={v}"]
    if detach:
        return subprocess.run(command[:2] + ["-d"] + command[2:] + [IMAGE, *args],
                              capture_output=True, text=True, timeout=60).stdout.strip()
    return subprocess.run(command + [IMAGE, *args], capture_output=True, text=True,
                          timeout=timeout)


def indexed(path):
    with zipfile.ZipFile(path) as z:
        index = gzip.decompress(z.read("indexes/index.cdx.gz")).decode()
    return [json.loads(line.split(" ", 2)[2])["url"] for line in index.splitlines()]


def summary(out):
    with open(os.path.join(out, "summary.json")) as f:
        return json.load(f)


def crawl(tmp):
    out = os.path.join(tmp, "crawl")
    os.makedirs(out)
    run = podman(out, {"ALLOW_INTERNAL": "1", "CRAWL_SCOPE": "host", "CRAWL_DEPTH": "1",
                       "CRAWL_DELAY": "0"}, "crawl", SITE + "index.html")
    check("crawl: exit 0", run.returncode == 0, f"{run.returncode} {run.stderr[-400:]}")
    s = summary(out)
    check("crawl: the summary", s["ok"] and s["mode"] == "script" and len(s["archives"]) == 1,
          json.dumps(s)[:300])
    urls = indexed(s["archives"][0]) if s["archives"] else []
    check("crawl: the pages archived",
          all(SITE + p in urls for p in ("index.html", "a.html", "b.html")), ", ".join(urls))
    check("crawl: pages load in the container", "Crawl index" in run.stdout)


def task(tmp):
    out = os.path.join(tmp, "task")
    os.makedirs(out)
    log = os.path.join(tmp, "mock.log")
    mock = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mock_assistant.py"),
                             "claude", str(PORT), SITE, log])
    time.sleep(0.5)
    try:
        run = podman(out, {"ALLOW_INTERNAL": "1", "SCRIPTS": "1",
                           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}",
                           "ANTHROPIC_API_KEY": "test-key"},
                     "task", f"Archive {SITE}index.html and the page it links to as A.")
    finally:
        mock.terminate()
        mock.wait()
    check("task: exit 0", run.returncode == 0, f"{run.returncode} {run.stderr[-400:]}")
    entries = [json.loads(line) for line in open(log)] if os.path.exists(log) else []
    problems = [p for e in entries for p in e["problems"]]
    check("task: the stand-in found nothing wrong", entries and not problems,
          "; ".join(problems)[:300])
    s = summary(out)
    a = s.get("assistant") or {}
    check("task: the summary", s["ok"] and s["mode"] == "task" and s["archives"]
          and a.get("backend") == "claude" and "Saved the archive" in (a.get("reply") or ""),
          json.dumps(s)[:300])


def mcp(tmp):
    out = os.path.join(tmp, "mcp")
    os.makedirs(out)
    wrapper = os.path.join(tmp, "vimb-in-podman")
    with open(wrapper, "w") as f:
        f.write(f"""#!/bin/sh
exec podman run -i --rm --network host -v {out}:{out}:Z -e OUT={out} {IMAGE} mcp "$@"
""")
    os.chmod(wrapper, 0o755)
    run = subprocess.run([PYTHON_WITH_MCP, "-I", os.path.join(HERE, "mcp-test.py"), wrapper],
                         capture_output=True, text=True, timeout=400)
    last = run.stdout.strip().splitlines()[-1:] or [""]
    check("mcp: mcp-test.py passes against the container", run.returncode == 0,
          last[0] + run.stdout[-600:] if run.returncode else last[0])


def timeout(tmp):
    out = os.path.join(tmp, "timeout")
    os.makedirs(out)
    run = podman(out, {"ALLOW_INTERNAL": "1", "CRAWL_SCOPE": "host", "CRAWL_DELAY": "8",
                       "TIMEOUT": "6"}, "crawl", SITE + "index.html")
    s = summary(out)
    check("timeout: exit 2, and what was archived is saved",
          run.returncode == 2 and s["timed_out"] and len(s["archives"]) == 1
          and os.path.exists(s["archives"][0]), f"{run.returncode} {json.dumps(s)[:300]}")


def stop(tmp):
    out = os.path.join(tmp, "stop")
    os.makedirs(out)
    container = podman(out, {"ALLOW_INTERNAL": "1", "CRAWL_SCOPE": "host", "CRAWL_DELAY": "8"},
                       "crawl", SITE + "index.html", detach=True)
    time.sleep(8)
    subprocess.run(["podman", "stop", "-t", "60", container], capture_output=True, timeout=90)
    waczs = [f for f in os.listdir(out) if f.endswith(".wacz")]
    check("podman stop: the archive is saved", len(waczs) == 1, ", ".join(os.listdir(out)))


with tempfile.TemporaryDirectory(dir=os.environ.get("VIMB_TEST_TMP")) as tmp:
    for test in (crawl, task, mcp, timeout, stop):
        print(f"-- {test.__name__}")
        try:
            test(tmp)
        except Exception as e:      # one failing doesn't stop the rest
            check(f"{test.__name__}: ran", False, repr(e)[:300])

print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
sys.exit(1 if failures else 0)
