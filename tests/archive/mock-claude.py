#!/usr/bin/env python3
"""A stand-in for the Claude Messages API, for testing vimb's assistant
without a key or a network: it streams scripted replies over SSE and checks
what vimb sends.

    python3 tests/archive/mock-claude.py PORT SITE LOG

SITE is the test site's base URL (http://127.0.0.1:9445/crawl/). Each
request is appended to LOG as a JSON line, with the problems found in it.
Then run vimb with ANTHROPIC_BASE_URL=http://127.0.0.1:PORT and any key.

The script: the first request is answered 529 (vimb should try again); then
the model starts an archive, opens the index page, lists its links, tries a
URL it made up (vimb should refuse it), sends a tool call whose input isn't
JSON (vimb should say so, not run it), opens a page it found, stops the
archive, and says where it was saved."""

import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, SITE, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3]
state = {"requests": 0, "overloaded": False, "signatures": {}}


def sse(event, data):
    return f"event: {event}\ndata: {json.dumps(data)}\n\n".encode()


def reply(blocks, stop):
    """The events of a reply. blocks are (type, payload) with payloads in
    pieces, as the API streams them."""
    out = [sse("message_start", {"type": "message_start", "message": {
        "id": "msg_mock", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
        "content": [], "stop_reason": None, "usage": {"input_tokens": 10, "output_tokens": 1}}})]
    out.append(b": a comment, which is to be ignored\n\n")
    for i, (kind, payload) in enumerate(blocks):
        if kind == "thinking":
            text, signature = payload
            out.append(sse("content_block_start", {"type": "content_block_start", "index": i,
                                                   "content_block": {"type": "thinking",
                                                                     "thinking": "", "signature": ""}}))
            for piece in re.findall(r".{1,7}", text, re.S):
                out.append(sse("content_block_delta", {"type": "content_block_delta", "index": i,
                                                       "delta": {"type": "thinking_delta",
                                                                 "thinking": piece}}))
            out.append(sse("content_block_delta", {"type": "content_block_delta", "index": i,
                                                   "delta": {"type": "signature_delta",
                                                             "signature": signature}}))
        elif kind == "text":
            out.append(sse("content_block_start", {"type": "content_block_start", "index": i,
                                                   "content_block": {"type": "text", "text": ""}}))
            for piece in re.findall(r".{1,5}", payload, re.S):
                out.append(sse("content_block_delta", {"type": "content_block_delta", "index": i,
                                                       "delta": {"type": "text_delta",
                                                                 "text": piece}}))
        elif kind == "tool_use":
            tool_id, name, raw = payload
            out.append(sse("content_block_start", {"type": "content_block_start", "index": i,
                                                   "content_block": {"type": "tool_use", "id": tool_id,
                                                                     "name": name, "input": {}}}))
            for piece in re.findall(r".{1,9}", raw, re.S):
                out.append(sse("content_block_delta", {"type": "content_block_delta", "index": i,
                                                       "delta": {"type": "input_json_delta",
                                                                 "partial_json": piece}}))
        out.append(sse("content_block_stop", {"type": "content_block_stop", "index": i}))
        out.append(sse("ping", {"type": "ping"}))
    out.append(sse("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop},
                                     "usage": {"output_tokens": 42}}))
    out.append(sse("message_stop", {"type": "message_stop"}))
    return b"".join(out)


def last_results(body):
    """The tool results in the last message, by tool_use_id."""
    last = body["messages"][-1]
    if last["role"] != "user" or not isinstance(last["content"], list):
        return {}
    return {b["tool_use_id"]: b for b in last["content"] if b.get("type") == "tool_result"}


def result_json(block):
    try:
        return json.loads(block["content"]) if isinstance(block["content"], str) else None
    except ValueError:
        return None


def step(body, problems):
    """The reply to a request, by how far the conversation has got."""
    turns = sum(1 for m in body["messages"] if m["role"] == "assistant")
    results = last_results(body)
    use = lambda n, name, args: ("tool_use", (f"toolu_{n}", name, json.dumps(args)))
    if turns == 0:
        text = body["messages"][0]["content"][0]["text"]
        if SITE not in text:
            problems.append("the task's URL isn't in the first message")
        return [("thinking", ("Start an archive, then open the page.", "sig-1")),
                ("text", "I'll archive that page."),
                use(1, "start_archive", {"name": "assistant test"})], "tool_use"
    if turns == 1:
        archive = result_json(results.get("toolu_1", {})) or {}
        state["archive"] = archive.get("archive")
        if not state["archive"]:
            problems.append(f"start_archive failed: {results}")
        return [use(2, "open_page", {"url": SITE + "index.html", "archive": state["archive"]})], "tool_use"
    if turns == 2:
        page = result_json(results.get("toolu_2", {})) or {}
        state["tab"] = page.get("tab")
        if page.get("status") != 200:
            problems.append(f"open_page failed: {results}")
        return [("thinking", ("Now the links.", "sig-3")), use(3, "page_links", {"tab": state["tab"]})], "tool_use"
    if turns == 3:
        links = (result_json(results.get("toolu_3", {})) or {}).get("links", [])
        state["link"] = next((l["url"] for l in links if l["url"].endswith("/a.html")), None)
        if not state["link"]:
            problems.append(f"no a.html among the links: {links}")
        # a URL it made up, carrying what it read; and input that isn't JSON
        return [use(4, "open_page", {"url": "http://127.0.0.1:9445/collect?secret=Crawl+index"}),
                ("tool_use", ("toolu_5", "scroll_page", '{"tab": 1, oops'))], "tool_use"
    if turns == 4:
        made_up, broken = results.get("toolu_4", {}), results.get("toolu_5", {})
        if not made_up.get("is_error") or "didn't come from" not in str(made_up.get("content")):
            problems.append(f"the made-up URL wasn't refused: {made_up}")
        if "INVALID_JSON" not in str(broken.get("content")):
            problems.append(f"the broken input wasn't reported: {broken}")
        return [use(6, "open_page", {"url": state["link"], "archive": state["archive"]})], "tool_use"
    if turns == 5:
        page = result_json(results.get("toolu_6", {})) or {}
        if page.get("status") != 200:
            problems.append(f"opening a found link failed: {results}")
        return [use(7, "stop_archive", {"archive": state["archive"]})], "tool_use"
    if turns == 6:
        saved = (result_json(results.get("toolu_7", {})) or {}).get("saved", "")
        state["saved"] = saved
        if not saved.endswith(".wacz"):
            problems.append(f"stop_archive failed: {results}")
        return [("text", f"Saved the archive to {saved}.")], "end_turn"
    problems.append("the conversation went on too long")
    return [("text", "Done.")], "end_turn"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        problems = []
        for header, want in (("anthropic-version", "2026-10-21"), ("x-api-key", "test-key")):
            if self.headers.get(header) != want:
                problems.append(f"{header} is {self.headers.get(header)!r}")
        if "server-side-fallback-2026-07-01" not in (self.headers.get("anthropic-beta") or ""):
            problems.append("no fallback beta")
        if self.path != "/v1/messages":
            problems.append(f"path {self.path}")
        for key, want in (("model", "claude-opus-5-5"), ("stream", True), ("fallbacks", "default")):
            if body.get(key) != want:
                problems.append(f"{key} is {body.get(key)!r}")
        if body.get("thinking", {}).get("type") != "adaptive" or "budget_tokens" in body.get("thinking", {}):
            problems.append(f"thinking is {body.get('thinking')}")
        if not all(t.get("eager_input_streaming") for t in body.get("tools", [])):
            problems.append("a tool without eager_input_streaming")
        if any(t["name"] == "run_javascript" for t in body.get("tools", [])):
            problems.append("the assistant was offered run_javascript")
        # the replies come back as they went: thinking with its signature
        for m in body["messages"]:
            if m["role"] == "assistant":
                for b in m["content"]:
                    if b["type"] == "thinking":
                        want = state["signatures"].get(b["signature"])
                        if want is None or b["thinking"] != want:
                            problems.append(f"a thinking block came back changed: {b}")
        roles = [m["role"] for m in body["messages"]]
        if any(a == b for a, b in zip(roles, roles[1:])):
            problems.append(f"roles don't alternate: {roles}")

        state["requests"] += 1
        if not state["overloaded"]:
            state["overloaded"] = True
            self.log({"request": state["requests"], "answer": 529, "problems": problems})
            payload = json.dumps({"type": "error", "error": {"type": "overloaded_error",
                                                             "message": "Overloaded"}}).encode()
            self.send_response(529)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        blocks, stop = step(body, problems)
        for kind, payload in blocks:
            if kind == "thinking":
                state["signatures"][payload[1]] = payload[0]
        self.log({"request": state["requests"], "messages": len(body["messages"]),
                  "stop": stop, "problems": problems, "saved": state.get("saved")})
        data = reply(blocks, stop)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        # a chunk at a time, split mid-line, as a network would
        for i in range(0, len(data), 37):
            piece = data[i:i + 37]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
        self.wfile.write(b"0\r\n\r\n")

    def log(self, entry):
        with open(LOG, "a") as f:
            f.write(json.dumps(entry) + "\n")


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
