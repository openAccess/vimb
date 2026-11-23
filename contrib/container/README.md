# vimb in a container

An image that runs vimb with nobody at the keyboard, to archive the web as
WACZ files:

- **An assistant's task**: "archive nytimes.com and the articles linked from
  its front page"
- **A crawl**: from some URLs, following their links
- **A Lua script** (see Scripting in vimb(1))
- **MCP over stdio**: so an MCP client can drive vimb in the container

The container has its own virtual display (Xvfb), fonts and media codecs.
It's also the way to run vimb on a Mac: WebKitGTK doesn't run there, but
Podman and Docker Desktop do.

## Building

From the top of the vimb tree:

```sh
podman build -t vimb-archive -f contrib/container/Containerfile .
```

`docker build` takes the same arguments.

## Running

Archives go to `/out` in the container, so mount a directory there. Each run
also writes `summary.json`, which says how the run went: its status, the
archives it saved, and, for a task, what the assistant said. The exit status
is 0 if all went well, 1 if not, and 2 if `TIMEOUT` ran out first.

```sh
mkdir -p out

# an assistant's task (Claude; GEMINI_API_KEY and -e ASSISTANT=gemini for Gemini)
podman run --rm -v ./out:/out:Z -e ANTHROPIC_API_KEY \
    vimb-archive task "Archive nytimes.com and the articles linked from its front page"

# a crawl: the URLs and the links on them
podman run --rm -v ./out:/out:Z vimb-archive crawl example.com

# the same site, three links deep, at most 500 pages, in under an hour
podman run --rm -v ./out:/out:Z -e CRAWL_SCOPE=site -e CRAWL_DEPTH=3 \
    -e CRAWL_MAX_PAGES=500 -e TIMEOUT=3600 vimb-archive crawl example.com

# a Lua script of your own
podman run --rm -v ./out:/out:Z -v ./crawl.lua:/in/crawl.lua:ro,Z \
    vimb-archive script /in/crawl.lua

# MCP: run the container as the MCP server, with -i
podman run -i --rm -v ./out:/out:Z vimb-archive mcp
```

`vimb-archive help` lists the modes and the environment variables:

| Variable | What it does |
| --- | --- |
| `ASSISTANT` | Which model the assistant uses: `claude` (the default) or `gemini`. |
| `SCRIPTS=1` | Lets the assistant run the Lua scripts it writes. They're sandboxed, but nobody approves them. |
| `TIMEOUT` | Stops the run after this many seconds, saving what was archived. |
| `STRICT_SSL=0` | Archives sites whatever their certificates. |
| `ALLOW_INTERNAL=1` | Lets archives reach private and loopback addresses. They're refused otherwise, so that a page can't pull the container's network into an archive. |
| `CRAWL_SCOPE`, `CRAWL_DEPTH` and the other `CRAWL_*` variables | Shape a crawl. |

`podman stop` (or `docker stop`) ends a run early. Archives are saved first,
so give it time: `-t 60`.

## Notes

- **Your user ID.** With rootless Podman, files written to `/out` belong to
  you. With Docker, pass `--user "$(id -u):$(id -g)"` for the same.
- **WebKit's sandbox.** WebKit sandboxes its web processes with bubblewrap,
  which needs user namespaces that a container usually doesn't allow. WebKit
  notices, says so ("Bubblewrap does not work inside of this container"), and
  runs without it. The container is then the only sandbox around web content.
- **Reaching the host.** To reach a server running on the host itself (say,
  for testing), use `--network host`. That shares more than the network: the
  host's X displays become reachable too (their abstract sockets go with the
  network), so it's for testing only.
