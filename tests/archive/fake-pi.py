#!/usr/bin/env python3
"""Plays Pi (pi.dev) in its RPC mode, for testing vimb's Pi assistant: it
checks how vimb starts it and the extension vimb gives it, then plays the
scripted conversation of mock_assistant.py, asking for each tool call
through an "editor" dialog as the extension would.

vimb runs it as VIMB_PI_COMMAND. It needs VIMB_TEST_SITE and writes what it
finds to $VIMB_FAKE_PI_LOG, a JSON line per step."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mock_assistant import Scenario, is_png  # noqa: E402

LOG = os.environ["VIMB_FAKE_PI_LOG"]
CHANNEL = "vimb-tool"
problems = []


def log(entry):
    with open(LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")


def out(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


# ---- how it was started --------------------------------------------------------

args = sys.argv[1:]
flag = lambda name: args[args.index(name) + 1] if name in args else None
for needed in ("--no-session", "--no-builtin-tools", "--no-extensions", "--no-skills",
               "--no-prompt-templates", "--no-context-files", "--no-approve", "--offline"):
    if needed not in args:
        problems.append(f"started without {needed}")
if flag("--mode") != "rpc":
    problems.append(f"--mode is {flag('--mode')}")
if flag("--thinking") != "medium":
    problems.append(f"--thinking is {flag('--thinking')}")
if not flag("--system-prompt"):
    problems.append("no --system-prompt")
tools = (flag("--tools") or "").split(",")
if "run_javascript" in tools or "crawl" not in tools:
    problems.append(f"--tools is {tools}")
extension = flag("--extension") or ""
source = open(extension).read() if os.path.exists(extension) else ""
if CHANNEL not in source or '"crawl"' not in source or "ctx.ui.editor" not in source:
    problems.append(f"the extension {extension} isn't vimb's")
if os.path.dirname(extension) != os.getcwd():
    problems.append("not run in the extension's directory")

scenario = Scenario("pi", os.environ["VIMB_TEST_SITE"])
next_id = 0


def read():
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    return json.loads(line)


def run_call(call):
    """Asks vimb, as the extension does; the answer is (is_error, text)."""
    global next_id
    _, call_id, name, raw = call
    params = json.loads(raw)
    out({"type": "tool_execution_start", "toolCallId": call_id, "toolName": name, "args": params})
    next_id += 1
    dialog = f"ui-{next_id}"
    out({"type": "extension_ui_request", "id": dialog, "method": "editor", "title": CHANNEL,
         "prefill": json.dumps({"tool": name, "params": params})})
    while True:
        message = read()
        if message.get("type") == "extension_ui_response" and message.get("id") == dialog:
            break
        problems.append(f"while a call waited: {message}")
    if message.get("cancelled"):
        return True, "vimb did not answer."
    answer = json.loads(message["value"])
    if answer.get("image"):
        ok = answer["image"].get("mime") == "image/png" and is_png(answer["image"].get("data", ""))
        return (False, "<png>") if ok else (True, "a bad image")
    return (False, answer["text"]) if answer.get("ok") else (True, answer["error"])


def stream_text(kind, text):
    for i in range(0, len(text), 6):
        out({"type": "message_update", "assistantMessageEvent": {
            "type": "thinking_delta" if kind == "thinking" else "text_delta", "delta": text[i:i + 6]}})


while True:
    message = read()
    if message.get("type") != "prompt":
        problems.append(f"unexpected {message}")
        continue
    out({"type": "response", "command": "prompt", "success": True, "id": message.get("id")})
    if scenario.site not in message.get("message", ""):
        problems.append("the task's URL isn't in the prompt")
    out({"type": "agent_start"})
    turn, results = 0, {}
    while True:
        step_problems = []
        blocks, stop = scenario.step(turn, results, step_problems)
        problems += step_problems
        results = {}
        for block in blocks:
            if block[0] in ("thinking", "text"):
                stream_text(block[0], block[1])
            else:
                results[block[1]] = run_call(block)
        log({"turn": turn, "stop": stop, "problems": problems, "saved": scenario.state.get("saved")})
        problems = []
        turn += 1
        if stop == "end_turn":
            break
    out({"type": "message_end", "message": {"role": "assistant", "stopReason": "stop"}})
    out({"type": "agent_end"})
    out({"type": "agent_settled"})
