/**
 * vimb - a webkit based vim like browser.
 *
 * Copyright (C) 2026 vimb contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#ifndef _ARCHIVE_WACZ_H
#define _ARCHIVE_WACZ_H

#include <glib.h>

/* Packaging a capture as a WACZ.
 *
 * A capture is written as loose files -- gzipped WARCs, and a page list
 * beside them -- because that survives being cut off: each record is whole
 * once written, where a ZIP is unreadable until its directory is written at
 * the end. When the capture is over, the files are packaged into the one
 * file a web archive is passed around as.
 *
 * The WACZ is laid out as `wacz create` lays one out (WACZ 1.1.1), as the
 * Chromium fork's warc::PackageCapture() lays it out: the WARCs stored,
 * unchanged, under "archive/"; a CDXJ index of them in compressed blocks,
 * "indexes/index.cdx.gz", with their own index "indexes/index.idx"; the page
 * list at "pages/pages.jsonl"; and "datapackage.json", giving the SHA-256 of
 * each, with its own in "datapackage-digest.json".
 *
 * The page list is the capture's pages.jsonl when it has one. Otherwise the
 * pages are the top-level page loads the archive itself records (see
 * indexer.h), untitled -- which is what a capture cut off before it wrote
 * its list still has. */

/* Packages the *.warc.gz files and pages.jsonl in dir as the WACZ at
 * wacz_path, which is written under a temporary name, read back -- every
 * archive byte for byte, and the index -- and only then put in place. A WARC
 * that ends partway through a record, as one cut off by a crash does, is
 * packaged up to its last whole record.
 *
 * title, when not NULL or empty, is what the person archiving called it.
 * With remove_sources, the WARCs and page list are deleted once the WACZ is
 * in place. */
gboolean archive_wacz_package(const char *dir, const char *wacz_path, const char *software,
        const char *title, gboolean remove_sources, GError **error);

/* The name of the page list in a capture directory. */
#define ARCHIVE_PAGES_FILE "pages.jsonl"

/* The first line of every page list. */
#define ARCHIVE_PAGES_HEADER \
    "{\"format\": \"json-pages-1.0\", \"id\": \"pages\", \"title\": \"All Pages\"}"

/* One page-list line for a page: id, url, ts (ISO 8601) and title, when it
 * has one. */
char *archive_page_line(const char *url, const char *ts, const char *title);

#endif /* end of include guard: _ARCHIVE_WACZ_H */
