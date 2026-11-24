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

#include "recorder.h"
#include "warc.h"

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

/* The response's head alone, as a revisit holds it; NULL if it can't be had. */
static Spool *head_of(ArchiveExchange *exchange)
{
    HeadCopy copy = {spool_new(g_get_tmp_dir(), (gsize)exchange->head_length + 1),
        exchange->head_length};
    spool_foreach(exchange->response, copy_head, &copy, NULL);
    if (copy.left) {
        spool_free(copy.head);
        return NULL;
    }
    return copy.head;
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
