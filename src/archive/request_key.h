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

#ifndef _ARCHIVE_REQUEST_KEY_H
#define _ARCHIVE_REQUEST_KEY_H

#include <glib.h>

/* The key an index files a request under: its URL's SURT for a GET; for any
 * other method, the SURT of its URL with the method -- and for a POST or
 * PUT, what it sent -- added to the query:
 *
 *   POST https://example.org/api {"q": "x"}  ->  org,example)/api?__wb_method=post&q=x
 *
 * A URL alone doesn't tell one POST from another (a GraphQL API sends every
 * query to one URL), so the key holds what was sent too. This follows
 * cdxj-indexer, as the Chromium fork's warc::RequestKey() does: a form is
 * decoded into its fields; JSON, or plain text that parses as JSON, into its
 * leaves, named by the key holding them; anything else is base64 in a
 * "__wb_post_data" field; a gzipped body is keyed by what it inflates to.
 * body_len is -1 when the body isn't known, and then only the method is
 * added. JSON leaves are taken in the order they were sent, as cdxj-indexer
 * takes them. NULL for a URL with no host. */
char *archive_request_key(const char *url, const char *method, const char *content_type,
        const guint8 *body, gssize body_len);

/* What a POST or PUT body adds to its key -- "q=x" above -- which an index
 * also records as "requestBody". Cut at 4096 characters, as cdxj-indexer
 * cuts it, unless whole. */
char *archive_request_body_query(const char *content_type, const guint8 *body,
        gsize body_len, gboolean whole);

#endif /* end of include guard: _ARCHIVE_REQUEST_KEY_H */
