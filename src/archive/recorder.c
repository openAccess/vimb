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

#include <string.h>

#include "recorder.h"
#include "warc.h"

#define SPOOL_MEMORY_LIMIT (1 << 20)

/* Payloads smaller than this aren't worth a revisit, whose head is as big. */
#define REVISIT_MIN 512

/* Where a payload was first recorded. */
typedef struct {
    char *id;
    char *uri;
    char *date;
} Original;

static void original_free(Original *o)
{
    g_free(o->id);
    g_free(o->uri);
    g_free(o->date);
    g_free(o);
}

struct ArchiveRecorder {
    GHashTable  *originals;     /* payload digest -> Original; the writer's thread's */
    WarcWriter  *writer;
    GThread     *thread;
    GAsyncQueue *queue;
    gint        count;
    GMutex      lock;
    GError      *error;         /* the first a write met */
};

/* Queued to tell the thread to stop. */
static ArchiveExchange stop_marker;

ArchiveExchange *archive_exchange_new(void)
{
    return g_new0(ArchiveExchange, 1);
}

void archive_exchange_free(ArchiveExchange *exchange)
{
    if (!exchange || exchange == &stop_marker) {
        return;
    }
    g_free(exchange->target_uri);
    g_free(exchange->ip_address);
    g_clear_pointer(&exchange->date, g_date_time_unref);
    spool_free(exchange->request);
    spool_free(exchange->response);
    g_free(exchange->payload_digest);
    g_free(exchange->truncated);
    g_free(exchange->record_type);
    g_free(exchange->content_type);
    g_free(exchange);
}

typedef struct {
    Spool   *head;
    guint64 left;
} HeadCopy;

static gboolean copy_head(const guint8 *data, gsize len, gpointer user_data, GError **error)
{
    HeadCopy *copy = user_data;
    gsize n = (gsize)MIN((guint64)len, copy->left);
    if (n && !spool_append(copy->head, data, n, error)) {
        return FALSE;
    }
    copy->left -= n;
    return copy->left > 0;          /* stopped, once it has it all */
}

/* The first length bytes of a spool; NULL if it hasn't that many. */
static Spool *head_bytes(Spool *spool, guint64 length)
{
    HeadCopy copy = {spool_new(g_get_tmp_dir(), (gsize)length + 1), length};
    spool_foreach(spool, copy_head, &copy, NULL);
    if (copy.left) {
        spool_free(copy.head);
        return NULL;
    }
    return copy.head;
}

/* The response's head alone, as a revisit holds it; NULL if it can't be had. */
static Spool *head_of(ArchiveExchange *exchange)
{
    return head_bytes(exchange->response, exchange->head_length);
}

typedef struct {
    Spool   *to;
    guint64 skip;
} Tail;

static gboolean copy_tail(const guint8 *data, gsize len, gpointer user_data, GError **error)
{
    Tail *t = user_data;
    gsize skipped = (gsize)MIN((guint64)len, t->skip);
    t->skip -= skipped;
    return len == skipped || spool_append(t->to, data + skipped, len - skipped, error);
}

/* A message whose head names any of names gets them as "[redacted]": a new
 * spool, or NULL to keep the one it has. */
static Spool *redacted(Spool *message, guint64 head_length, const char **names)
{
    Spool *head = head_length ? head_bytes(message, head_length) : NULL;
    GBytes *bytes = head ? spool_get_bytes(head, (gsize)head_length + 1, NULL) : NULL;
    spool_free(head);
    if (!bytes) {
        return NULL;
    }
    gsize size;
    const char *text = g_bytes_get_data(bytes, &size);
    char *copy = g_strndup(text, size);
    g_bytes_unref(bytes);
    char **lines = g_strsplit(copy, "\r\n", -1);
    GString *out = g_string_new(NULL);
    gboolean changed = FALSE;
    for (int i = 0; lines[i]; i++) {
        const char *colon = i ? strchr(lines[i], ':') : NULL;
        gboolean secret = FALSE;
        for (int n = 0; colon && names[n] && !secret; n++) {
            secret = (gsize)(colon - lines[i]) == strlen(names[n])
                && !g_ascii_strncasecmp(lines[i], names[n], colon - lines[i]);
        }
        if (secret) {
            g_string_append_len(out, lines[i], colon - lines[i] + 1);
            g_string_append(out, " [redacted]");
            changed = TRUE;
        } else {
            g_string_append(out, lines[i]);
        }
        if (lines[i + 1]) {
            g_string_append(out, "\r\n");
        }
    }
    g_strfreev(lines);
    g_free(copy);
    Spool *result = NULL;
    if (changed) {
        result = spool_new(g_get_tmp_dir(), SPOOL_MEMORY_LIMIT);
        Tail tail = {result, head_length};
        if (!spool_append(result, out->str, out->len, NULL)
                || !spool_foreach(message, copy_tail, &tail, NULL)) {
            spool_free(result);
            result = NULL;
        }
    }
    g_string_free(out, TRUE);
    return result;
}

static const char *request_secrets[]  = {"Cookie", "Authorization", NULL};
static const char *response_secrets[] = {"Set-Cookie", "Set-Cookie2", NULL};

/* An exchange of an archive made signed in, its credentials taken out. */
static void redact(ArchiveExchange *exchange)
{
    Spool *request  = redacted(exchange->request, exchange->request_head_length, request_secrets);
    Spool *response = redacted(exchange->response, exchange->head_length, response_secrets);
    if (request) {
        spool_free(exchange->request);
        exchange->request = request;
    }
    if (response) {
        spool_free(exchange->response);
        exchange->response = response;
    }
}

static gboolean dedupable(const ArchiveExchange *exchange)
{
    return exchange->payload_digest && !exchange->truncated && exchange->head_length
        && exchange->payload_length >= REVISIT_MIN;
}

/* A record of its own, not an exchange. */
static void write_record(ArchiveRecorder *recorder, ArchiveExchange *exchange)
{
    GError *error = NULL;
    WarcRecordInfo info = {
        .type         = exchange->record_type,
        .target_uri   = exchange->target_uri,
        .content_type = exchange->content_type,
        .date         = exchange->date,
    };
    if (warc_writer_write(recorder->writer, &info, exchange->response, NULL, NULL, NULL,
                &error)) {
        g_atomic_int_inc(&recorder->count);
    } else {
        g_warning("archive: could not record %s: %s", exchange->target_uri, error->message);
        g_mutex_lock(&recorder->lock);
        if (!recorder->error) {
            recorder->error = error;
            error = NULL;
        }
        g_mutex_unlock(&recorder->lock);
        g_clear_error(&error);
    }
}

static void write_exchange(ArchiveRecorder *recorder, ArchiveExchange *exchange)
{
    GError *error   = NULL;
    char *response_id = NULL;
    if (exchange->record_type) {
        write_record(recorder, exchange);
        return;
    }
    if (exchange->redact) {
        redact(exchange);
    }
    Original *original = dedupable(exchange)
        ? g_hash_table_lookup(recorder->originals, exchange->payload_digest) : NULL;
    Spool *head = original ? head_of(exchange) : NULL;

    WarcRecordInfo response = {
        .type           = "response",
        .target_uri     = exchange->target_uri,
        .ip_address     = exchange->ip_address,
        .content_type   = "application/http;msgtype=response",
        .payload_digest = exchange->payload_digest,
        .truncated      = exchange->truncated,
        .date           = exchange->date,
    };
    WarcRecordInfo request = {
        .type         = "request",
        .target_uri   = exchange->target_uri,
        .ip_address   = exchange->ip_address,
        .content_type = "application/http;msgtype=request",
        .date         = exchange->date,
    };

    if (head) {
        response.type           = "revisit";
        response.profile        = WARC_PROFILE_IDENTICAL;
        response.refers_to      = original->id;
        response.refers_to_uri  = original->uri;
        response.refers_to_date = original->date;
    }
    if (warc_writer_write(recorder->writer, &response, head ? head : exchange->response,
                &response_id, NULL, NULL, &error)) {
        if (!head && dedupable(exchange)
                && !g_hash_table_contains(recorder->originals, exchange->payload_digest)) {
            Original *o = g_new0(Original, 1);
            o->id   = g_strdup(response_id);
            o->uri  = g_strdup(exchange->target_uri);
            o->date = warc_format_date(exchange->date);
            g_hash_table_insert(recorder->originals, g_strdup(exchange->payload_digest), o);
        }
        request.concurrent_to = response_id;
        if (warc_writer_write(recorder->writer, &request, exchange->request,
                    NULL, NULL, NULL, &error)) {
            g_atomic_int_inc(&recorder->count);
        }
    }
    if (error) {
        g_warning("archive: could not record %s: %s", exchange->target_uri, error->message);
        g_mutex_lock(&recorder->lock);
        if (!recorder->error) {
            recorder->error = error;
            error = NULL;
        }
        g_mutex_unlock(&recorder->lock);
        g_clear_error(&error);
    }
    g_free(response_id);
    if (head) {
        spool_free(head);
    }
}

static gpointer run(gpointer data)
{
    ArchiveRecorder *recorder = data;
    for (;;) {
        ArchiveExchange *exchange = g_async_queue_pop(recorder->queue);
        if (exchange == &stop_marker) {
            break;
        }
        write_exchange(recorder, exchange);
        archive_exchange_free(exchange);
    }
    return NULL;
}

ArchiveRecorder *archive_recorder_start(const char *warc_path, const char *software,
        GError **error)
{
    WarcWriter *writer = warc_writer_open(warc_path, software, error);
    if (!writer) {
        return NULL;
    }
    ArchiveRecorder *recorder = g_new0(ArchiveRecorder, 1);
    recorder->writer = writer;
    recorder->queue  = g_async_queue_new();
    recorder->originals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)original_free);
    g_mutex_init(&recorder->lock);
    recorder->thread = g_thread_new("archive-writer", run, recorder);
    return recorder;
}

void archive_recorder_submit(ArchiveRecorder *recorder, ArchiveExchange *exchange)
{
    g_async_queue_push(recorder->queue, exchange);
}

guint archive_recorder_count(ArchiveRecorder *recorder)
{
    return (guint)g_atomic_int_get(&recorder->count);
}

gboolean archive_recorder_finish(ArchiveRecorder *recorder, guint *written, GError **error)
{
    g_async_queue_push(recorder->queue, &stop_marker);
    g_thread_join(recorder->thread);
    if (written) {
        *written = archive_recorder_count(recorder);
    }

    GError *close_error = NULL;
    gboolean closed = warc_writer_close(recorder->writer, &close_error);
    gboolean ok     = closed && !recorder->error;
    if (recorder->error) {
        g_propagate_error(error, recorder->error);
        g_clear_error(&close_error);
    } else if (!closed) {
        g_propagate_error(error, close_error);
    }
    g_async_queue_unref(recorder->queue);
    g_hash_table_unref(recorder->originals);
    g_mutex_clear(&recorder->lock);
    g_free(recorder);
    return ok;
}
