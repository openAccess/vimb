"""Drive vimb from Python: browse, read pages and archive them as WACZ.

A thin client for vimb's MCP server (vimb --mcp), with nothing to install but
vimb itself -- it uses only Python's standard library::

    from vimb import Vimb

    with Vimb() as v:
        archive = v.start_archive(name="example")["archive"]
        tab = v.open_page(url="https://example.com/", archive=archive)["tab"]
        v.scroll_page(tab=tab)
        print(v.read_page(tab=tab)["text"])
        print(v.stop_archive(archive=archive)["saved"])

Each of vimb's tools is a method, taking the tool's arguments by name and
returning its result as a dict (see v.tools() or "MCP" in vimb(1)); a tool
that fails raises VimbError. screenshot() returns PNG bytes. Leaving the with
block (or close()) ends vimb, saving any archive still recording.

vimb needs a display. Without one, run it under Xvfb, e.g.
Vimb(command=["xvfb-run", "-a", "vimb"]).
"""

import base64
import json
import subprocess

__all__ = ["Vimb", "VimbError"]


class VimbError(Exception):
    """A tool failed, or vimb did."""


class Vimb:
    def __init__(self, command=("vimb",), allow_eval=False, allow_domains=None,
                 window_size=None, args=(), stderr=None):
        """Starts vimb --mcp.

        command: how to run vimb, as a list.
        allow_eval: allow run_javascript.
        allow_domains: the only sites that may be opened (a list).
        window_size: (width, height).
        args: further arguments for vimb.
        stderr: where vimb's messages go (default: this process's stderr).
        """
        argv = list(command) + ["--mcp"]
        if allow_eval:
            argv.append("--mcp-allow-eval")
        if allow_domains:
            argv += ["--mcp-allow-domains", ",".join(allow_domains)]
        if window_size:
            argv += ["--window-size", "%dx%d" % tuple(window_size)]
        argv += list(args)
        self._process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=stderr)
        self._next_id = 0
        self._tools = None
        self.server_info = self._request("initialize", {
            "protocolVersion": "2025-11-25",
            "capabilities": {},
            "clientInfo": {"name": "vimb.py", "version": "1"},
        })
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    # ---- the protocol ---------------------------------------------------

    def _send(self, message):
        try:
            self._process.stdin.write(json.dumps(message).encode() + b"\n")
            self._process.stdin.flush()
        except (BrokenPipeError, ValueError) as e:
            raise VimbError("vimb has quit") from e

    def _request(self, method, params):
        self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": self._next_id, "method": method, "params": params})
        while True:
            line = self._process.stdout.readline()
            if not line:
                raise VimbError("vimb has quit (exit status %s)" % self._process.wait())
            message = json.loads(line)
            if message.get("id") != self._next_id or "method" in message:
                continue
            if "error" in message:
                raise VimbError(message["error"].get("message", "error"))
            return message["result"]

    def call(self, tool, /, **arguments):
        """Runs a tool and returns its result: a dict, or PNG bytes for a
        screenshot."""
        result = self._request("tools/call", {"name": tool, "arguments": arguments})
        content = result.get("content") or [{}]
        if result.get("isError"):
            raise VimbError(content[0].get("text", "%s failed" % tool))
        if content[0].get("type") == "image":
            return base64.b64decode(content[0]["data"])
        if "structuredContent" in result:
            return result["structuredContent"]
        return json.loads(content[0].get("text", "null"))

    def tools(self):
        """The tools vimb offers: a list of {name, description, inputSchema}."""
        if self._tools is None:
            self._tools = self._request("tools/list", {})["tools"]
        return self._tools

    def __getattr__(self, name):
        if name.startswith("_") or name not in {t["name"] for t in self.tools()}:
            raise AttributeError(name)
        return lambda **arguments: self.call(name, **arguments)

    def __dir__(self):
        return list(super().__dir__()) + [t["name"] for t in self.tools()]

    # ---- ending ----------------------------------------------------------

    def close(self, timeout=60):
        """Ends vimb -- archives still recording are saved first -- and
        returns its exit status."""
        if self._process.stdin and not self._process.stdin.closed:
            self._process.stdin.close()
        try:
            return self._process.wait(timeout)
        except subprocess.TimeoutExpired:
            self._process.terminate()
            return self._process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
