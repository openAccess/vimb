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

struct ArchiveRecorder {
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
    g_free(exchange);
}

static void write_exchange(ArchiveRecorder *recorder, ArchiveExchange *exchange)
{
    GError *error   = NULL;
    char *response_id = NULL;

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

    if (warc_writer_write(recorder->writer, &response, exchange->response,
                &response_id, NULL, NULL, &error)) {
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
    g_mutex_clear(&recorder->lock);
    g_free(recorder);
    return ok;
}
