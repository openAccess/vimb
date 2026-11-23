#!/usr/bin/env python3
"""Drives vimb --mcp with the official MCP Python SDK (pip install mcp):
archives the test site (tests/archive/site, served on 127.0.0.1:9445) and
checks what each tool gives back.

    python3 tests/archive/mcp-test.py [path/to/vimb]

vimb needs a display (DISPLAY, or WAYLAND_DISPLAY); set XDG_CONFIG_HOME,
XDG_DATA_HOME and XDG_CACHE_HOME to keep it off your own profile. Exits 0 if
all is well, else 1."""

import asyncio
import base64
import json
import os
import sys

from mcp import Client, MCPError, StdioServerParameters

VIMB = sys.argv[1] if len(sys.argv) > 1 else "src/vimb"
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")
ENV_PASSED = ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME",
              "XDG_DATA_HOME", "XDG_CACHE_HOME", "HOME", "LANG")
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def value(result):
    """A tool result's structured value."""
    if result.is_error:
        raise RuntimeError(result.content[0].text)
    if result.structured_content is not None:
        return result.structured_content
    return json.loads(result.content[0].text)


def server(*extra):
    env = {k: os.environ[k] for k in ENV_PASSED if k in os.environ}
    return StdioServerParameters(command=VIMB, args=["--mcp", "--window-size", "1280x900", *extra],
                                 env=env)


async def full_run():
    async with Client(server()) as client:
        tools = {t.name for t in (await client.list_tools()).tools}
        check("tools listed", "crawl" in tools and "read_page" in tools, ", ".join(sorted(tools)))
        check("no run_javascript without --mcp-allow-eval", "run_javascript" not in tools)

        archive = value(await client.call_tool("start_archive",
                                               {"name": "mcp test", "allow_internal": True}))
        n = archive["archive"]
        check("start_archive", archive["state"] == "recording", json.dumps(archive))

        page = value(await client.call_tool("open_page", {"url": SITE + "index.html",
                                                          "archive": n}))
        tab = page["tab"]
        check("open_page", page.get("status") == 200 and page.get("title"), json.dumps(page))

        text = value(await client.call_tool("read_page", {"tab": tab, "limit": 20}))
        check("read_page text window", len(text["text"]) <= 20 and text["length"] > 0,
              json.dumps(text))
        html = value(await client.call_tool("read_page", {"tab": tab, "format": "html"}))
        check("read_page html", html["text"].lower().startswith("<html") or "<body" in html["text"],
              html["text"][:40])
        source = value(await client.call_tool("read_page", {"tab": tab, "format": "source"}))
        check("read_page source", "<a" in source["text"], source["text"][:40])
        bad = await client.call_tool("read_page", {"tab": tab, "format": "pdf"})
        check("read_page bad format is an error", bad.is_error, bad.content[0].text)

        links = value(await client.call_tool("page_links", {"tab": tab}))["links"]
        check("page_links", len(links) >= 2 and all(l["url"].startswith("http") for l in links),
              json.dumps(links)[:200])

        scrolled = value(await client.call_tool("scroll_page", {"tab": tab}))
        check("scroll_page", scrolled.get("reached_end") is True, json.dumps(scrolled))

        shot = await client.call_tool("screenshot", {"tab": tab})
        image = shot.content[0]
        png = base64.b64decode(image.data) if image.type == "image" else b""
        check("screenshot", image.type == "image" and png[:8] == b"\x89PNG\r\n\x1a\n",
              f"{image.type}, {len(png)} bytes")

        tabs = value(await client.call_tool("list_tabs", {}))["tabs"]
        check("list_tabs", any(t["tab"] == tab and t["archiving"] for t in tabs), json.dumps(tabs))

        crawled = value(await client.call_tool("crawl", {
            "archive": n, "seeds": [SITE + "a.html"], "depth": 1, "scope": "host", "delay": 0}))
        check("crawl", crawled.get("loaded", 0) >= 2 and not crawled.get("failed"),
              json.dumps(crawled))

        refused = await client.call_tool("open_page", {"url": "file:///etc/passwd"})
        check("file: URLs refused", refused.is_error, refused.content[0].text)
        try:
            await client.call_tool("run_javascript", {"tab": tab, "script": "1"})
            check("run_javascript unknown", False)
        except MCPError as e:
            check("run_javascript unknown", True, str(e))
        gone = await client.call_tool("load_page", {"tab": 999, "url": SITE})
        check("no such tab", gone.is_error, gone.content[0].text)

        closed = value(await client.call_tool("close_tab", {"tab": tab}))
        check("close_tab", closed["closed"] is True, json.dumps(closed))

        status = value(await client.call_tool("archive_status", {"archive": n}))
        check("archive_status", status["responses"] > 0, json.dumps(status))

        saved = value(await client.call_tool("stop_archive", {"archive": n}))
        path = saved.get("saved", "")
        check("stop_archive", path.endswith(".wacz") and os.path.exists(path), path)

        # replay what was saved: what it holds comes back, nothing else does
        replay = value(await client.call_tool("replay_archive", {"path": path}))
        check("replay_archive opens the list of pages", replay.get("status") == 200
              and replay.get("title") == "mcp test", json.dumps(replay))
        listed = value(await client.call_tool("page_links", {"tab": replay["tab"]}))["links"]
        check("the list links the pages", any(l["url"] == SITE + "index.html" for l in listed),
              json.dumps(listed)[:200])
        page = value(await client.call_tool("load_page", {"tab": replay["tab"],
                                                          "url": SITE + "c.html"}))
        check("a crawled page replays", page.get("status") == 200 and page.get("title") == "Page C",
              json.dumps(page))
        page = value(await client.call_tool("load_page", {"tab": replay["tab"],
                                                          "url": SITE + "b.html"}))
        check("a page never archived doesn't", page.get("status") == 404, json.dumps(page))
        return path


async def eval_and_domains():
    async with Client(server("--mcp-allow-eval", "--mcp-allow-domains", "127.0.0.1")) as client:
        tools = {t.name for t in (await client.list_tools()).tools}
        check("run_javascript with --mcp-allow-eval", "run_javascript" in tools)
        page = value(await client.call_tool("open_page", {"url": SITE + "index.html"}))
        result = value(await client.call_tool("run_javascript", {
            "tab": page["tab"], "script": "document.querySelectorAll('a').length"}))
        check("run_javascript", isinstance(result["value"], (int, float)) and result["value"] > 0,
              json.dumps(result))
        elsewhere = await client.call_tool("open_page", {"url": "https://example.com/"})
        check("other domains refused", elsewhere.is_error, elsewhere.content[0].text)
        # an archive left recording is saved when the client goes
        archive = value(await client.call_tool("start_archive", {"name": "mcp left open",
                                                                "allow_internal": True}))
        value(await client.call_tool("open_page", {"url": SITE + "b.html",
                                                   "archive": archive["archive"]}))


async def main():
    path = await full_run()
    await eval_and_domains()
    if path:
        left = os.path.join(os.path.dirname(path), "mcp left open.wacz")
        for _ in range(100):
            if os.path.exists(left):
                break
            await asyncio.sleep(0.1)
        check("archive left open is saved on exit", os.path.exists(left), left)
    print("all ok" if not failures else f"{len(failures)} failed: {', '.join(failures)}")
    return 0 if not failures else 1


sys.exit(asyncio.run(main()))
