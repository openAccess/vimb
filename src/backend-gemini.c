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

/* The assistant on Gemini, by generateContent, streamed. (Google's newer
 * Interactions API keeps conversations on its servers by default; this one
 * keeps nothing, and the conversation stays here.)
 *
 * The replies go back as they came, part for part: a part that carries a
 * thought signature must come back as it was, and on its own, for the model
 * to accept the results of the calls it made. Text that streamed in pieces
 * is joined up again, except where a signature is. */

#include <string.h>

#include "../version.h"
#include "backend.h"

#define DEFAULT_MODEL   "gemini-4-hydrogen"
#define DEFAULT_URL     "https://generativelanguage.googleapis.com"
#define ATTEMPTS        4

typedef struct {
    Backend      base;
    SoupSession  *session;
    char         *url;
    char         *key;
    char         *level;        /* thinkingLevel */
    JsonObject   *system;       /* systemInstruction */
    JsonArray    *tools;
    JsonArray    *contents;
    guint        images;        /* screenshots sent, to name them */
    /* the turn */
    GCancellable *cancellable;
    JsonArray    *parts;
    char         *finish;
    char         *blocked;
    char         *stream_error;
    guint        attempt;
    guint        retry_source;
} Gemini;

static void request(Gemini *g);

/* ---- the conversation -------------------------------------------------- */

static JsonObject *last_content(Gemini *g, const char *role)
{
    guint n = json_array_get_length(g->contents);
    if (!n) {
        return NULL;
    }
    JsonObject *c = json_array_get_object_element(g->contents, n - 1);
    return !g_strcmp0(json_object_get_string_member_with_default(c, "role", ""), role) ? c : NULL;
}

/* Adds parts for the user: to the last content if that is the user's still
 * (its turn never answered), else as a content of its own. Takes parts. */
static void add_user_parts(Gemini *g, JsonArray *parts)
{
    JsonObject *last = last_content(g, "user");
    if (last) {
        JsonArray *have = json_object_get_array_member(last, "parts");
        for (guint i = 0; i < json_array_get_length(parts); i++) {
            json_array_add_element(have, json_node_copy(json_array_get_element(parts, i)));
        }
        json_array_unref(parts);
        return;
    }
    JsonObject *c = json_object_new();
    json_object_set_string_member(c, "role", "user");
    json_object_set_array_member(c, "parts", parts);
    json_array_add_object_element(g->contents, c);
}

/* A function's response, as {"result": ...} or {"error": ...}. Takes
 * response. */
static JsonObject *function_response(const char *id, const char *name, JsonObject *response)
{
    JsonObject *fr = json_object_new();
    if (id && *id) {
        json_object_set_string_member(fr, "id", id);
    }
    json_object_set_string_member(fr, "name", name);
    json_object_set_object_member(fr, "response", response);
    JsonObject *part = json_object_new();
    json_object_set_object_member(part, "functionResponse", fr);
    return part;
}

static JsonObject *error_response(const char *text)
{
    JsonObject *r = json_object_new();
    json_object_set_string_member(r, "error", text);
    return r;
}

/* Answers, as not run, the calls of the last reply if nothing has. */
static void answer_unanswered(Gemini *g, JsonArray *parts)
{
    JsonObject *last = last_content(g, "model");
    JsonArray *have  = last ? json_object_get_array_member(last, "parts") : NULL;
    for (guint i = 0; have && i < json_array_get_length(have); i++) {
        JsonObject *p = json_array_get_object_element(have, i);
        if (json_object_has_member(p, "functionCall")) {
            JsonObject *fc = json_object_get_object_member(p, "functionCall");
            json_array_add_object_element(parts, function_response(
                    json_object_get_string_member_with_default(fc, "id", ""),
                    json_object_get_string_member_with_default(fc, "name", ""),
                    error_response("not run: the turn was stopped first")));
        }
    }
}

static void gemini_send(Backend *b, const char *text)
{
    Gemini *g        = (Gemini *)b;
    JsonArray *parts = json_array_new();
    answer_unanswered(g, parts);
    JsonObject *t = json_object_new();
    json_object_set_string_member(t, "text", text);
    json_array_add_object_element(parts, t);
    add_user_parts(g, parts);
    g->attempt = 0;
    request(g);
}

static void gemini_send_results(Backend *b, GPtrArray *results)
{
    Gemini *g        = (Gemini *)b;
    JsonArray *parts = json_array_new();
    for (guint i = 0; i < results->len; i++) {
        BackendToolResult *r = g_ptr_array_index(results, i);
        JsonObject *response;
        JsonObject *part;
        if (r->image_data) {
            /* the picture goes with the response, which refers to it */
            char *name = g_strdup_printf("screenshot-%u.png", ++g->images);
            JsonObject *ref = json_object_new();
            json_object_set_string_member(ref, "$ref", name);
            response = json_object_new();
            json_object_set_object_member(response, "image", ref);
            part = function_response(r->id, r->name, response);
            JsonObject *data = json_object_new();
            json_object_set_string_member(data, "mimeType", r->image_mime);
            json_object_set_string_member(data, "displayName", name);
            json_object_set_string_member(data, "data", r->image_data);
            JsonObject *inline_part = json_object_new();
            json_object_set_object_member(inline_part, "inlineData", data);
            JsonArray *extra = json_array_new();
            json_array_add_object_element(extra, inline_part);
            json_object_set_array_member(json_object_get_object_member(part, "functionResponse"),
                    "parts", extra);
            g_free(name);
        } else if (r->ok) {
            response = json_object_new();
            JsonParser *parser = json_parser_new();
            if (r->text && json_parser_load_from_data(parser, r->text, -1, NULL)) {
                json_object_set_member(response, "result",
                        json_node_copy(json_parser_get_root(parser)));
            } else {
                json_object_set_string_member(response, "result", r->text ? r->text : "");
            }
            g_object_unref(parser);
            part = function_response(r->id, r->name, response);
        } else {
            part = function_response(r->id, r->name, error_response(r->text ? r->text : "failed"));
        }
        json_array_add_object_element(parts, part);
    }
    add_user_parts(g, parts);
    g->attempt = 0;
    request(g);
}

/* ---- the turn ---------------------------------------------------------- */

static void turn_reset(Gemini *g)
{
    if (g->cancellable) {
        g_cancellable_cancel(g->cancellable);
        g_clear_object(&g->cancellable);
    }
    if (g->retry_source) {
        g_source_remove(g->retry_source);
        g->retry_source = 0;
    }
    g_clear_pointer(&g->parts, json_array_unref);
    g_clear_pointer(&g->finish, g_free);
    g_clear_pointer(&g->blocked, g_free);
    g_clear_pointer(&g->stream_error, g_free);
}

/* Whether a part is nothing but text, or a thought's text. */
static gboolean plain_text(JsonObject *part)
{
    JsonObjectIter iter;
    const char *name;
    if (!json_object_has_member(part, "text")) {
        return FALSE;
    }
    json_object_iter_init(&iter, part);
    while (json_object_iter_next(&iter, &name, NULL)) {
        if (strcmp(name, "text") && strcmp(name, "thought")) {
            return FALSE;
        }
    }
    return TRUE;
}

static void add_part(Gemini *g, JsonObject *part)
{
    gboolean thought = json_object_get_boolean_member_with_default(part, "thought", FALSE);
    const char *text = json_object_has_member(part, "text")
        ? json_object_get_string_member(part, "text") : NULL;
    if (text && *text) {
        (thought ? g->base.events.note : g->base.events.text)(text, g->base.data);
    }
    guint n = json_array_get_length(g->parts);
    JsonObject *last = n ? json_array_get_object_element(g->parts, n - 1) : NULL;
    if (last && plain_text(part) && plain_text(last)
            && thought == json_object_get_boolean_member_with_default(last, "thought", FALSE)) {
        char *joined = g_strconcat(json_object_get_string_member(last, "text"), text, NULL);
        json_object_set_string_member(last, "text", joined);
        g_free(joined);
        return;
    }
    json_array_add_object_element(g->parts, json_object_ref(part));
}

static void on_event(const char *event, const char *data, gpointer user)
{
    Gemini *g          = user;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, NULL)
            || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_object_unref(parser);
        return;
    }
    JsonObject *chunk = json_node_get_object(json_parser_get_root(parser));
    JsonNode *node;
    if ((node = json_object_get_member(chunk, "error")) && JSON_NODE_HOLDS_OBJECT(node)) {
        g_free(g->stream_error);
        g->stream_error = g_strdup(json_object_get_string_member_with_default(
                    json_node_get_object(node), "message", "the API failed"));
    }
    if ((node = json_object_get_member(chunk, "promptFeedback")) && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *feedback = json_node_get_object(node);
        if (json_object_has_member(feedback, "blockReason")) {
            g_free(g->blocked);
            g->blocked = g_strdup(json_object_get_string_member(feedback, "blockReason"));
        }
    }
    JsonArray *candidates = (node = json_object_get_member(chunk, "candidates"))
        && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    JsonObject *candidate = candidates && json_array_get_length(candidates)
        ? json_array_get_object_element(candidates, 0) : NULL;
    if (candidate) {
        JsonObject *content = (node = json_object_get_member(candidate, "content"))
            && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        JsonArray *parts = content && (node = json_object_get_member(content, "parts"))
            && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
        for (guint i = 0; parts && i < json_array_get_length(parts); i++) {
            JsonNode *p = json_array_get_element(parts, i);
            if (JSON_NODE_HOLDS_OBJECT(p)) {
                add_part(g, json_node_get_object(p));
            }
        }
        if ((node = json_object_get_member(candidate, "finishReason"))
                && JSON_NODE_HOLDS_VALUE(node)) {
            g_free(g->finish);
            g->finish = g_strdup(json_node_get_string(node));
        }
    }
    g_object_unref(parser);
}

static gboolean retry(gpointer data)
{
    Gemini *g = data;
    g->retry_source = 0;
    request(g);
    return G_SOURCE_REMOVE;
}

static gboolean refused(const char *finish)
{
    static const char *reasons[] = {"SAFETY", "RECITATION", "BLOCKLIST", "PROHIBITED_CONTENT",
        "SPII", "IMAGE_SAFETY", "IMAGE_PROHIBITED_CONTENT", "IMAGE_RECITATION", NULL};
    for (int i = 0; reasons[i]; i++) {
        if (!strcmp(finish, reasons[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static void on_done(guint status, const char *error, gpointer user)
{
    Gemini *g = user;
    g_clear_object(&g->cancellable);
    if (!error && g->stream_error) {
        error  = g->stream_error;
        status = 503;
    }
    if (error && g->attempt < ATTEMPTS && !json_array_get_length(g->parts)
            && (status == 0 || status == 408 || status == 429 || status >= 500)) {
        guint delay = 2u << g->attempt;
        g_message("assistant: %s; trying again in %u s", error, delay);
        g_clear_pointer(&g->stream_error, g_free);
        json_array_unref(g->parts);
        g->parts = json_array_new();
        g->retry_source = g_timeout_add_seconds(delay, retry, g);
        return;
    }

    char *why = NULL;
    const char *stop = NULL;
    GPtrArray *calls = NULL;
    if (error) {
        why = g_strdup(error);
    } else if (g->blocked || (g->finish && refused(g->finish))) {
        stop = "refusal";           /* the partial reply isn't kept */
    } else if (!g->finish) {
        why = g_strdup("the reply was cut off");
    } else if (strcmp(g->finish, "STOP") && strcmp(g->finish, "MAX_TOKENS")
            && strcmp(g->finish, "CONTINUATION")) {
        why = g_strdup_printf("the model's reply failed (%s)", g->finish);
    } else {
        if (json_array_get_length(g->parts)) {
            JsonObject *c = json_object_new();
            json_object_set_string_member(c, "role", "model");
            json_object_set_array_member(c, "parts", json_array_ref(g->parts));
            json_array_add_object_element(g->contents, c);
        }
        calls = g_ptr_array_new_with_free_func((GDestroyNotify)backend_tool_call_free);
        for (guint i = 0; i < json_array_get_length(g->parts); i++) {
            JsonObject *p = json_array_get_object_element(g->parts, i);
            JsonNode *fc  = json_object_get_member(p, "functionCall");
            if (!fc || !JSON_NODE_HOLDS_OBJECT(fc)) {
                continue;
            }
            JsonObject *f = json_node_get_object(fc);
            JsonNode *args = json_object_get_member(f, "args");
            BackendToolCall *call = g_new0(BackendToolCall, 1);
            call->id    = g_strdup(json_object_get_string_member_with_default(f, "id", ""));
            call->name  = g_strdup(json_object_get_string_member_with_default(f, "name", ""));
            call->input = args && JSON_NODE_HOLDS_OBJECT(args)
                ? json_object_ref(json_node_get_object(args)) : json_object_new();
            g_ptr_array_add(calls, call);
        }
        /* calls cut off by MAX_TOKENS are answered as not run, next time */
        stop = !strcmp(g->finish, "MAX_TOKENS") ? "max_tokens" : calls->len ? "tool_use" : "end_turn";
    }
    char *stop_copy = g_strdup(stop);
    turn_reset(g);
    gboolean run = calls && calls->len && !g_strcmp0(stop_copy, "tool_use");
    g->base.events.turn_done(run ? calls : NULL, stop_copy, why, g->base.data);
    if (calls) {
        g_ptr_array_unref(calls);
    }
    g_free(stop_copy);
    g_free(why);
}

static void request(Gemini *g)
{
    turn_reset(g);
    g->parts = json_array_new();
    g->attempt++;

    JsonObject *thinking = json_object_new();
    json_object_set_string_member(thinking, "thinkingLevel", g->level);
    json_object_set_boolean_member(thinking, "includeThoughts", TRUE);
    JsonObject *generation = json_object_new();
    json_object_set_object_member(generation, "thinkingConfig", thinking);

    JsonObject *body = json_object_new();
    json_object_set_object_member(body, "systemInstruction", json_object_ref(g->system));
    json_object_set_array_member(body, "contents", json_array_ref(g->contents));
    json_object_set_array_member(body, "tools", json_array_ref(g->tools));
    json_object_set_object_member(body, "generationConfig", generation);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, body);

    SoupMessage *message = soup_message_new(SOUP_METHOD_POST, g->url);
    soup_message_headers_replace(soup_message_get_request_headers(message), "x-goog-api-key", g->key);
    backend_set_json_body(message, node);
    json_node_unref(node);

    g->cancellable = g_cancellable_new();
    backend_sse(g->session, message, g->cancellable, on_event, on_done, g);
    g_object_unref(message);
}

static void gemini_cancel(Backend *b)
{
    turn_reset((Gemini *)b);
}

static void gemini_free(Backend *b)
{
    Gemini *g = (Gemini *)b;
    turn_reset(g);
    g_object_unref(g->session);
    g_free(g->url);
    if (g->key) {
        memset(g->key, 0, strlen(g->key));
    }
    g_free(g->key);
    g_free(g->level);
    json_object_unref(g->system);
    json_array_unref(g->tools);
    json_array_unref(g->contents);
    g_free(g);
}

/* How hard to think, as the Gemini models take it: low, medium and high
 * all of them do; xhigh only some, so more than high is high. */
static const char *thinking_level(const char *effort)
{
    if (!effort || !*effort || !strcmp(effort, "medium")) {
        return "medium";
    }
    return !strcmp(effort, "low") ? "low" : "high";
}

Backend *backend_gemini_new(const BackendConfig *config, const BackendEvents *events,
        gpointer data, GError **error)
{
    const char *key = g_getenv("GEMINI_API_KEY");
    if (!key || !*key) {
        key = g_getenv("GOOGLE_API_KEY");
    }
    if (!key || !*key) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "the Gemini assistant needs an API key in GEMINI_API_KEY");
        return NULL;
    }
    const char *base  = g_getenv("GEMINI_BASE_URL");
    const char *model = config->model && *config->model ? config->model : DEFAULT_MODEL;
    Gemini *g = g_new0(Gemini, 1);
    g->base.name         = "gemini";
    g->base.send         = gemini_send;
    g->base.send_results = gemini_send_results;
    g->base.cancel       = gemini_cancel;
    g->base.free         = gemini_free;
    g->base.events       = *events;
    g->base.data         = data;
    g->key   = g_strdup(key);
    g->url   = g_strdup_printf("%s/v1beta/models/%s:streamGenerateContent?alt=sse",
            base && *base ? base : DEFAULT_URL, model);
    g->level = g_strdup(thinking_level(config->effort));
    g->session  = soup_session_new_with_options("user-agent", "vimb/" VERSION, "timeout", 300, NULL);
    g->contents = json_array_new();

    JsonObject *text = json_object_new();
    json_object_set_string_member(text, "text", config->system);
    JsonArray *parts = json_array_new();
    json_array_add_object_element(parts, text);
    g->system = json_object_new();
    json_object_set_array_member(g->system, "parts", parts);

    /* the tools, as functions; one without parameters says none */
    JsonArray *functions = json_array_new();
    JsonArray *list = json_node_get_array(config->tools);
    for (guint i = 0; i < json_array_get_length(list); i++) {
        JsonObject *t      = json_array_get_object_element(list, i);
        JsonObject *schema = json_object_get_object_member(t, "inputSchema");
        JsonObject *f      = json_object_new();
        json_object_set_string_member(f, "name", json_object_get_string_member(t, "name"));
        json_object_set_string_member(f, "description",
                json_object_get_string_member(t, "description"));
        JsonObject *props = schema && json_object_has_member(schema, "properties")
            ? json_object_get_object_member(schema, "properties") : NULL;
        if (props && json_object_get_size(props)) {
            json_object_set_member(f, "parameters",
                    json_node_copy(json_object_get_member(t, "inputSchema")));
        }
        json_array_add_object_element(functions, f);
    }
    JsonObject *tool = json_object_new();
    json_object_set_array_member(tool, "functionDeclarations", functions);
    g->tools = json_array_new();
    json_array_add_object_element(g->tools, tool);
    return &g->base;
}
