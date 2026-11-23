-- Archives a list of pages, read to their ends, then crawls a little of one
-- site. URLs from $VIMB_TEST_URLS (space separated), or a default list.
--   vimb --window-size 1280x900 --script tests/archive/list-test.lua
local urls = {}
for url in (os.getenv("VIMB_TEST_URLS")
            or "https://example.com/ https://en.wikipedia.org/wiki/Web_archiving"):gmatch("%S+") do
  urls[#urls + 1] = url
end

local a = assert(vimb.archive.start("list test"))
local tab
for _, url in ipairs(urls) do
  local info, err
  if tab then
    info, err = tab:load(url)
  else
    tab, info = a:open(url)
    if not tab then err = info end
  end
  if info then
    local scrolled = tab:scroll()
    vimb.log("page", info.status, url, info.title, scrolled.screens .. " screens")
  else
    vimb.log("failed", url, err)
  end
end
if tab then tab:close() end

local crawl_seed = os.getenv("VIMB_TEST_CRAWL") or "https://www.gnu.org/"
local summary = a:crawl{seed = crawl_seed, scope = "site", depth = 1, max_pages = 4,
                        delay = 1, log = true}
vimb.log("crawl", summary.pages, summary.loaded, summary.failed, summary.left, summary.stopped)
vimb.log("saved", assert(a:stop()))
