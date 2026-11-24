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
#include <unistd.h>
#include <zlib.h>

#include "warc.h"

#define OUT_CHUNK (64 * 1024)

struct WarcWriter {
    char    *path;
    char    *filename;
    int     fd;
    guint64 offset;         /* where the next record starts */
    char    *warcinfo_id;
};

typedef struct {
    WarcWriter *writer;
    z_stream   zs;
    guint8     out[OUT_CHUNK];
    guint64    written;
} Member;

static gboolean write_all(WarcWriter *writer, const guint8 *data, gsize len,
        GError **error)
{
    while (len) {
        ssize_t n = write(writer->fd, data, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not write %s: %s", writer->path, g_strerror(errno));
            return FALSE;
        }
        data += n;
        len  -= n;
    }
    return TRUE;
}

static gboolean member_begin(Member *m, WarcWriter *writer, GError **error)
{
    memset(&m->zs, 0, sizeof(m->zs));
    m->writer  = writer;
    m->written = 0;
    /* 15 + 16: a gzip wrapper, so each record is a gzip member of its own. */
    if (deflateInit2(&m->zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Could not start compressing a record");
        return FALSE;
    }
    return TRUE;
}

static gboolean member_deflate(Member *m, const guint8 *data, gsize len, int flush,
        GError **error)
{
    int ret;

    m->zs.next_in  = (Bytef *)data;
    m->zs.avail_in = (uInt)len;
    do {
        m->zs.next_out  = m->out;
        m->zs.avail_out = sizeof(m->out);
        ret = deflate(&m->zs, flush);
        if (ret == Z_STREAM_ERROR) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not compress a record");
            return FALSE;
        }
        gsize produced = sizeof(m->out) - m->zs.avail_out;
        if (produced && !write_all(m->writer, m->out, produced, error)) {
            return FALSE;
        }
        m->written += produced;
    } while (m->zs.avail_out == 0 || (flush == Z_FINISH && ret != Z_STREAM_END));
    return TRUE;
}

static gboolean member_write(const guint8 *data, gsize len, gpointer user_data,
        GError **error)
{
    Member *m = user_data;
    /* zlib takes at most 4 GiB at a time. */
    while (len) {
        gsize n = MIN(len, (gsize)G_MAXUINT32);
        if (!member_deflate(m, data, n, Z_NO_FLUSH, error)) {
            return FALSE;
        }
        data += n;
        len  -= n;
    }
    return TRUE;
}

static gboolean member_end(Member *m, GError **error)
{
    gboolean ok = member_deflate(m, NULL, 0, Z_FINISH, error);
    deflateEnd(&m->zs);
    return ok;
}

char *warc_new_record_id(void)
{
    char *uuid = g_uuid_string_random();
    char *id   = g_strdup_printf("<urn:uuid:%s>", uuid);
    g_free(uuid);
    return id;
}

char *warc_format_date(GDateTime *date)
{
    GDateTime *utc = g_date_time_to_utc(date);
    char *text     = g_strdup_printf("%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
            g_date_time_get_year(utc), g_date_time_get_month(utc),
            g_date_time_get_day_of_month(utc), g_date_time_get_hour(utc),
            g_date_time_get_minute(utc), g_date_time_get_second(utc),
            g_date_time_get_microsecond(utc));
    g_date_time_unref(utc);
    return text;
}

char *warc_label_sha1(const guint8 digest[20])
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    GString *out = g_string_new("sha1:");
    guint32 buffer = 0;
    int bits = 0;

    for (int i = 0; i < 20; i++) {
        buffer = (buffer << 8) | digest[i];
        bits  += 8;
        while (bits >= 5) {
            g_string_append_c(out, alphabet[(buffer >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    if (bits) {
        g_string_append_c(out, alphabet[(buffer << (5 - bits)) & 31]);
    }
    return g_string_free(out, FALSE);
}

typedef struct {
    GChecksum *sha1;
} DigestState;

static gboolean digest_piece(const guint8 *data, gsize len, gpointer user_data,
        GError **error)
{
    g_checksum_update(((DigestState *)user_data)->sha1, data, (gssize)len);
    return TRUE;
}

/* WARC header values are single lines. */
static gboolean field_ok(const char *value)
{
    return !value || !strpbrk(value, "\r\n");
}

static void add_field(GString *head, const char *name, const char *value)
{
    if (value && *value) {
        g_string_append_printf(head, "%s: %s\r\n", name, value);
    }
}

gboolean warc_writer_write(WarcWriter *writer, const WarcRecordInfo *info,
        Spool *block, char **record_id, guint64 *offset, guint64 *length,
        GError **error)
{
    if (!field_ok(info->target_uri) || !field_ok(info->content_type)
            || !field_ok(info->ip_address) || !field_ok(info->refers_to_uri)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "A WARC header value would span lines");
        return FALSE;
    }

    /* The block digest is needed before the block is written. */
    DigestState digest = {g_checksum_new(G_CHECKSUM_SHA1)};
    if (block && !spool_foreach(block, digest_piece, &digest, error)) {
        g_checksum_free(digest.sha1);
        return FALSE;
    }
    guint8 sha1[20];
    gsize sha1_len = sizeof(sha1);
    g_checksum_get_digest(digest.sha1, sha1, &sha1_len);
    g_checksum_free(digest.sha1);
    char *block_digest = warc_label_sha1(sha1);

    GDateTime *now = info->date ? g_date_time_ref(info->date) : g_date_time_new_now_utc();
    char *date     = warc_format_date(now);
    char *id       = warc_new_record_id();
    GString *head  = g_string_new("WARC/1.1\r\n");
    g_date_time_unref(now);

    add_field(head, "WARC-Type", info->type);
    add_field(head, "WARC-Record-ID", id);
    add_field(head, "WARC-Date", date);
    add_field(head, "WARC-Target-URI", info->target_uri);
    add_field(head, "WARC-Concurrent-To", info->concurrent_to);
    add_field(head, "WARC-Warcinfo-ID", writer->warcinfo_id);
    add_field(head, "WARC-IP-Address", info->ip_address);
    if (!strcmp(info->type, "warcinfo")) {
        add_field(head, "WARC-Filename", writer->filename);
    }
    add_field(head, "WARC-Profile", info->profile);
    add_field(head, "WARC-Refers-To", info->refers_to);
    add_field(head, "WARC-Refers-To-Target-URI", info->refers_to_uri);
    add_field(head, "WARC-Refers-To-Date", info->refers_to_date);
    add_field(head, "WARC-Truncated", info->truncated);
    add_field(head, "WARC-Block-Digest", block_digest);
    add_field(head, "WARC-Payload-Digest", info->payload_digest);
    add_field(head, "Content-Type", info->content_type);
    g_string_append_printf(head, "Content-Length: %" G_GUINT64_FORMAT "\r\n\r\n",
            block ? spool_length(block) : 0);
    g_free(block_digest);
    g_free(date);

    Member *m      = g_new(Member, 1);
    guint64 start  = writer->offset;
    gboolean ok    = member_begin(m, writer, error)
        && member_write((const guint8 *)head->str, head->len, m, error)
        && (!block || spool_foreach(block, member_write, m, error))
        && member_write((const guint8 *)"\r\n\r\n", 4, m, error)
        && member_end(m, error);
    if (ok) {
        writer->offset += m->written;
        if (offset) {
            *offset = start;
        }
        if (length) {
            *length = m->written;
        }
    } else {
        /* A record half written would make the rest of the file unreadable;
         * cut it back off. */
        if (ftruncate(writer->fd, (off_t)start) == 0) {
            lseek(writer->fd, (off_t)start, SEEK_SET);
        }
    }
    g_free(m);
    g_string_free(head, TRUE);

    if (ok && record_id) {
        *record_id = id;
    } else {
        g_free(id);
    }
    return ok;
}

WarcWriter *warc_writer_open(const char *path, const char *software, GError **error)
{
    WarcWriter *writer = g_new0(WarcWriter, 1);
    writer->path       = g_strdup(path);
    writer->filename   = g_path_get_basename(path);
    writer->fd         = g_open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (writer->fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not make %s: %s", path, g_strerror(errno));
        g_free(writer->filename);
        g_free(writer->path);
        g_free(writer);
        return NULL;
    }

    char *fields = g_strdup_printf(
            "software: %s\r\n"
            "format: WARC File Format 1.1\r\n"
            "conformsTo: http://iipc.github.io/warc-specifications/specifications/warc-format/warc-1.1/\r\n",
            software);
    Spool *block = spool_new(NULL, G_MAXSIZE);
    WarcRecordInfo info = {
        .type         = "warcinfo",
        .content_type = "application/warc-fields",
    };
    gboolean ok = spool_append(block, fields, strlen(fields), error)
        && warc_writer_write(writer, &info, block, &writer->warcinfo_id, NULL, NULL, error);
    spool_free(block);
    g_free(fields);
    if (!ok) {
        close(writer->fd);
        g_unlink(path);
        g_free(writer->filename);
        g_free(writer->path);
        g_free(writer);
        return NULL;
    }
    return writer;
}

gboolean warc_writer_close(WarcWriter *writer, GError **error)
{
    gboolean ok = TRUE;
    if (fsync(writer->fd) < 0 || close(writer->fd) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not finish %s: %s", writer->path, g_strerror(errno));
        ok = FALSE;
    }
    g_free(writer->warcinfo_id);
    g_free(writer->filename);
    g_free(writer->path);
    g_free(writer);
    return ok;
}

const char *warc_writer_path(const WarcWriter *writer)
{
    return writer->path;
}
