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

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "zip.h"

#define COPY_SIZE     (1024 * 1024)
#define MAX32         G_GUINT64_CONSTANT(0xFFFFFFFF)
#define ZIP64_EXTRA   0x0001
#define MADE_BY       ((3 << 8) | 45)     /* Unix, 4.5 */
#define UTF8_NAMES    (1 << 11)
#define FILE_MODE     0100644

typedef struct {
    char    *name;
    guint64 size;
    guint64 header_offset;
    guint32 crc;
} WrittenEntry;

struct ArchiveZipWriter {
    char      *path;
    int       fd;
    guint64   offset;
    guint16   dos_time;
    guint16   dos_date;
    GArray    *entries;     /* of WrittenEntry */
};

/* ---- little-endian bytes ---- */

static void put16(GByteArray *b, guint16 v)
{
    guint8 x[2] = {v & 0xff, v >> 8};
    g_byte_array_append(b, x, 2);
}

static void put32(GByteArray *b, guint32 v)
{
    guint8 x[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
    g_byte_array_append(b, x, 4);
}

static void put64(GByteArray *b, guint64 v)
{
    put32(b, (guint32)(v & 0xffffffff));
    put32(b, (guint32)(v >> 32));
}

static guint16 get16(const guint8 *p)
{
    return (guint16)(p[0] | (p[1] << 8));
}

static guint32 get32(const guint8 *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
}

static guint64 get64(const guint8 *p)
{
    return (guint64)get32(p) | ((guint64)get32(p + 4) << 32);
}

/* ---- writing ---- */

static gboolean pwrite_all(int fd, const void *data, gsize len, guint64 offset,
        const char *path, GError **error)
{
    const guint8 *p = data;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not write %s: %s", path, g_strerror(errno));
            return FALSE;
        }
        p      += n;
        len    -= (gsize)n;
        offset += (guint64)n;
    }
    return TRUE;
}

static gboolean zip_write(ArchiveZipWriter *zip, const void *data, gsize len, GError **error)
{
    if (!pwrite_all(zip->fd, data, len, zip->offset, zip->path, error)) {
        return FALSE;
    }
    zip->offset += len;
    return TRUE;
}

static guint16 needed_version(guint64 size)
{
    return size >= MAX32 ? 45 : 20;
}

static void entry_clear(gpointer data)
{
    g_free(((WrittenEntry *)data)->name);
}

ArchiveZipWriter *archive_zip_writer_new(const char *path, GError **error)
{
    int fd = g_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not make %s: %s", path, g_strerror(errno));
        return NULL;
    }
    ArchiveZipWriter *zip = g_new0(ArchiveZipWriter, 1);
    zip->path    = g_strdup(path);
    zip->fd      = fd;
    zip->entries = g_array_new(FALSE, TRUE, sizeof(WrittenEntry));
    g_array_set_clear_func(zip->entries, entry_clear);

    GDateTime *now = g_date_time_new_now_local();
    zip->dos_time = (guint16)((g_date_time_get_hour(now) << 11)
            | (g_date_time_get_minute(now) << 5) | (g_date_time_get_second(now) / 2));
    zip->dos_date = (guint16)(((g_date_time_get_year(now) - 1980) << 9)
            | (g_date_time_get_month(now) << 5) | g_date_time_get_day_of_month(now));
    g_date_time_unref(now);
    return zip;
}

static gboolean write_local_header(ArchiveZipWriter *zip, const char *name, guint64 size,
        guint32 crc, GError **error)
{
    WrittenEntry entry = {g_strdup(name), size, zip->offset, crc};
    g_array_append_val(zip->entries, entry);

    gboolean large = size >= MAX32;
    GByteArray *h  = g_byte_array_new();
    put32(h, 0x04034b50);
    put16(h, needed_version(size));
    put16(h, UTF8_NAMES);
    put16(h, 0);                            /* stored */
    put16(h, zip->dos_time);
    put16(h, zip->dos_date);
    put32(h, crc);
    put32(h, large ? (guint32)MAX32 : (guint32)size);
    put32(h, large ? (guint32)MAX32 : (guint32)size);
    put16(h, (guint16)strlen(name));
    put16(h, large ? 20 : 0);
    g_byte_array_append(h, (const guint8 *)name, (guint)strlen(name));
    if (large) {
        put16(h, ZIP64_EXTRA);
        put16(h, 16);
        put64(h, size);
        put64(h, size);
    }
    gboolean ok = zip_write(zip, h->data, h->len, error);
    g_byte_array_free(h, TRUE);
    return ok;
}

gboolean archive_zip_add(ArchiveZipWriter *zip, const char *name, const void *data,
        gsize len, char **sha256, GError **error)
{
    guint32 crc = (guint32)crc32_z(crc32_z(0, NULL, 0), data, len);
    if (!write_local_header(zip, name, len, crc, error) || !zip_write(zip, data, len, error)) {
        return FALSE;
    }
    if (sha256) {
        char *hex = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
        *sha256   = g_strconcat("sha256:", hex, NULL);
        g_free(hex);
    }
    return TRUE;
}

gboolean archive_zip_add_file(ArchiveZipWriter *zip, const char *name, const char *source,
        guint64 length, char **sha256, GError **error)
{
    int in = g_open(source, O_RDONLY, 0);
    if (in < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not open %s: %s", source, g_strerror(errno));
        return FALSE;
    }
    guint64 header_offset = zip->offset;
    if (!write_local_header(zip, name, length, 0, error)) {
        close(in);
        return FALSE;
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    uLong crc      = crc32_z(0, NULL, 0);
    guint8 *buf    = g_malloc(COPY_SIZE);
    gboolean ok    = TRUE;
    for (guint64 copied = 0; copied < length && ok;) {
        gsize want = (gsize)MIN((guint64)COPY_SIZE, length - copied);
        ssize_t n  = pread(in, buf, want, (off_t)copied);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(n < 0 ? errno : EIO),
                    "Could not read %s: %s", source, n < 0 ? g_strerror(errno) : "it is short");
            ok = FALSE;
            break;
        }
        ok = zip_write(zip, buf, (gsize)n, error);
        g_checksum_update(sum, buf, n);
        crc     = crc32_z(crc, buf, (gsize)n);
        copied += (guint64)n;
    }
    g_free(buf);
    close(in);
    if (ok) {
        /* The CRC wasn't known when the header went out; it goes in now. */
        guint8 le[4] = {crc & 0xff, (crc >> 8) & 0xff, (crc >> 16) & 0xff, (crc >> 24) & 0xff};
        g_array_index(zip->entries, WrittenEntry, zip->entries->len - 1).crc = (guint32)crc;
        ok = pwrite_all(zip->fd, le, 4, header_offset + 14, zip->path, error);
    }
    if (ok && sha256) {
        *sha256 = g_strconcat("sha256:", g_checksum_get_string(sum), NULL);
    }
    g_checksum_free(sum);
    return ok;
}

gboolean archive_zip_finish(ArchiveZipWriter *zip, GError **error)
{
    guint64 directory_offset = zip->offset;
    GByteArray *d = g_byte_array_new();

    for (guint i = 0; i < zip->entries->len; i++) {
        WrittenEntry *e      = &g_array_index(zip->entries, WrittenEntry, i);
        gboolean large_size   = e->size >= MAX32;
        gboolean large_offset = e->header_offset >= MAX32;
        GByteArray *extra     = g_byte_array_new();
        if (large_size || large_offset) {
            put16(extra, ZIP64_EXTRA);
            put16(extra, (guint16)((large_size ? 16 : 0) + (large_offset ? 8 : 0)));
            if (large_size) {
                put64(extra, e->size);
                put64(extra, e->size);
            }
            if (large_offset) {
                put64(extra, e->header_offset);
            }
        }
        put32(d, 0x02014b50);
        put16(d, MADE_BY);
        put16(d, needed_version(e->size));
        put16(d, UTF8_NAMES);
        put16(d, 0);                        /* stored */
        put16(d, zip->dos_time);
        put16(d, zip->dos_date);
        put32(d, e->crc);
        put32(d, large_size ? (guint32)MAX32 : (guint32)e->size);
        put32(d, large_size ? (guint32)MAX32 : (guint32)e->size);
        put16(d, (guint16)strlen(e->name));
        put16(d, (guint16)extra->len);
        put16(d, 0);                        /* comment */
        put16(d, 0);                        /* disk */
        put16(d, 0);                        /* internal attributes */
        put32(d, (guint32)FILE_MODE << 16);
        put32(d, large_offset ? (guint32)MAX32 : (guint32)e->header_offset);
        g_byte_array_append(d, (const guint8 *)e->name, (guint)strlen(e->name));
        g_byte_array_append(d, extra->data, extra->len);
        g_byte_array_free(extra, TRUE);
    }
    gboolean ok = zip_write(zip, d->data, d->len, error);
    guint64 directory_size = d->len;
    guint64 count          = zip->entries->len;
    g_byte_array_free(d, TRUE);

    GByteArray *end = g_byte_array_new();
    if (count >= 0xFFFF || directory_offset >= MAX32 || directory_size >= MAX32) {
        guint64 zip64_end = zip->offset;
        put32(end, 0x06064b50);
        put64(end, 44);
        put16(end, MADE_BY);
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, count);
        put64(end, count);
        put64(end, directory_size);
        put64(end, directory_offset);
        put32(end, 0x07064b50);
        put32(end, 0);
        put64(end, zip64_end);
        put32(end, 1);
    }
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, (guint16)MIN(count, 0xFFFF));
    put16(end, (guint16)MIN(count, 0xFFFF));
    put32(end, (guint32)MIN(directory_size, MAX32));
    put32(end, (guint32)MIN(directory_offset, MAX32));
    put16(end, 0);
    ok = ok && zip_write(zip, end->data, end->len, error);
    g_byte_array_free(end, TRUE);

    if (ok && (fsync(zip->fd) < 0 || close(zip->fd) < 0)) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not finish %s: %s", zip->path, g_strerror(errno));
        ok = FALSE;
    } else if (!ok) {
        close(zip->fd);
    }
    g_array_free(zip->entries, TRUE);
    g_free(zip->path);
    g_free(zip);
    return ok;
}

void archive_zip_abandon(ArchiveZipWriter *zip)
{
    close(zip->fd);
    g_array_free(zip->entries, TRUE);
    g_free(zip->path);
    g_free(zip);
}

/* ---- reading ---- */

struct ArchiveZipReader {
    char       *path;
    int        fd;
    guint64    size;
    GPtrArray  *entries;
    GHashTable *by_name;
};

static gboolean pread_all(ArchiveZipReader *zip, void *buf, gsize len, guint64 offset,
        GError **error)
{
    if (offset > zip->size || len > zip->size - offset) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s: a read runs past the end of the file", zip->path);
        return FALSE;
    }
    guint8 *p = buf;
    while (len) {
        ssize_t n = pread(zip->fd, p, len, (off_t)offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(n < 0 ? errno : EIO),
                    "Could not read %s", zip->path);
            return FALSE;
        }
        p      += n;
        len    -= (gsize)n;
        offset += (guint64)n;
    }
    return TRUE;
}

static void zip_entry_free(gpointer data)
{
    ArchiveZipEntry *e = data;
    g_free(e->name);
    g_free(e);
}

static gboolean fail(GError **error, const char *path, const char *what)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not a ZIP this reads: %s",
            path, what);
    return FALSE;
}

static gboolean read_directory(ArchiveZipReader *zip, GError **error)
{
    /* The end record is in the last 64 KiB and 22 bytes: the comment it may
     * be followed by is at most 65535 bytes. */
    guint64 tail_len = MIN(zip->size, (guint64)(65535 + 22));
    guint8 *tail     = g_malloc(tail_len);
    gboolean ok      = FALSE;
    if (!pread_all(zip, tail, tail_len, zip->size - tail_len, error)) {
        goto out;
    }
    gssize at = -1;
    for (gssize i = (gssize)tail_len - 22; i >= 0; i--) {
        if (get32(tail + i) == 0x06054b50) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        fail(error, zip->path, "no end of central directory");
        goto out;
    }
    guint64 end_offset = zip->size - tail_len + (guint64)at;
    guint64 count      = get16(tail + at + 10);
    guint64 dir_size   = get32(tail + at + 12);
    guint64 dir_offset = get32(tail + at + 16);

    if (count == 0xFFFF || dir_size == MAX32 || dir_offset == MAX32) {
        guint8 locator[20], end64[56];
        if (end_offset < 20 || !pread_all(zip, locator, 20, end_offset - 20, error)
                || get32(locator) != 0x07064b50
                || !pread_all(zip, end64, 56, get64(locator + 8), error)
                || get32(end64) != 0x06064b50) {
            if (error && !*error) {
                fail(error, zip->path, "no ZIP64 end of central directory");
            }
            goto out;
        }
        count      = get64(end64 + 32);
        dir_size   = get64(end64 + 40);
        dir_offset = get64(end64 + 48);
    }
    if (dir_size > 256 * 1024 * 1024 || count > dir_size / 46 + 1) {
        fail(error, zip->path, "an implausible central directory");
        goto out;
    }

    guint8 *dir = g_malloc(dir_size ? dir_size : 1);
    if (!pread_all(zip, dir, dir_size, dir_offset, error)) {
        g_free(dir);
        goto out;
    }
    gsize p = 0;
    for (guint64 i = 0; i < count; i++) {
        if (p + 46 > dir_size || get32(dir + p) != 0x02014b50) {
            fail(error, zip->path, "a broken central directory entry");
            g_free(dir);
            goto out;
        }
        guint16 flags     = get16(dir + p + 8);
        guint16 method    = get16(dir + p + 10);
        guint64 csize     = get32(dir + p + 20);
        guint64 usize     = get32(dir + p + 24);
        guint16 name_len  = get16(dir + p + 28);
        guint16 extra_len = get16(dir + p + 30);
        guint16 comm_len  = get16(dir + p + 32);
        guint64 local     = get32(dir + p + 42);
        if (p + 46 + name_len + extra_len + comm_len > dir_size) {
            fail(error, zip->path, "a central directory entry runs past its end");
            g_free(dir);
            goto out;
        }
        const guint8 *extra = dir + p + 46 + name_len;
        for (gsize x = 0; x + 4 <= extra_len;) {
            guint16 id  = get16(extra + x);
            guint16 len = get16(extra + x + 2);
            if (x + 4 + len > extra_len) {
                break;
            }
            if (id == ZIP64_EXTRA) {
                const guint8 *v = extra + x + 4, *v_end = v + len;
                if (usize == MAX32 && v + 8 <= v_end) {
                    usize = get64(v);
                    v += 8;
                }
                if (csize == MAX32 && v + 8 <= v_end) {
                    csize = get64(v);
                    v += 8;
                }
                if (local == MAX32 && v + 8 <= v_end) {
                    local = get64(v);
                }
            }
            x += 4 + len;
        }

        guint8 header[30];
        if (!pread_all(zip, header, 30, local, error) || get32(header) != 0x04034b50) {
            if (error && !*error) {
                fail(error, zip->path, "a missing local header");
            }
            g_free(dir);
            goto out;
        }
        ArchiveZipEntry *e   = g_new0(ArchiveZipEntry, 1);
        e->name              = g_strndup((const char *)dir + p + 46, name_len);
        e->data_offset       = local + 30 + get16(header + 26) + get16(header + 28);
        e->compressed_size   = csize;
        e->uncompressed_size = usize;
        e->stored            = method == 0;
        if ((flags & 1) || (method != 0 && method != 8) || e->data_offset > zip->size
                || csize > zip->size - e->data_offset) {
            /* Encrypted, compressed some other way, or out of bounds: not
             * an entry this can read, so not one it offers. */
            zip_entry_free(e);
        } else {
            g_ptr_array_add(zip->entries, e);
            g_hash_table_insert(zip->by_name, e->name, e);
        }
        p += 46 + name_len + extra_len + comm_len;
    }
    g_free(dir);
    ok = TRUE;
out:
    g_free(tail);
    return ok;
}

ArchiveZipReader *archive_zip_reader_open(const char *path, GError **error)
{
    int fd = g_open(path, O_RDONLY, 0);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not open %s: %s", path, g_strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    ArchiveZipReader *zip = g_new0(ArchiveZipReader, 1);
    zip->path    = g_strdup(path);
    zip->fd      = fd;
    zip->size    = (guint64)st.st_size;
    zip->entries = g_ptr_array_new_with_free_func(zip_entry_free);
    zip->by_name = g_hash_table_new(g_str_hash, g_str_equal);
    if (zip->size < 22 || !read_directory(zip, error)) {
        if (zip->size < 22) {
            fail(error, path, "too short");
        }
        archive_zip_reader_free(zip);
        return NULL;
    }
    return zip;
}

void archive_zip_reader_free(ArchiveZipReader *zip)
{
    if (!zip) {
        return;
    }
    close(zip->fd);
    g_hash_table_unref(zip->by_name);
    g_ptr_array_unref(zip->entries);
    g_free(zip->path);
    g_free(zip);
}

const ArchiveZipEntry *archive_zip_find(ArchiveZipReader *zip, const char *name)
{
    return g_hash_table_lookup(zip->by_name, name);
}

const GPtrArray *archive_zip_entries(ArchiveZipReader *zip)
{
    return zip->entries;
}

GBytes *archive_zip_read_entry(ArchiveZipReader *zip, const ArchiveZipEntry *entry,
        gsize max, GError **error)
{
    if (entry->uncompressed_size > max || entry->compressed_size > max) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "%s in %s is too large",
                entry->name, zip->path);
        return NULL;
    }
    guint8 *raw = g_malloc(entry->compressed_size ? entry->compressed_size : 1);
    if (!pread_all(zip, raw, entry->compressed_size, entry->data_offset, error)) {
        g_free(raw);
        return NULL;
    }
    if (entry->stored) {
        return g_bytes_new_take(raw, entry->compressed_size);
    }
    /* Raw deflate: no zlib or gzip wrapper. */
    z_stream zs = {0};
    guint8 *out = g_malloc(entry->uncompressed_size ? entry->uncompressed_size : 1);
    inflateInit2(&zs, -15);
    zs.next_in   = raw;
    zs.avail_in  = (uInt)entry->compressed_size;
    zs.next_out  = out;
    zs.avail_out = (uInt)entry->uncompressed_size;
    int ret = inflate(&zs, Z_FINISH);
    gboolean ok = ret == Z_STREAM_END && zs.total_out == entry->uncompressed_size;
    inflateEnd(&zs);
    g_free(raw);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s in %s does not inflate",
                entry->name, zip->path);
        g_free(out);
        return NULL;
    }
    return g_bytes_new_take(out, entry->uncompressed_size);
}

gboolean archive_zip_read_at(ArchiveZipReader *zip, const ArchiveZipEntry *entry,
        guint64 offset, void *buf, gsize len, GError **error)
{
    if (!entry->stored) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "%s in %s is compressed, so can't be read at an offset", entry->name, zip->path);
        return FALSE;
    }
    if (offset > entry->uncompressed_size || len > entry->uncompressed_size - offset) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "A read runs past the end of %s", entry->name);
        return FALSE;
    }
    return pread_all(zip, buf, len, entry->data_offset + offset, error);
}
