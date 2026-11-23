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

#ifndef _ARCHIVE_SPOOL_H
#define _ARCHIVE_SPOOL_H

#include <glib.h>

/* Bytes held in memory up to a limit, and in an unlinked temporary file once
 * they outgrow it: a response body is recorded whole before its WARC record
 * can be written, since the record's header gives its length, and a video
 * should not have to fit in memory for that.
 *
 * One thread appends; reading starts once appending is done. */
typedef struct Spool Spool;

/* A spool that keeps up to memory_limit bytes in memory, and the rest in a
 * file made in tmp_dir. */
Spool *spool_new(const char *tmp_dir, gsize memory_limit);
void spool_free(Spool *spool);

gboolean spool_append(Spool *spool, const void *data, gsize len, GError **error);
guint64 spool_length(const Spool *spool);

/* Calls func with the content in order, a piece at a time, until it is all
 * given or func returns FALSE. */
typedef gboolean (*SpoolFunc)(const guint8 *data, gsize len, gpointer user_data,
        GError **error);
gboolean spool_foreach(Spool *spool, SpoolFunc func, gpointer user_data,
        GError **error);

/* The content as one string, when it is small enough to be wanted that way;
 * NULL when it is longer than max. */
GBytes *spool_get_bytes(Spool *spool, gsize max, GError **error);

#endif /* end of include guard: _ARCHIVE_SPOOL_H */
