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

#ifndef _ARCHIVE_SURT_H
#define _ARCHIVE_SURT_H

#include <glib.h>

/* The SURT (Sort-friendly URI Reordering Transform) key a web archive index
 * files url under:
 *
 *   https://www.Example.org:443/A/b/?b=2&a=1#frag  ->  org,example)/a/b?a=1&b=2
 *
 * With the host reversed, a site's pages sort together and its subdomains
 * beneath it, so a lookup is a search in a sorted file. What matters most is
 * being the transform everyone else uses -- an index is written by one tool
 * and read by another -- so this follows the Python surt library that
 * cdxj-indexer and pywb use, as the Chromium fork's warc::ToSurt() does:
 * the scheme, userinfo, fragment and a "www."/"www2." prefix go; the host is
 * lowercased and reversed; a port the scheme doesn't imply stays; the path
 * and query are decoded and lowercased, the path loses a trailing slash and
 * empty segments, and the query is sorted. Only what can't be written plainly
 * is escaped again.
 *
 * NULL for a URL with no host. */
char *archive_surt(const char *url);

#endif /* end of include guard: _ARCHIVE_SURT_H */
