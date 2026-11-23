#!/usr/bin/env python3
"""Archives a page of the test site (tests/archive/site, served on
127.0.0.1:9445) with contrib/python/vimb.py.

    python3 tests/archive/vimb-py-test.py [path/to/vimb]

vimb needs a display. Exits 0 if all is well."""

import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "contrib", "python"))
from vimb import Vimb, VimbError  # noqa: E402

VIMB = sys.argv[1] if len(sys.argv) > 1 else "src/vimb"
SITE = os.environ.get("VIMB_TEST_SITE", "http://127.0.0.1:9445/crawl/")

with Vimb(command=[VIMB], window_size=(1280, 900), allow_eval=True) as v:
    print("server", v.server_info["serverInfo"])
    assert "run_javascript" in dir(v)
    archive = v.start_archive(name="vimb.py test", allow_internal=True)["archive"]
    page = v.open_page(url=SITE + "d.html", archive=archive)
    tab = page["tab"]
    print("opened", page)
    print("scrolled", v.scroll_page(tab=tab))
    print("images", v.run_javascript(tab=tab, script="document.images.length")["value"])
    png = v.screenshot(tab=tab)
    assert png[:4] == b"\x89PNG", png[:8]
    print("screenshot", len(png), "bytes")
    try:
        v.load_page(tab=tab, url="javascript:alert(1)")
        raise AssertionError("javascript: URL loaded")
    except VimbError as e:
        print("refused", e)
    saved = v.stop_archive(archive=archive)["saved"]
    print("saved", saved)

with zipfile.ZipFile(saved) as z:
    names = z.namelist()
    index = z.read("indexes/index.cdx.gz")
assert "datapackage.json" in names and "pages/pages.jsonl" in names, names
import gzip  # noqa: E402
urls = [line.split(b" ", 2)[0] for line in gzip.decompress(index).splitlines()]
print("indexed", [u.decode() for u in urls])
assert any(u.endswith(b"/crawl/lazy.png") for u in urls), "the lazy image wasn't archived"
print("all ok")
