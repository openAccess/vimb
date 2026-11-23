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

#include "backend.h"

#define ERROR_BODY_MAX (64 * 1024)

void backend_tool_call_free(BackendToolCall *call)
{
    if (call) {
        g_free(call->id);
        g_free(call->name);
        if (call->input) {
            json_object_unref(call->input);
        }
        g_free(call->raw);
        g_free(call);
    }
}

void backend_tool_result_free(BackendToolResult *result)
{
    if (result) {
        g_free(result->id);
        g_free(result->name);
        g_free(result->text);
        g_free(result->image_mime);
        g_free(result->image_data);
        g_free(result);
    }
}

void backend_set_json_body(SoupMessage *message, JsonNode *body)
{
    gsize length;
    char *text   = json_to_string(body, FALSE);
    length       = strlen(text);
    GBytes *bytes = g_bytes_new_take(text, length);
    soup_message_set_request_body_from_bytes(message, "application/json", bytes);
    g_bytes_unref(bytes);
}

char *backend_json_string(JsonNode *node)
{
    return node ? json_to_string(node, FALSE) : g_strdup("null");
}

/* ---- server-sent events ------------------------------------------------- */

typedef struct {
    SoupMessage      *message;
    GCancellable     *cancellable;
    GDataInputStream *in;
    SseEventFunc     on_event;
    SseDoneFunc      on_done;
    gpointer         user;
    GString          *event;
    GString          *data;
    gboolean         has_data;
    GString          *error_body;   /* the body of a failed request */
    guint            status;
} Sse;

static void sse_free(Sse *s)
{
    g_object_unref(s->message);
    g_clear_object(&s->cancellable);
    g_clear_object(&s->in);
    g_string_free(s->event, TRUE);
    g_string_free(s->data, TRUE);
    if (s->error_body) {
        g_string_free(s->error_body, TRUE);
    }
    g_free(s);
}

static void sse_finish(Sse *s, GError *error)
{
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        sse_free(s);                /* cancelled: nobody is told */
        return;
    }
    s->on_done(s->status, error ? error->message : NULL, s->user);
    sse_free(s);
}

static void sse_dispatch(Sse *s)
{
    if (s->has_data) {
        s->on_event(s->event->len ? s->event->str : "message", s->data->str, s->user);
    }
    g_string_truncate(s->event, 0);
    g_string_truncate(s->data, 0);
    s->has_data = FALSE;
}

static void sse_read(Sse *s);

static void on_sse_line(GObject *source, GAsyncResult *res, gpointer user)
{
    Sse *s        = user;
    GError *error = NULL;
    gsize length;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), res,
            &length, &error);
    if (line && g_cancellable_is_cancelled(s->cancellable)) {
        g_free(line);               /* whoever asked has gone */
        sse_free(s);
        return;
    }
    if (!line) {
        if (!error) {
            sse_dispatch(s);        /* the stream ended */
        }
        sse_finish(s, error);
        g_clear_error(&error);
        return;
    }
    if (length && line[length - 1] == '\r') {
        line[--length] = '\0';
    }
    if (!length) {
        sse_dispatch(s);
    } else if (line[0] != ':') {    /* a comment, otherwise */
        char *colon       = strchr(line, ':');
        const char *value = colon ? colon + 1 : "";
        if (colon) {
            *colon = '\0';
            if (*value == ' ') {
                value++;
            }
        }
        if (!strcmp(line, "event")) {
            g_string_assign(s->event, value);
        } else if (!strcmp(line, "data")) {
            if (s->has_data) {
                g_string_append_c(s->data, '\n');
            }
            g_string_append(s->data, value);
            s->has_data = TRUE;
        }
    }
    g_free(line);
    sse_read(s);
}

static void sse_read(Sse *s)
{
    g_data_input_stream_read_line_async(s->in, G_PRIORITY_DEFAULT, s->cancellable, on_sse_line, s);
}

/* The body of a failed request, read whole (or the first of it). */
static void on_error_body(GObject *source, GAsyncResult *res, gpointer user)
{
    Sse *s        = user;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &error);
    if (!bytes) {
        sse_finish(s, error);
        g_error_free(error);
        return;
    }
    gsize size;
    const char *data = g_bytes_get_data(bytes, &size);
    if (size && s->error_body->len < ERROR_BODY_MAX) {
        g_string_append_len(s->error_body, data, size);
        g_bytes_unref(bytes);
        g_input_stream_read_bytes_async(G_INPUT_STREAM(source), 8192, G_PRIORITY_DEFAULT,
                s->cancellable, on_error_body, s);
        return;
    }
    g_bytes_unref(bytes);

    /* the message of an API's JSON error, if there is one, else the body */
    char *message = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, s->error_body->str, s->error_body->len, NULL)
            && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(parser));
        JsonNode *e   = json_object_get_member(o, "error");
        if (e && JSON_NODE_HOLDS_OBJECT(e)
                && json_object_has_member(json_node_get_object(e), "message")) {
            message = g_strdup(json_object_get_string_member(json_node_get_object(e), "message"));
        }
    }
    g_object_unref(parser);
    if (!message) {
        message = g_strndup(s->error_body->str, MIN(s->error_body->len, 500));
    }
    GError *failed = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s: %s", s->status,
            soup_message_get_reason_phrase(s->message), message);
    g_free(message);
    sse_finish(s, failed);
    g_error_free(failed);
}

static void on_sse_sent(GObject *source, GAsyncResult *res, gpointer user)
{
    Sse *s           = user;
    GError *error    = NULL;
    GInputStream *in = soup_session_send_finish(SOUP_SESSION(source), res, &error);
    if (!in) {
        sse_finish(s, error);
        g_error_free(error);
        return;
    }
    s->status = soup_message_get_status(s->message);
    if (!SOUP_STATUS_IS_SUCCESSFUL(s->status)) {
        s->error_body = g_string_new(NULL);
        g_input_stream_read_bytes_async(in, 8192, G_PRIORITY_DEFAULT, s->cancellable,
                on_error_body, s);
        g_object_unref(in);
        return;
    }
    s->in = g_data_input_stream_new(in);
    g_object_unref(in);
    /* LF, with any CR before it stripped: ANY could split a CRLF across reads */
    g_data_input_stream_set_newline_type(s->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    sse_read(s);
}

void backend_sse(SoupSession *session, SoupMessage *message, GCancellable *cancellable,
        SseEventFunc on_event, SseDoneFunc on_done, gpointer user)
{
    Sse *s         = g_new0(Sse, 1);
    s->message     = g_object_ref(message);
    s->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    s->on_event    = on_event;
    s->on_done     = on_done;
    s->user        = user;
    s->event       = g_string_new(NULL);
    s->data        = g_string_new(NULL);
    soup_message_headers_replace(soup_message_get_request_headers(message), "Accept",
            "text/event-stream");
    soup_session_send_async(session, message, G_PRIORITY_DEFAULT, cancellable, on_sse_sent, s);
}
