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

#ifndef _ARCHIVE_ZIP_H
#define _ARCHIVE_ZIP_H

#include <glib.h>

/* The ZIP a WACZ is. Entries are written stored -- uncompressed -- since a
 * record is read out of an archive inside a WACZ by seeking to it, which a
 * deflated entry doesn't allow; ZIP64 fields are used where a size or offset
 * needs them. */

typedef struct ArchiveZipWriter ArchiveZipWriter;

/* Creates path, replacing any file there. */
ArchiveZipWriter *archive_zip_writer_new(const char *path, GError **error);

/* Adds an entry holding data, and gives back its "sha256:..." digest. */
gboolean archive_zip_add(ArchiveZipWriter *zip, const char *name, const void *data,
        gsize len, char **sha256, GError **error);

/* Adds an entry holding the first length bytes of the file at source. */
gboolean archive_zip_add_file(ArchiveZipWriter *zip, const char *name, const char *source,
        guint64 length, char **sha256, GError **error);

/* Writes the central directory, closes the file and frees the writer. */
gboolean archive_zip_finish(ArchiveZipWriter *zip, GError **error);

/* Closes the file, unfinished, and frees the writer. */
void archive_zip_abandon(ArchiveZipWriter *zip);

typedef struct {
    char    *name;
    guint64 data_offset;        /* where its bytes begin in the file */
    guint64 compressed_size;
    guint64 uncompressed_size;
    gboolean stored;            /* rather than deflated */
} ArchiveZipEntry;

/* Reads a ZIP's directory without unpacking it: an entry is used where it
 * lies, a record being a read at an offset into a stored archive. */
typedef struct ArchiveZipReader ArchiveZipReader;

ArchiveZipReader *archive_zip_reader_open(const char *path, GError **error);
void archive_zip_reader_free(ArchiveZipReader *zip);

const ArchiveZipEntry *archive_zip_find(ArchiveZipReader *zip, const char *name);

/* Every entry, in directory order. */
const GPtrArray *archive_zip_entries(ArchiveZipReader *zip);

/* All of an entry, inflated if it was deflated -- for the small parts, not
 * an archive. NULL if it is longer than max. */
GBytes *archive_zip_read_entry(ArchiveZipReader *zip, const ArchiveZipEntry *entry,
        gsize max, GError **error);

/* len bytes at offset within a stored entry. */
gboolean archive_zip_read_at(ArchiveZipReader *zip, const ArchiveZipEntry *entry,
        guint64 offset, void *buf, gsize len, GError **error);

#endif /* end of include guard: _ARCHIVE_ZIP_H */
