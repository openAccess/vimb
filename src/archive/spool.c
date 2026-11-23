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
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "spool.h"

#define READ_CHUNK (64 * 1024)

struct Spool {
    char       *tmp_dir;
    gsize      memory_limit;
    GByteArray *memory;     /* the content, until it outgrows memory_limit */
    int        fd;          /* then the file holding all of it */
    guint64    length;
};

static gboolean write_all(int fd, const guint8 *data, gsize len, GError **error)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not write a spool file: %s", g_strerror(errno));
            return FALSE;
        }
        data += n;
        len  -= n;
    }
    return TRUE;
}

/* Moves what is in memory to a new file, which is unlinked at once: it ends
 * with the spool, however the process does. */
static gboolean spill(Spool *spool, GError **error)
{
    char *template = g_build_filename(spool->tmp_dir, "spool-XXXXXX", NULL);
    int fd         = g_mkstemp_full(template, O_RDWR, 0600);

    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not make a spool file in %s: %s", spool->tmp_dir,
                g_strerror(errno));
        g_free(template);
        return FALSE;
    }
    g_unlink(template);
    g_free(template);

    if (!write_all(fd, spool->memory->data, spool->memory->len, error)) {
        close(fd);
        return FALSE;
    }
    g_byte_array_free(spool->memory, TRUE);
    spool->memory = NULL;
    spool->fd     = fd;
    return TRUE;
}

Spool *spool_new(const char *tmp_dir, gsize memory_limit)
{
    Spool *spool        = g_new0(Spool, 1);
    spool->tmp_dir      = g_strdup(tmp_dir ? tmp_dir : g_get_tmp_dir());
    spool->memory_limit = memory_limit;
    spool->memory       = g_byte_array_new();
    spool->fd           = -1;
    return spool;
}

void spool_free(Spool *spool)
{
    if (!spool) {
        return;
    }
    if (spool->memory) {
        g_byte_array_free(spool->memory, TRUE);
    }
    if (spool->fd >= 0) {
        close(spool->fd);
    }
    g_free(spool->tmp_dir);
    g_free(spool);
}

gboolean spool_append(Spool *spool, const void *data, gsize len, GError **error)
{
    if (!len) {
        return TRUE;
    }
    if (spool->memory && spool->memory->len + len > spool->memory_limit) {
        if (!spill(spool, error)) {
            return FALSE;
        }
    }
    if (spool->memory) {
        g_byte_array_append(spool->memory, data, len);
    } else if (!write_all(spool->fd, data, len, error)) {
        return FALSE;
    }
    spool->length += len;
    return TRUE;
}

guint64 spool_length(const Spool *spool)
{
    return spool->length;
}

gboolean spool_foreach(Spool *spool, SpoolFunc func, gpointer user_data,
        GError **error)
{
    if (spool->memory) {
        return spool->memory->len == 0
            || func(spool->memory->data, spool->memory->len, user_data, error);
    }

    guint8 *buf     = g_malloc(READ_CHUNK);
    guint64 offset  = 0;
    gboolean result = TRUE;
    while (offset < spool->length) {
        ssize_t n = pread(spool->fd, buf, READ_CHUNK, (off_t)offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            g_set_error(error, G_FILE_ERROR,
                    g_file_error_from_errno(n < 0 ? errno : EIO),
                    "Could not read a spool file: %s",
                    n < 0 ? g_strerror(errno) : "it ended early");
            result = FALSE;
            break;
        }
        if (!func(buf, (gsize)n, user_data, error)) {
            result = FALSE;
            break;
        }
        offset += (guint64)n;
    }
    g_free(buf);
    return result;
}

static gboolean append_to_array(const guint8 *data, gsize len, gpointer user_data,
        GError **error)
{
    g_byte_array_append(user_data, data, len);
    return TRUE;
}

GBytes *spool_get_bytes(Spool *spool, gsize max, GError **error)
{
    if (spool->length > max) {
        return NULL;
    }
    GByteArray *all = g_byte_array_sized_new((guint)spool->length);
    if (!spool_foreach(spool, append_to_array, all, error)) {
        g_byte_array_free(all, TRUE);
        return NULL;
    }
    return g_byte_array_free_to_bytes(all);
}
