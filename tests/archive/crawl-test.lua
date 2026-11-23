-- Archives the test site (tests/archive/site, served on 127.0.0.1:9445) by
-- crawling it, then reads a page every way a script can.
--   vimb --script tests/archive/crawl-test.lua
local base = os.getenv("VIMB_TEST_SITE") or "http://127.0.0.1:9445/crawl/"
local a = assert(vimb.archive.start{name = "lua crawl test", allow_internal = true})

local summary = assert(a:crawl{seed = base .. "index.html", depth = 2, scope = "host",
                               delay = 0, log = true})
vimb.log("crawl", summary.pages, summary.loaded, summary.failed, summary.left, summary.stopped)

local t, info = a:open(base .. "a.html")
assert(t, info)
vimb.log("open", info.status, info.title, t.url, t.archiving)
vimb.log("text", #t:text(), (t:text():gsub("\n", " ")))
local links = t:links()
vimb.log("links", #links, table.concat(links, " "))
vimb.log("html", (t:html():sub(1, 15)))
vimb.log("source", (t:source():sub(1, 15)))
vimb.log("eval", t:eval("document.querySelectorAll('a').length"))
local scrolled = t:scroll()
vimb.log("scroll", scrolled.screens, scrolled.reached_end)
t:close()

local missing, err = a:open("http://127.0.0.1:9/nothing", {timeout = 10})
vimb.log("unreachable", missing, err)

vimb.log("responses", a.responses, a.state)
local path = assert(a:stop())
vimb.log("saved", path)
