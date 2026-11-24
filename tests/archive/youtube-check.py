#!/usr/bin/env python3
"""Archives YouTube videos as they play, then replays them with no network:
the check that replay answers YouTube's SABR streaming -- POSTs to
googlevideo.com's /videoplayback, counted ("rn"), with an id each player
makes up ("cpn") and a protobuf body saying what it has buffered -- from
the archive, each request with its own answer, in order.

    python3 tests/archive/youtube-check.py [--video NAME]... [--keep DIR] [path/to/vimb]

NAME is zoo -- "Me at the zoo" (19 s), one SABR request, found by its
body -- or bunny -- Big Buck Bunny, 75 s of it, a stream of some 25
counted requests -- or a video id, ID[:SECONDS]. Both by default, about
five minutes. --keep DIR keeps the archives there, and what each vimb
logged.

Not run by `make test`: it needs YouTube, live, and a display (DISPLAY).
Each vimb runs with a profile of its own. The replay runs in a network
namespace with only loopback (`unshare -rn`) where that is allowed, so that
nothing can come from the web; elsewhere it says so and replays anyway
(replay never fetches). YouTube changes: a bot check, a consent page or an
ad playing is reported as such. Exits 0 if all is well."""

import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from urllib.parse import parse_qs, urlsplit

VIDEOS = {
    # name: (id, seconds to watch, fewest SABR requests, what the title says)
    "zoo": ("jNQXAC9IVRw", 40, 1, "Me at the zoo"),
    "bunny": ("aqz-KE-bpKQ", 75, 2, "Big Buck Bunny"),
}

LUA = r"""
local mode, url, watch = "@MODE@", "@URL@", @WATCH@
local STATE = [[(function () {
  var v = document.querySelector('video');
  if (!v) return JSON.stringify({none: true});
  var b = [];
  for (var i = 0; i < v.buffered.length; i++)
    b.push(+v.buffered.end(i).toFixed(1));
  return JSON.stringify({t: +v.currentTime.toFixed(1), duration: v.duration,
    paused: v.paused, ended: v.ended, error: v.error && v.error.code, buffered: b,
    width: v.videoWidth, ad: !!document.querySelector('.ad-showing'),
    visible: document.visibilityState, focused: document.hasFocus()});
})()]]
local PLAY = [[(function () {
  var v = document.querySelector('video');
  if (v) { v.muted = true; v.play(); }
  return 1;
})()]]
local SKIP = [[(function () {
  var b = document.querySelector('.ytp-skip-ad-button, .ytp-ad-skip-button');
  if (b) b.click();
  return 1;
})()]]

-- plays it for watch seconds, ads not counted, logging its state each second
local function play(t)
  local played, waited = 0, 0
  while played < watch and waited < watch + 60 do
    vimb.sleep(1)
    waited = waited + 1
    local s = t:eval(STATE) or "{}"
    vimb.log("STATE " .. s)
    if s:find('"ad":true') then t:eval(SKIP) else played = played + 1 end
    if s:find('"ended":true') then break end
    if waited >= 3 and s:find('"paused":true') then t:eval(PLAY) end
  end
end

if mode == "record" then
  local a = assert(vimb.archive.start{name = "youtube check"})
  local t = assert(a:open(url))
  vimb.log("TITLE " .. tostring(t.title))
  if tostring(t.url):find("consent%.") then
    vimb.log("BLOCKED YouTube showed its consent page")
  elseif (t:text() or ""):find("confirm you") then
    vimb.log("BLOCKED YouTube asked to confirm this isn't a bot")
  else
    play(t)
  end
  vimb.log("SAVED " .. assert(a:stop()))
else
  local t, info = vimb.replay([==[@WACZ@]==], url)
  vimb.log("REPLAY " .. tostring(info and info.status) .. " " .. tostring(info and info.title))
  if t then play(t) end
end
"""

ASKED = re.compile(r"replay: POST (\S+): (from the archive|missing)$")
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


def note(text):
    print("     " + text, flush=True)


def plural(n, one, many=None):
    return f"{n} {one if n == 1 else many or one + 's'}"


def kept(keep, name, what):
    """With --keep, a file copied there, or text written there."""
    if not keep:
        return
    os.makedirs(keep, exist_ok=True)
    if what.endswith(".wacz") and os.path.exists(what):
        shutil.copy(what, os.path.join(keep, name))
    else:
        with open(os.path.join(keep, name), "w") as f:
            f.write(what)


def can_isolate():
    try:
        return subprocess.run(["unshare", "-rn", "sh", "-c", "ip link set lo up"],
                              capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def run_vimb(vimb, tmp, name, lua, seconds, isolated=False):
    """Runs vimb on a script, in a profile of its own; its log, by kind, the
    states it logged, and all it wrote (GLib's debug messages, replay's
    "from the archive" and "missing", go to stdout)."""
    profile = os.path.join(tmp, name)
    for d in ("config", "data", "cache"):
        os.makedirs(os.path.join(profile, d), exist_ok=True)
    script = os.path.join(profile, "check.lua")
    with open(script, "w") as f:
        f.write(lua)
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(profile, "config"),
               XDG_DATA_HOME=os.path.join(profile, "data"),
               XDG_CACHE_HOME=os.path.join(profile, "cache"), G_MESSAGES_DEBUG="archive")
    command = [os.path.abspath(vimb), "--window-size", "1280x900", "--script", script]
    if isolated:
        command = ["unshare", "-rn", "sh", "-c", 'ip link set lo up && exec "$0" "$@"'] + command
    # stdin from /dev/null: when vimb's stdin is a socket (as a harness may
    # give it), YouTube's page plays the video but loads no more of itself
    try:
        run = subprocess.run(command, capture_output=True, text=True, env=env,
                             stdin=subprocess.DEVNULL, timeout=seconds + 120)
    except subprocess.TimeoutExpired as e:
        tail = e.stderr or b""
        return {}, [], "timed out: " + (tail.decode(errors="replace") if isinstance(tail, bytes)
                                        else tail)
    log, states = {}, []
    for line in run.stdout.splitlines():
        kind, _, rest = line.partition(" ")
        if kind == "STATE":
            try:
                states.append(json.loads(rest))
            except ValueError:
                pass
        elif kind in ("TITLE", "BLOCKED", "SAVED", "REPLAY"):
            log[kind] = rest
    return log, states, run.stdout + run.stderr


def furthest(states):
    """How far it played, whether it ended, how far it had buffered, and
    whether an ad played."""
    t = max([s.get("t", 0) for s in states] or [0])
    ended = any(s.get("ended") for s in states)
    buffered = max([max(s.get("buffered") or [0]) for s in states] or [0])
    return t, ended, buffered, any(s.get("ad") for s in states)


def sabr_counts(url):
    """The count of a SABR request, or None if url isn't one."""
    parts = urlsplit(url)
    q = parse_qs(parts.query)
    if not parts.path.endswith("/videoplayback") or q.get("sabr") != ["1"] or "rn" not in q:
        return None
    return int(q["rn"][0])


def archived_sabr(wacz):
    with zipfile.ZipFile(wacz) as z:
        index = gzip.decompress(z.read("indexes/index.cdx.gz")).decode()
    counts = []
    for line in index.splitlines():
        entry = json.loads(line.split(" ", 2)[2])
        rn = sabr_counts(entry["url"]) if entry.get("method") == "POST" else None
        if rn is not None and entry.get("status") == "200":
            counts.append(rn)
    return sorted(counts)


def check_video(vimb, tmp, name, video_id, watch, fewest, title, isolated, keep):
    url = f"https://www.youtube.com/watch?v={video_id}"
    lua = LUA.replace("@URL@", url).replace("@WATCH@", str(watch))
    print(f"-- {name}: {url}, {watch} s", flush=True)

    log, states, output = run_vimb(vimb, tmp, f"{name}-record", lua.replace("@MODE@", "record"),
                                   watch)
    kept(keep, f"youtube-{name}-record.log", output)
    if not check(f"{name}: the page loads", title in log.get("TITLE", ""),
                 log.get("TITLE") or output[-300:]):
        return
    if not check(f"{name}: YouTube lets it be watched", "BLOCKED" not in log, log.get("BLOCKED")):
        return
    live_t, live_ended, live_buffered, live_ad = furthest(states)
    duration = next((s["duration"] for s in reversed(states) if s.get("duration")), 0)
    if live_ad:
        note("an ad played while recording: what follows may not hold")
    if not check(f"{name}: it plays live", live_ended or live_t >= min(watch, duration) - 8,
                 f"to {live_t} s of {duration:.0f}" + (", the end" if live_ended else "")):
        return
    wacz = log.get("SAVED", "")
    if not check(f"{name}: saved", wacz.endswith(".wacz") and os.path.exists(wacz), wacz):
        return
    kept(keep, f"youtube-{name}.wacz", wacz)
    counts = archived_sabr(wacz)
    n = len(counts)
    if not check(f"{name}: YouTube streams it by SABR",
                 n >= fewest and counts == list(range(1, n + 1)),
                 plural(n, "request") + (f", rn {counts[0]}..{counts[-1]}" if counts else "")):
        return

    log, states, output = run_vimb(vimb, tmp, f"{name}-replay",
                                   lua.replace("@MODE@", "replay").replace("@WACZ@", wacz),
                                   watch, isolated)
    kept(keep, f"youtube-{name}-replay.log", output)
    check(f"{name}: replay opens the page", log.get("REPLAY", "").startswith("200 "),
          log.get("REPLAY") or output[-300:])
    asked, others = [], []
    for line in output.splitlines():
        m = ASKED.search(line)
        if m:
            rn = sabr_counts(m.group(1))
            if rn is not None:
                asked.append((rn, m.group(2) == "from the archive"))
    for line in output.splitlines():
        if line.endswith(": missing") and "replay: " in line:
            what = line.split("replay: ", 1)[1].rsplit(": ", 1)[0]
            method, _, url_asked = what.partition(" ")
            if sabr_counts(url_asked) is None:
                others.append(what)
    answered = [rn for rn, found in asked if found and rn <= n]
    check(f"{name}: each SABR request answered from the archive, in order",
          answered == list(range(1, n + 1)) and all(found for rn, found in asked if rn <= n),
          " ".join(f"{rn}{'' if found else '?'}" for rn, found in asked))
    past = [rn for rn, _ in asked if rn > n]
    if past:
        check(f"{name}: past the capture, nothing", not any(f for rn, f in asked if rn > n),
              f"rn {past[0]}..{past[-1]} missing")
    t, ended, buffered, _ = furthest(states)
    check(f"{name}: it plays back as far as it played live",
          ended if live_ended else t >= live_t - 5,
          f"to {t} s" + (", the end" if ended else "") + f"; live, {live_t} s")
    check(f"{name}: with what was archived, buffered alike", abs(buffered - live_buffered) < 0.25,
          f"to {buffered} s; live, {live_buffered} s")
    if others:
        hosts = {}
        for what in set(others):
            host = urlsplit(what.partition(" ")[2]).hostname or "?"
            hosts[host] = hosts.get(host, 0) + 1
        top = sorted(hosts.items(), key=lambda h: -h[1])[:4]
        note(f"{plural(len(others), 'other request')} not in the archive "
             f"({plural(len(set(others)), 'address', 'addresses')}: "
             + ", ".join(f"{h} {n}" for h, n in top) + ")")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("vimb", nargs="?", default="src/vimb")
    parser.add_argument("--video", action="append",
                        help="zoo, bunny, or ID[:SECONDS] (default: zoo and bunny)")
    parser.add_argument("--keep", help="a directory to keep the archives in")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        sys.exit("vimb needs a display: set DISPLAY")
    isolated = can_isolate()
    if not isolated:
        note("no network namespace (unshare -rn): replaying with the network there")
    with tempfile.TemporaryDirectory() as tmp:
        for spec in args.video or ["zoo", "bunny"]:
            if spec in VIDEOS:
                video_id, watch, fewest, title = VIDEOS[spec]
                name = spec
            else:
                video_id, _, seconds = spec.partition(":")
                watch, fewest, title, name = int(seconds or 60), 1, "YouTube", video_id
            check_video(args.vimb, tmp, name, video_id, watch, fewest, title, isolated, args.keep)
    print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
