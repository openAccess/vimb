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

#ifndef _ARCHIVE_WARC_H
#define _ARCHIVE_WARC_H

#include <glib.h>

#include "spool.h"

/* Writes WARC/1.1 files, one gzip member per record so that a record can be
 * read on its own from its offset -- which is how an index points into a
 * WACZ. Digests are SHA-1 in unpadded base32, as warcprox and the Chromium
 * fork write them. Not thread-safe: one writer thread owns a file. */
typedef struct WarcWriter WarcWriter;

typedef struct {
    const char *type;           /* "response", "request", "metadata", ... */
    const char *target_uri;
    const char *concurrent_to;  /* the record ID of the record this goes with */
    const char *ip_address;
    const char *content_type;   /* of the block */
    const char *payload_digest; /* "sha1:..." when the block has a payload */
    const char *truncated;      /* WARC-Truncated: "length", "time", ... */
    GDateTime  *date;           /* NULL: now */
} WarcRecordInfo;

/* Makes path, which must not exist, and writes its warcinfo record. */
WarcWriter *warc_writer_open(const char *path, const char *software, GError **error);

/* Writes a record whose block is what block holds (NULL: empty). Its record
 * ID, offset and length in the file are given back for indexing. */
gboolean warc_writer_write(WarcWriter *writer, const WarcRecordInfo *info,
        Spool *block, char **record_id, guint64 *offset, guint64 *length,
        GError **error);

/* Flushes and closes the file, and frees the writer. */
gboolean warc_writer_close(WarcWriter *writer, GError **error);

const char *warc_writer_path(const WarcWriter *writer);

/* "<urn:uuid:...>" */
char *warc_new_record_id(void);
/* 2026-11-23T19:27:14.055123Z */
char *warc_format_date(GDateTime *date);
/* "sha1:" and the digest in unpadded base32 */
char *warc_label_sha1(const guint8 digest[20]);

#endif /* end of include guard: _ARCHIVE_WARC_H */
