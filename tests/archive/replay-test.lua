-- Archives part of the test site (tests/archive/site, served on
-- 127.0.0.1:9445), then replays it: what was archived comes back, what
-- wasn't -- even what the live site would serve -- doesn't.
--   vimb --window-size 1280x900 -C "set archive-allow-internal=true" \
--        --script tests/archive/replay-test.lua
local base = os.getenv("VIMB_TEST_SITE") or "http://127.0.0.1:9445/crawl/"
local failures = 0
local function check(what, ok, detail)
    vimb.log((ok and "ok   " or "FAIL ") .. what .. (detail and (": " .. tostring(detail)) or ""))
    if not ok then failures = failures + 1 end
end

-- the index and the pages it links to; c.html, two links away, isn't archived
local a = assert(vimb.archive.start{name = "replay source"})
local crawled = assert(a:crawl{seed = base .. "index.html", depth = 1, scope = "host", delay = 0})
-- a page loading one stylesheet by two names: the second is a revisit
local dup = assert(a:open(base .. "dup.html"))
dup:close()
-- a page that writes the time it loaded at, by its own clock
local clock = assert(a:open(base .. "clock.html"))
local captured_at = tonumber(clock:eval("document.getElementById('now').textContent"))
clock:close()
local path = assert(a:stop())
vimb.sleep(6)                       -- so that replay's time and the capture's differ
check("archived", crawled.loaded == 3, crawled.loaded .. " pages, " .. path)

-- replay begins at the list of what the archive holds
local t, info = vimb.replay(path)
check("replay opens", t and info and info.status == 200, info and info.title)
check("a replay tab says so", t and t.archiving)
local listed = t and t:links() or {}
check("the list links the pages", #listed == 5, table.concat(listed, " "))

-- archived pages come back
info = t:load(base .. "index.html")
check("an archived page", info and info.status == 200 and info.title == "Crawl index",
      info and (info.status .. " " .. tostring(info.title)))
check("its text", t:text():find("Crawl test") ~= nil)
info = t:load(base .. "b.html")
check("another archived page", info and info.status == 200 and info.title == "Page B",
      info and info.status)

-- what wasn't archived doesn't, though the live site has it
info = t:load(base .. "c.html")
check("an unarchived page isn't fetched", info and info.status == 404
      and info.title == "Not in the archive", info and (info.status .. " " .. tostring(info.title)))
check("it says why", t:text():find("Nothing was archived at this address") ~= nil,
      (t:text():gsub("\n", " ")))
info = t:load("http://other.example/")
check("an unarchived site", info and info.status == 404, info and info.status)
check("it says why", t:text():find("from this site") ~= nil)

-- a tab opened from a replay tab replays too: a link followed stays in it
info = t:load(base .. "index.html")
t:eval("document.querySelector('a[href=\"a.html\"]').click()")
vimb.sleep(1.5)
check("a link followed is replayed", t.title == "Page A" and t.archiving, t.title)

-- a revisit replays with the payload it refers to
info = t:load(base .. "big-copy.css")
check("a revisit replays", info and info.status == 200
      and t:text():find("the same stylesheet, by two names") ~= nil, info and info.status)

-- a replayed page's clock reads as the capture's did, not as the real one
info = t:load(base .. "clock.html")
local replayed_at = tonumber(t:eval("document.getElementById('now').textContent"))
local origin_at = tonumber(t:eval("document.getElementById('origin').textContent"))
local real_now = tonumber(t:eval("Date.now()"))      -- vimb's own world keeps the real clock
check("a replayed page reads the capture's clock", replayed_at and captured_at
      and math.abs(replayed_at - captured_at) < 3000 and real_now - replayed_at > 4000,
      string.format("captured %s, replayed %s, really %s", captured_at, replayed_at, real_now))
check("and so do performance's times", origin_at and math.abs(origin_at - replayed_at) < 1000,
      origin_at)

-- the same archive opened again shares the replay
local t2, info2 = vimb.replay(path, base .. "a.html")
check("a second tab", t2 and info2 and info2.status == 200 and info2.title == "Page A",
      info2 and info2.status)

-- not a WACZ
local none, err = vimb.replay("/etc/hostname")
check("not a WACZ is refused", none == nil and err ~= nil, err)

t:close()
t2:close()
vimb.log(failures == 0 and "all ok" or (failures .. " failed"))
if failures > 0 then error(failures .. " checks failed") end
