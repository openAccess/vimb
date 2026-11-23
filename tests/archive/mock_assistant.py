#!/usr/bin/env python3
"""Stand-ins for the models behind vimb's assistant, for testing it without
a key or a network: the Claude Messages API and the Gemini API, each
streaming the same scripted conversation over SSE and checking what vimb
sends. (fake-pi.py plays the same script as Pi.)

    python3 tests/archive/mock_assistant.py claude|gemini PORT SITE LOG

SITE is the test site's base URL (http://127.0.0.1:9445/crawl/). Each
request is appended to LOG as a JSON line, with the problems found in it.
Then run vimb with ANTHROPIC_BASE_URL (or GEMINI_BASE_URL) at
http://127.0.0.1:PORT, and the key test-key.

The script: the first request is answered "overloaded" (vimb should try
again); then the model starts an archive, opens the index page, lists its
links, tries a URL it made up (vimb should refuse it) along with a call vimb
can't run (input that isn't JSON, or a tab that isn't one), opens a page it
found and takes a screenshot, stops the archive, and says where it was
saved."""

import json
import re
import sys


class Scenario:
    """The conversation, a step at a time. A step's reply is a list of
    ("thinking", text), ("text", text) and ("call", id, name, raw JSON)."""

    def __init__(self, api, site):
        self.api, self.site, self.state = api, site, {}

    def step(self, turn, results, problems):
        """results: {call id: (is_error, text)} -- the last step's."""
        st, site = self.state, self.site
        call = lambda n, name, args: ("call", f"call_{n}", name, json.dumps(args))
        res = lambda n: results.get(f"call_{n}", (True, ""))
        val = lambda n: _json(res(n)[1]) if not res(n)[0] else {}

        if turn == 0:
            return [("thinking", "Start an archive, then open the page."),
                    ("text", "I'll archive that page."),
                    call(1, "start_archive", {"name": "assistant test"})], "tool_use"
        if turn == 1:
            st["archive"] = val(1).get("archive")
            if not st["archive"]:
                problems.append(f"start_archive failed: {results}")
            return [call(2, "open_page", {"url": site + "index.html", "archive": st["archive"]})], "tool_use"
        if turn == 2:
            page = val(2)
            st["tab"] = page.get("tab")
            if page.get("status") != 200:
                problems.append(f"open_page failed: {results}")
            return [("thinking", "Now the links."), call(3, "page_links", {"tab": st["tab"]})], "tool_use"
        if turn == 3:
            links = val(3).get("links", [])
            st["link"] = next((l["url"] for l in links if l["url"].endswith("/a.html")), None)
            if not st["link"]:
                problems.append(f"no a.html among the links: {links}")
            made_up = call(4, "open_page", {"url": "http://127.0.0.1:9445/collect?secret=Crawl+index"})
            if self.api == "claude":
                broken = ("call", "call_5", "scroll_page", '{"tab": 1, oops')
            else:
                broken = call(5, "scroll_page", {"tab": "oops"})
            return [made_up, broken], "tool_use"
        if turn == 4:
            error, text = res(4)
            if not error or "didn't come from" not in text:
                problems.append(f"the made-up URL wasn't refused: {res(4)}")
            error, text = res(5)
            want = "INVALID_JSON" if self.api == "claude" else "say which tab"
            if want not in text:
                problems.append(f"the call vimb can't run wasn't reported: {res(5)}")
            return [call(6, "open_page", {"url": st["link"], "archive": st["archive"]}),
                    call(7, "screenshot", {"tab": st["tab"]})], "tool_use"
        if turn == 5:
            if val(6).get("status") != 200:
                problems.append(f"opening a found link failed: {res(6)}")
            if res(7) != (False, "<png>"):
                problems.append(f"the screenshot didn't come back as a PNG: {res(7)[1][:80]}")
            return [call(8, "stop_archive", {"archive": st["archive"]})], "tool_use"
        if turn == 6:
            saved = val(8).get("saved", "")
            st["saved"] = saved
            if not saved.endswith(".wacz"):
                problems.append(f"stop_archive failed: {res(8)}")
            # scripts: one that works, and tries what it mustn't; one that
            # never waits; one that eats memory
            script = SCRIPT.replace("SITE", site)
            return [call(9, "run_lua", {"code": script, "description": "reads page B"}),
                    call(10, "run_lua", {"code": "while true do end", "description": "spins"}),
                    call(11, "run_lua", {"code": "local t = {} for i = 1, 1e9 do "
                                         "t[i] = ('x'):rep(4096) .. i end", "description": "eats"})], "tool_use"
        if turn == 7:
            error, text = res(9)
            got = _json(text) if not error else {}
            want = {"io": True, "osexec": True, "load": True, "require": True, "status": 200,
                    "refused": True, "noeval": True, "noquit": True, "learned": True}
            value = got.get("value") or {}
            for key, expected in want.items():
                if value.get(key) != expected:
                    problems.append(f"script: {key} is {value.get(key)!r}, not {expected!r} ({text[:300]})")
            if value.get("links", 0) < 1 or "hello from the script" not in got.get("log", ""):
                problems.append(f"script: links or log wrong: {text[:300]}")
            error, text = res(10)
            if not error or "too long" not in text:
                problems.append(f"the endless script wasn't stopped: {res(10)}")
            error, text = res(11)
            if not error or "memory" not in text:
                problems.append(f"the hungry script wasn't stopped: {res(11)}")
            return [("text", f"Saved the archive to {st['saved']}.")], "end_turn"
        problems.append("the conversation went on too long")
        return [("text", "Done.")], "end_turn"


# What a script may and mayn't do. b.html was among the index's links, so
# it may be opened; a URL made up in the script may not; nor may a link of
# b.html's be opened before the script has read it.
SCRIPT = """
local r = {}
r.io, r.osexec, r.load, r.require = io == nil, os.execute == nil, load == nil, require == nil
local t, info = vimb.open("SITEb.html")
r.status = info and info.status
local ok, err = pcall(t.load, t, "http://127.0.0.1:9445/collect?x=" .. t.title)
r.refused = not ok and tostring(err):find("didn't come from") ~= nil
r.noeval = not pcall(t.eval, t, "1")
r.noquit = not pcall(vimb.quit)
local links = t:links()
r.links = #links
-- a link read by the script may be opened by it
local found = nil
for _, l in ipairs(links) do if l:find("/d.html") then found = l end end
r.learned = found ~= nil and t:load(found) ~= nil
vimb.log("hello from the script")
t:close()
return r
"""


def _json(text):
    try:
        value = json.loads(text)
        return value if isinstance(value, dict) else {}
    except ValueError:
        return {}


def is_png(b64):
    import base64
    try:
        return base64.b64decode(b64)[:8] == b"\x89PNG\r\n\x1a\n"
    except ValueError:
        return False


def pieces(text, n):
    return re.findall(".{1,%d}" % n, text, re.S) or [""]


def sse(data, event=None):
    return ((f"event: {event}\n" if event else "") + f"data: {json.dumps(data)}\n\n").encode()


# ---- Claude ------------------------------------------------------------------

class Claude:
    path = "/v1/messages"

    def __init__(self, scenario):
        self.scenario, self.signatures, self.sigs = scenario, {}, 0

    def check(self, headers, body, problems):
        for header, want in (("anthropic-version", "2026-10-21"), ("x-api-key", "test-key")):
            if headers.get(header) != want:
                problems.append(f"{header} is {headers.get(header)!r}")
        if "server-side-fallback-2026-07-01" not in (headers.get("anthropic-beta") or ""):
            problems.append("no fallback beta")
        for key, want in (("model", "claude-opus-5-5"), ("stream", True), ("fallbacks", "default")):
            if body.get(key) != want:
                problems.append(f"{key} is {body.get(key)!r}")
        if body.get("thinking", {}).get("type") != "adaptive" or "budget_tokens" in body.get("thinking", {}):
            problems.append(f"thinking is {body.get('thinking')}")
        tools = body.get("tools", [])
        if not all(t.get("eager_input_streaming") for t in tools):
            problems.append("a tool without eager_input_streaming")
        if any(t["name"] == "run_javascript" for t in tools):
            problems.append("the assistant was offered run_javascript")
        for m in body["messages"]:
            if m["role"] == "assistant":
                for b in m["content"]:
                    if b["type"] == "thinking" and self.signatures.get(b["signature"]) != b["thinking"]:
                        problems.append(f"a thinking block came back changed: {b}")
        roles = [m["role"] for m in body["messages"]]
        if any(a == b for a, b in zip(roles, roles[1:])):
            problems.append(f"roles don't alternate: {roles}")
        if body["messages"][0]["content"][0]["text"].find(self.scenario.site) < 0:
            problems.append("the task's URL isn't in the first message")

    def turn_and_results(self, body):
        turn = sum(1 for m in body["messages"] if m["role"] == "assistant")
        results = {}
        last = body["messages"][-1]
        for b in last["content"] if last["role"] == "user" else []:
            if b.get("type") == "tool_result":
                content = b["content"]
                if isinstance(content, list):
                    image = next((c for c in content if c.get("type") == "image"), None)
                    text = "<png>" if image and is_png(image["source"]["data"]) else json.dumps(content)
                else:
                    text = content
                results[b["tool_use_id"]] = (bool(b.get("is_error")), text)
        return turn, results

    overloaded = (529, {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})

    def stream(self, blocks, stop):
        out = [sse({"type": "message_start", "message": {
            "id": "msg_mock", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
            "content": [], "stop_reason": None, "usage": {"input_tokens": 10, "output_tokens": 1}}},
            "message_start"), b": a comment, which is to be ignored\n\n"]
        for i, block in enumerate(blocks):
            start = lambda cb: sse({"type": "content_block_start", "index": i, "content_block": cb},
                                   "content_block_start")
            delta = lambda d: sse({"type": "content_block_delta", "index": i, "delta": d},
                                  "content_block_delta")
            if block[0] == "thinking":
                self.sigs += 1
                signature = f"sig-{self.sigs}"
                self.signatures[signature] = block[1]
                out.append(start({"type": "thinking", "thinking": "", "signature": ""}))
                out += [delta({"type": "thinking_delta", "thinking": p}) for p in pieces(block[1], 7)]
                out.append(delta({"type": "signature_delta", "signature": signature}))
            elif block[0] == "text":
                out.append(start({"type": "text", "text": ""}))
                out += [delta({"type": "text_delta", "text": p}) for p in pieces(block[1], 5)]
            else:
                _, call_id, name, raw = block
                out.append(start({"type": "tool_use", "id": call_id, "name": name, "input": {}}))
                out += [delta({"type": "input_json_delta", "partial_json": p}) for p in pieces(raw, 9)]
            out.append(sse({"type": "content_block_stop", "index": i}, "content_block_stop"))
            out.append(sse({"type": "ping"}, "ping"))
        out.append(sse({"type": "message_delta", "delta": {"stop_reason": stop},
                        "usage": {"output_tokens": 42}}, "message_delta"))
        out.append(sse({"type": "message_stop"}, "message_stop"))
        return b"".join(out)


# ---- Gemini ------------------------------------------------------------------

class Gemini:
    path = "/v1beta/models/gemini-4-hydrogen:streamGenerateContent?alt=sse"

    def __init__(self, scenario):
        self.scenario, self.sent, self.sigs = scenario, [], 0

    def check(self, headers, body, problems):
        if headers.get("x-goog-api-key") != "test-key":
            problems.append(f"x-goog-api-key is {headers.get('x-goog-api-key')!r}")
        thinking = body.get("generationConfig", {}).get("thinkingConfig", {})
        if thinking != {"thinkingLevel": "medium", "includeThoughts": True}:
            problems.append(f"thinkingConfig is {thinking}")
        if not body.get("systemInstruction", {}).get("parts", [{}])[0].get("text"):
            problems.append("no systemInstruction")
        functions = body.get("tools", [{}])[0].get("functionDeclarations", [])
        names = {f["name"] for f in functions}
        if "run_javascript" in names or "crawl" not in names:
            problems.append(f"the functions offered: {sorted(names)}")
        for f in functions:
            if "parameters" in f and not f["parameters"].get("properties"):
                problems.append(f"{f['name']} has parameters with no properties")
        contents = body["contents"]
        roles = [c["role"] for c in contents]
        if roles[0] != "user" or any(a == b for a, b in zip(roles, roles[1:])):
            problems.append(f"roles don't alternate: {roles}")
        models = [c["parts"] for c in contents if c["role"] == "model"]
        for i, parts in enumerate(models):
            if i < len(self.sent) and parts != self.sent[i]:
                problems.append(f"model turn {i} came back changed: {parts} != {self.sent[i]}")
        # each response answers its call, all of a step's together, in order
        for i, c in enumerate(contents[1:], 1):
            if contents[i - 1]["role"] == "model" and c["role"] == "user":
                asked = [p["functionCall"]["id"] for p in contents[i - 1]["parts"] if "functionCall" in p]
                answered = [p["functionResponse"]["id"] for p in c["parts"] if "functionResponse" in p]
                if asked != answered:
                    problems.append(f"calls {asked} answered by {answered}")

    def turn_and_results(self, body):
        contents = body["contents"]
        turn = sum(1 for c in contents if c["role"] == "model")
        results = {}
        if contents[-1]["role"] == "user":
            for p in contents[-1]["parts"]:
                fr = p.get("functionResponse")
                if not fr:
                    continue
                response = fr.get("response", {})
                inline = [x["inlineData"] for x in fr.get("parts", []) if "inlineData" in x]
                if inline:
                    ok = (inline[0].get("mimeType") == "image/png" and is_png(inline[0].get("data", ""))
                          and response.get("image", {}).get("$ref") == inline[0].get("displayName"))
                    results[fr["id"]] = (False, "<png>" if ok else json.dumps(fr)[:200])
                elif "error" in response:
                    results[fr["id"]] = (True, str(response["error"]))
                else:
                    results[fr["id"]] = (False, json.dumps(response.get("result")))
        return turn, results

    overloaded = (503, {"error": {"code": 503, "message": "The model is overloaded.",
                                  "status": "UNAVAILABLE"}})

    def stream(self, blocks, stop):
        chunks, expect, signed = [], [], False

        def emit(part):
            chunks.append(part)
            # what vimb should send back: plain text joined, signed parts alone
            plain = set(part) <= {"text", "thought"}
            if (plain and expect and set(expect[-1]) <= {"text", "thought"}
                    and expect[-1].get("thought", False) == part.get("thought", False)):
                expect[-1] = dict(expect[-1], text=expect[-1]["text"] + part["text"])
            else:
                expect.append(dict(part))

        for block in blocks:
            if block[0] in ("thinking", "text"):
                for p in pieces(block[1], 6):
                    emit({"text": p, "thought": True} if block[0] == "thinking" else {"text": p})
            else:
                _, call_id, name, raw = block
                part = {"functionCall": {"id": call_id, "name": name, "args": json.loads(raw)}}
                if not signed:      # the first call of a step carries the signature
                    self.sigs += 1
                    part["thoughtSignature"] = f"gsig-{self.sigs}"
                    signed = True
                emit(part)
        if stop == "end_turn":      # a reply without calls may sign an empty last part
            self.sigs += 1
            emit({"text": "", "thoughtSignature": f"gsig-{self.sigs}"})
        self.sent.append(expect)
        out = []
        for i, part in enumerate(chunks):
            candidate = {"content": {"role": "model", "parts": [part]}, "index": 0}
            if i == len(chunks) - 1:
                candidate["finishReason"] = "STOP"
            out.append(sse({"candidates": [candidate], "modelVersion": "gemini-4-hydrogen"}))
        return b"".join(out)


# ---- serving -----------------------------------------------------------------

def serve(api, port, site, log):
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    scenario = Scenario(api, site)
    proto = Claude(scenario) if api == "claude" else Gemini(scenario)
    state = {"requests": 0, "overloaded": False}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def write_log(self, entry):
            with open(log, "a") as f:
                f.write(json.dumps(entry) + "\n")

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            problems = [] if self.path == proto.path else [f"path {self.path}"]
            proto.check(self.headers, body, problems)
            state["requests"] += 1
            if not state["overloaded"]:
                state["overloaded"] = True
                status, error = proto.overloaded
                self.write_log({"request": state["requests"], "answer": status, "problems": problems})
                payload = json.dumps(error).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            turn, results = proto.turn_and_results(body)
            blocks, stop = scenario.step(turn, results, problems)
            data = proto.stream(blocks, stop)
            self.write_log({"request": state["requests"], "turn": turn, "stop": stop,
                            "problems": problems, "saved": scenario.state.get("saved")})
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            # a chunk at a time, split mid-line, as a network would
            for i in range(0, len(data), 37):
                piece = data[i:i + 37]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    serve(sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4])
