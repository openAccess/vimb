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

#ifndef _ARCHIVE_INDEXER_H
#define _ARCHIVE_INDEXER_H

#include <glib.h>

/* Indexing an archive: reading it once, front to back, to say where each
 * record is, so a replay can go straight to the one it wants. It writes what
 * cdxj-indexer writes for a WACZ, as the Chromium fork's warc::IndexWarc()
 * does, so an archive vimb packages is indexed as `wacz create` would index
 * it:
 *
 *   org,example)/a 20260910201354 {"url": "...", "offset": "594", ...}
 *
 * Responses, revisits, and resources and metadata that aren't a capture's
 * description of itself get a line; a request is reached through its
 * response, which takes from it the method and body (for a POST or PUT, in
 * its key -- see request_key.h) and the referrer and cookie. */

typedef struct {
    char *url;
    char *date;                 /* WARC-Date, ISO 8601 */
} ArchiveNavigation;

typedef struct {
    GPtrArray *lines;           /* of char *, in file order, unsorted */
    GPtrArray *navigations;     /* of ArchiveNavigation *: top-level page loads */
    guint64   complete_length;  /* how much of the file is whole records */
} ArchiveWarcIndex;

ArchiveWarcIndex *archive_warc_index_new(void);
void archive_warc_index_free(ArchiveWarcIndex *index);

/* Indexes the gzipped WARC at path, which the index will name filename,
 * adding to index. Each record must be a gzip member of its own; nothing
 * past the last whole one is indexed. A navigation is a response to a
 * request a browser made for a top-level document (Sec-Fetch-Dest: document,
 * Sec-Fetch-Mode: navigate) that succeeded. */
gboolean archive_index_warc(const char *path, const char *filename,
        ArchiveWarcIndex *index, GError **error);

/* The first 14 digits of a WARC date: "2026-09-10T20:13:54Z" ->
 * "20260910201354". */
char *archive_index_timestamp(const char *date);

#endif /* end of include guard: _ARCHIVE_INDEXER_H */
