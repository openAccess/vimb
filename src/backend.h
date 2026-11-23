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

#ifndef _BACKEND_H
#define _BACKEND_H

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

/* The models behind the assistant (assistant.h): each backend keeps its own
 * conversation, in its own provider's form, and takes the user's words and
 * the results of the tools it asked for, a turn at a time. A conversation
 * only grows: what was sent is never changed. */

typedef struct {
    char       *id;         /* the backend's, to answer it by */
    char       *name;
    JsonObject *input;      /* NULL if what came wasn't a JSON object */
    char       *raw;        /* the input as it came, then */
} BackendToolCall;

typedef struct {
    char     *id;
    char     *name;
    gboolean ok;
    char     *text;         /* the result, as JSON, or why the tool failed */
    char     *image_mime;   /* or a picture: a screenshot */
    char     *image_data;   /* base64 */
} BackendToolResult;

typedef struct {
    /* What the model says, as it says it. */
    void (*text)(const char *delta, gpointer data);
    /* What it notes as it works: progress between tool calls. */
    void (*note)(const char *delta, gpointer data);
    /* The turn is over: with tool calls to answer by send_results(), or
     * finished -- stop says why: "end_turn", "max_tokens", "refusal" -- or
     * failed, with error. calls is the backend's. */
    void (*turn_done)(GPtrArray *calls, const char *stop, const char *error, gpointer data);
} BackendEvents;

typedef struct {
    const char *system;     /* the instructions, fixed for the conversation */
    JsonNode   *tools;      /* tools_list(): [{name, description, inputSchema}] */
    const char *model;      /* NULL or empty: the backend's choice */
    const char *effort;     /* how hard to think, where that can be said */
} BackendConfig;

typedef struct Backend Backend;
struct Backend {
    const char *name;
    void (*send)(Backend *b, const char *text);
    void (*send_results)(Backend *b, GPtrArray *results);
    void (*cancel)(Backend *b);     /* the turn ends, with no turn_done */
    void (*free)(Backend *b);
    BackendEvents events;
    gpointer      data;
};

Backend *backend_claude_new(const BackendConfig *config, const BackendEvents *events,
        gpointer data, GError **error);

void backend_tool_call_free(BackendToolCall *call);
void backend_tool_result_free(BackendToolResult *result);

/* ---- for backends ------------------------------------------------------ */

/* Server-sent events from an HTTP request: each event as it arrives, then
 * done, once, with NULL or why the request failed. An answer other than 2xx
 * fails, with its status and body. Cancelling calls neither again. */
typedef void (*SseEventFunc)(const char *event, const char *data, gpointer user);
typedef void (*SseDoneFunc)(guint status, const char *error, gpointer user);
void backend_sse(SoupSession *session, SoupMessage *message, GCancellable *cancellable,
        SseEventFunc on_event, SseDoneFunc on_done, gpointer user);

/* A JSON body for message. */
void backend_set_json_body(SoupMessage *message, JsonNode *body);

/* The text of a JSON node, or a string member's. */
char *backend_json_string(JsonNode *node);

#endif /* end of include guard: _BACKEND_H */
