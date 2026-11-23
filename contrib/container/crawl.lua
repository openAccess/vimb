-- Archives a crawl, for vimb-archive crawl: from the seeds in CRAWL_SEEDS,
-- following links as CRAWL_* say (see vimb-archive help), into one archive.
--   CRAWL_SEEDS="example.com https://example.org/a" vimb --script crawl.lua

local function env(name, default)
    local value = os.getenv(name)
    if value == nil or value == "" then
        return default
    end
    return value
end

local seeds = {}
for seed in env("CRAWL_SEEDS", ""):gmatch("%S+") do
    if not seed:match("^%a[%w+.-]*://") then
        seed = "https://" .. seed       -- example.com, as people write it
    end
    seeds[#seeds + 1] = seed
end
if #seeds == 0 then
    error("say where the crawl starts: a URL, or a site's name")
end

local name = env("CRAWL_NAME", nil)
if not name then
    name = seeds[1]:gsub("^%a[%w+.-]*://", ""):gsub("[/?#].*$", "")
end

local a = assert(vimb.archive.start(name))
local summary, err = a:crawl{
    seeds       = seeds,
    depth       = tonumber(env("CRAWL_DEPTH", "1")),
    scope       = env("CRAWL_SCOPE", "page-links"),
    pattern     = env("CRAWL_PATTERN", nil),
    max_pages   = tonumber(env("CRAWL_MAX_PAGES", "100")),
    max_seconds = tonumber(env("CRAWL_MAX_SECONDS", "0")),
    delay       = tonumber(env("CRAWL_DELAY", "1")),
    scroll      = env("CRAWL_SCROLL", "1") ~= "0",
    log         = true,
}
local path, why = a:stop()
if not summary then
    error("the crawl failed: " .. tostring(err))
end
vimb.log(string.format("crawled %d pages: %d loaded, %d failed, %d left (%s); saved %s",
                       summary.pages, summary.loaded, summary.failed, summary.left,
                       summary.stopped, tostring(path or why)))
if not path then
    error("the archive couldn't be saved: " .. tostring(why))
end
if summary.loaded == 0 then
    error("no page loaded")
end
