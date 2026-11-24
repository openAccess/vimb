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

#ifndef _ARCHIVE_RECORDER_H
#define _ARCHIVE_RECORDER_H

#include <glib.h>

#include "spool.h"

/* One HTTP exchange as the proxy saw it: the request it sent the server and
 * the response exactly as the server sent it back. */
typedef struct {
    char      *target_uri;
    char      *ip_address;
    GDateTime *date;            /* when the request was sent */
    Spool     *request;         /* head as sent upstream, then the body */
    Spool     *response;        /* every byte of the response, as received */
    char      *payload_digest;  /* "sha1:..." of the body, coding removed */
    char      *truncated;       /* WARC-Truncated, when the response was cut */
    guint64   head_length;      /* of the response's head, in response */
    guint64   payload_length;   /* of its body, coding removed */
    /* Not an exchange: a record of this type alone (a "resource" -- a
     * websocket's messages), its block in response, of content_type. */
    char      *record_type;
    char      *content_type;
} ArchiveExchange;

ArchiveExchange *archive_exchange_new(void);
void archive_exchange_free(ArchiveExchange *exchange);

/* Writes exchanges to a WARC file from a thread of its own, in the order they
 * are handed in, as a response record and a request record each -- or, for a
 * response whose payload an earlier one already holds, a revisit record in
 * its place: the response's head, and where the payload is (WARC 1.1's
 * identical-payload-digest profile). A browser fetches one file under many
 * URLs, and a crawl the same files from page after page. */
typedef struct ArchiveRecorder ArchiveRecorder;

ArchiveRecorder *archive_recorder_start(const char *warc_path, const char *software,
        GError **error);

/* Takes the exchange to write. Any thread. */
void archive_recorder_submit(ArchiveRecorder *recorder, ArchiveExchange *exchange);

/* How many exchanges have been written so far. Any thread. */
guint archive_recorder_count(ArchiveRecorder *recorder);

/* Writes what is queued, closes the file, and frees the recorder, giving
 * back how many exchanges it wrote in all. Reports the first error any write
 * met. */
gboolean archive_recorder_finish(ArchiveRecorder *recorder, guint *written, GError **error);

#endif /* end of include guard: _ARCHIVE_RECORDER_H */
