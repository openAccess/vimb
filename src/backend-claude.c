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

/* The assistant on Claude, by the Messages API, streamed.
 *
 * The conversation is kept as the API has it, and only grows: the replies
 * go back exactly as they came, thinking blocks and all, which the model
 * needs to carry its reasoning from one turn to the next. Words for a turn
 * that never got its reply -- cancelled, refused, failed -- join the next
 * turn's; tool calls left unanswered are answered as not run. */

#include <string.h>

#include "../version.h"
#include "backend.h"

#define DEFAULT_MODEL   "claude-opus-5-5"
#define DEFAULT_URL     "https://api.anthropic.com"
#define API_VERSION     "2026-10-21"
/* a declined request is retried on the model Anthropic picks; the notes the
 * model makes between tool calls come as text */
#define BETAS           "server-side-fallback-2026-07-01,thinking-display-updates-2026-08-18"
#define MAX_TOKENS      64000
#define ATTEMPTS        4

typedef struct {
    JsonObject *block;      /* as content_block_start gave it */
    GString    *text;
    GString    *thinking;
    GString    *signature;
    GString    *json;       /* a tool call's input, in pieces */
    JsonArray  *citations;
    gboolean   done;
} Block;

typedef struct {
    Backend      base;
    SoupSession  *session;
    char         *url;
    char         *key;
    char         *model;
    char         *effort;
    char         *system;
    JsonArray    *tools;
    JsonArray    *messages;
    /* the turn */
    GCancellable *cancellable;
    GPtrArray    *blocks;
    char         *stop;
    char         *stream_error;
    guint        attempt;
    guint        retry_source;
    GPtrArray    *calls;
} Claude;

static void request(Claude *k);

/* ---- the conversation -------------------------------------------------- */

static JsonObject *last_message(Claude *k, const char *role)
{
    guint n = json_array_get_length(k->messages);
    if (!n) {
        return NULL;
    }
    JsonObject *m = json_array_get_object_element(k->messages, n - 1);
    return !g_strcmp0(json_object_get_string_member(m, "role"), role) ? m : NULL;
}

/* Adds content blocks for the user: to the last message if that is the
 * user's still (its turn never answered), else as a message of its own. */
static void add_user_blocks(Claude *k, JsonArray *blocks)
{
    JsonObject *last = last_message(k, "user");
    if (last) {
        JsonArray *content = json_object_get_array_member(last, "content");
        for (guint i = 0; i < json_array_get_length(blocks); i++) {
            json_array_add_element(content, json_node_copy(json_array_get_element(blocks, i)));
        }
        json_array_unref(blocks);
        return;
    }
    JsonObject *m = json_object_new();
    json_object_set_string_member(m, "role", "user");
    json_object_set_array_member(m, "content", blocks);
    json_array_add_object_element(k->messages, m);
}

static JsonObject *tool_result(const char *id, gboolean error, const char *text)
{
    JsonObject *r = json_object_new();
    json_object_set_string_member(r, "type", "tool_result");
    json_object_set_string_member(r, "tool_use_id", id);
    json_object_set_string_member(r, "content", text);
    if (error) {
        json_object_set_boolean_member(r, "is_error", TRUE);
    }
    return r;
}

/* Answers, as not run, the tool calls of the last reply if nothing has. */
static void answer_unanswered(Claude *k, JsonArray *blocks)
{
    JsonObject *last = last_message(k, "assistant");
    JsonArray *content = last ? json_object_get_array_member(last, "content") : NULL;
    for (guint i = 0; content && i < json_array_get_length(content); i++) {
        JsonObject *b = json_array_get_object_element(content, i);
        if (!g_strcmp0(json_object_get_string_member_with_default(b, "type", ""), "tool_use")) {
            json_array_add_object_element(blocks, tool_result(json_object_get_string_member(b, "id"),
                    TRUE, "not run: the turn was stopped first"));
        }
    }
}

static void claude_send(Backend *b, const char *text)
{
    Claude *k         = (Claude *)b;
    JsonArray *blocks = json_array_new();
    answer_unanswered(k, blocks);
    JsonObject *t = json_object_new();
    json_object_set_string_member(t, "type", "text");
    json_object_set_string_member(t, "text", text);
    json_array_add_object_element(blocks, t);
    add_user_blocks(k, blocks);
    k->attempt = 0;
    request(k);
}

static void claude_send_results(Backend *b, GPtrArray *results)
{
    Claude *k         = (Claude *)b;
    JsonArray *blocks = json_array_new();
    for (guint i = 0; i < results->len; i++) {
        BackendToolResult *r = g_ptr_array_index(results, i);
        if (r->image_data) {
            JsonObject *source = json_object_new();
            json_object_set_string_member(source, "type", "base64");
            json_object_set_string_member(source, "media_type", r->image_mime);
            json_object_set_string_member(source, "data", r->image_data);
            JsonObject *image = json_object_new();
            json_object_set_string_member(image, "type", "image");
            json_object_set_object_member(image, "source", source);
            JsonArray *content = json_array_new();
            json_array_add_object_element(content, image);
            JsonObject *result = tool_result(r->id, FALSE, "");
            json_object_set_array_member(result, "content", content);
            json_array_add_object_element(blocks, result);
        } else {
            json_array_add_object_element(blocks, tool_result(r->id, !r->ok, r->text ? r->text : ""));
        }
    }
    add_user_blocks(k, blocks);
    k->attempt = 0;
    request(k);
}

/* ---- the turn ---------------------------------------------------------- */

static void block_free(Block *bl)
{
    json_object_unref(bl->block);
    GString *strings[] = {bl->text, bl->thinking, bl->signature, bl->json};
    for (gsize i = 0; i < G_N_ELEMENTS(strings); i++) {
        if (strings[i]) {
            g_string_free(strings[i], TRUE);
        }
    }
    if (bl->citations) {
        json_array_unref(bl->citations);
    }
    g_free(bl);
}

static void turn_reset(Claude *k)
{
    if (k->cancellable) {
        g_cancellable_cancel(k->cancellable);
        g_clear_object(&k->cancellable);
    }
    if (k->retry_source) {
        g_source_remove(k->retry_source);
        k->retry_source = 0;
    }
    g_ptr_array_set_size(k->blocks, 0);
    g_clear_pointer(&k->stop, g_free);
    g_clear_pointer(&k->stream_error, g_free);
    g_ptr_array_set_size(k->calls, 0);
}

static Block *block_at(Claude *k, JsonObject *event)
{
    gint64 i = json_object_get_int_member_with_default(event, "index", -1);
    return i >= 0 && i < k->blocks->len ? g_ptr_array_index(k->blocks, i) : NULL;
}

static GString *grow(GString **s, const char *more)
{
    if (!*s) {
        *s = g_string_new(NULL);
    }
    return g_string_append(*s, more ? more : "");
}

/* Puts what came in pieces into the block, as the API wants it back. */
static void block_finish(Block *bl, BackendToolCall **call)
{
    if (bl->done) {
        return;
    }
    bl->done = TRUE;
    if (bl->text) {
        json_object_set_string_member(bl->block, "text", bl->text->str);
    }
    if (bl->thinking) {
        json_object_set_string_member(bl->block, "thinking", bl->thinking->str);
    }
    if (bl->signature) {
        json_object_set_string_member(bl->block, "signature", bl->signature->str);
    }
    if (bl->citations) {
        json_object_set_array_member(bl->block, "citations", json_array_ref(bl->citations));
    }
    const char *type = json_object_get_string_member_with_default(bl->block, "type", "");
    if (strcmp(type, "tool_use")) {
        return;
    }
    /* The input streams unchecked (eager_input_streaming), so it is parsed
     * strictly here; what doesn't parse to an object isn't run. */
    *call = g_new0(BackendToolCall, 1);
    (*call)->id   = g_strdup(json_object_get_string_member_with_default(bl->block, "id", ""));
    (*call)->name = g_strdup(json_object_get_string_member_with_default(bl->block, "name", ""));
    if (bl->json && bl->json->len) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, bl->json->str, bl->json->len, NULL)
                && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            (*call)->input = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
            json_object_set_object_member(bl->block, "input", json_object_ref((*call)->input));
        } else {
            (*call)->raw = g_strdup(bl->json->str);
            json_object_set_object_member(bl->block, "input", json_object_new());
        }
        g_object_unref(parser);
    } else if (json_object_has_member(bl->block, "input")
            && JSON_NODE_HOLDS_OBJECT(json_object_get_member(bl->block, "input"))) {
        (*call)->input = json_object_ref(json_object_get_object_member(bl->block, "input"));
    } else {
        (*call)->input = json_object_new();
        json_object_set_object_member(bl->block, "input", json_object_ref((*call)->input));
    }
}

static void on_event(const char *event, const char *data, gpointer user)
{
    Claude *k          = user;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, NULL)
            || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_object_unref(parser);
        return;
    }
    JsonObject *e    = json_node_get_object(json_parser_get_root(parser));
    const char *type = json_object_get_string_member_with_default(e, "type", event);

    if (!strcmp(type, "content_block_start")) {
        gint64 i = json_object_get_int_member_with_default(e, "index", -1);
        JsonNode *cb = json_object_get_member(e, "content_block");
        if (i == k->blocks->len && cb && JSON_NODE_HOLDS_OBJECT(cb)) {
            Block *bl = g_new0(Block, 1);
            bl->block = json_object_ref(json_node_get_object(cb));
            g_ptr_array_add(k->blocks, bl);
            const char *bt = json_object_get_string_member_with_default(bl->block, "type", "");
            /* what a block starts with, it keeps */
            if (!strcmp(bt, "text")) {
                const char *t = json_object_get_string_member_with_default(bl->block, "text", "");
                grow(&bl->text, t);
                if (*t) {
                    k->base.events.text(t, k->base.data);
                }
            } else if (!strcmp(bt, "thinking")) {
                grow(&bl->thinking,
                        json_object_get_string_member_with_default(bl->block, "thinking", ""));
                grow(&bl->signature,
                        json_object_get_string_member_with_default(bl->block, "signature", ""));
            }
        }
    } else if (!strcmp(type, "content_block_delta")) {
        Block *bl      = block_at(k, e);
        JsonNode *node = json_object_get_member(e, "delta");
        JsonObject *d  = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        const char *dt = d ? json_object_get_string_member_with_default(d, "type", "") : "";
        if (!bl || !d) {
            /* nothing to add to */
        } else if (!strcmp(dt, "text_delta")) {
            const char *t = json_object_get_string_member_with_default(d, "text", "");
            grow(&bl->text, t);
            k->base.events.text(t, k->base.data);
        } else if (!strcmp(dt, "thinking_delta")) {
            const char *t = json_object_get_string_member_with_default(d, "thinking", "");
            grow(&bl->thinking, t);
            k->base.events.note(t, k->base.data);
        } else if (!strcmp(dt, "signature_delta")) {
            grow(&bl->signature, json_object_get_string_member_with_default(d, "signature", ""));
        } else if (!strcmp(dt, "input_json_delta")) {
            grow(&bl->json, json_object_get_string_member_with_default(d, "partial_json", ""));
        } else if (!strcmp(dt, "citations_delta") && json_object_has_member(d, "citation")) {
            if (!bl->citations) {
                bl->citations = json_array_new();
            }
            json_array_add_element(bl->citations,
                    json_node_copy(json_object_get_member(d, "citation")));
        }
    } else if (!strcmp(type, "content_block_stop")) {
        Block *bl = block_at(k, e);
        BackendToolCall *call = NULL;
        if (bl) {
            block_finish(bl, &call);
            if (call) {
                g_ptr_array_add(k->calls, call);
            }
            const char *bt = json_object_get_string_member_with_default(bl->block, "type", "");
            if (!strcmp(bt, "thinking") && bl->thinking && bl->thinking->len) {
                k->base.events.note("\n", k->base.data);
            }
        }
    } else if (!strcmp(type, "message_delta")) {
        JsonNode *node = json_object_get_member(e, "delta");
        if (node && JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *d = json_node_get_object(node);
            JsonNode *stop = json_object_get_member(d, "stop_reason");
            if (stop && JSON_NODE_HOLDS_VALUE(stop)) {
                g_free(k->stop);
                k->stop = g_strdup(json_node_get_string(stop));
            }
        }
    } else if (!strcmp(type, "error")) {
        JsonNode *node = json_object_get_member(e, "error");
        const char *message = node && JSON_NODE_HOLDS_OBJECT(node)
            ? json_object_get_string_member_with_default(json_node_get_object(node), "message", "")
            : "";
        g_free(k->stream_error);
        k->stream_error = g_strdup(*message ? message : "the API failed");
    }
    g_object_unref(parser);
}

static gboolean retry(gpointer data)
{
    Claude *k = data;
    k->retry_source = 0;
    request(k);
    return G_SOURCE_REMOVE;
}

/* Whether a failure is worth another try: too many requests, overloaded,
 * a server's error, or no connection -- and nothing came of the turn yet. */
static gboolean worth_retrying(Claude *k, guint status)
{
    return k->attempt < ATTEMPTS && !k->blocks->len
        && (status == 0 || status == 408 || status == 429 || status >= 500);
}

static void on_done(guint status, const char *error, gpointer user)
{
    Claude *k = user;
    g_clear_object(&k->cancellable);
    if (!error && k->stream_error) {
        error  = k->stream_error;
        status = 529;               /* overloaded, as a rule */
    }
    if (error && worth_retrying(k, status)) {
        guint delay = 2u << k->attempt;
        g_message("assistant: %s; trying again in %u s", error, delay);
        g_ptr_array_set_size(k->blocks, 0);
        g_clear_pointer(&k->stream_error, g_free);
        k->retry_source = g_timeout_add_seconds(delay, retry, k);
        return;
    }
    if (error || !k->stop) {
        char *why = g_strdup(error ? error : "the reply was cut off");
        turn_reset(k);
        k->base.events.turn_done(NULL, NULL, why, k->base.data);
        g_free(why);
        return;
    }

    /* a refusal's partial reply isn't kept: the words that drew it join the
     * next turn's, unless the conversation is begun again */
    char *stop = g_strdup(k->stop);
    if (strcmp(stop, "refusal")) {
        JsonArray *content = json_array_new();
        for (guint i = 0; i < k->blocks->len; i++) {
            Block *bl = g_ptr_array_index(k->blocks, i);
            BackendToolCall *call = NULL;
            block_finish(bl, &call);
            if (call) {
                g_ptr_array_add(k->calls, call);
            }
            json_array_add_object_element(content, json_object_ref(bl->block));
        }
        if (json_array_get_length(content)) {
            JsonObject *m = json_object_new();
            json_object_set_string_member(m, "role", "assistant");
            json_object_set_array_member(m, "content", content);
            json_array_add_object_element(k->messages, m);
        } else {
            json_array_unref(content);
        }
    }
    /* tool calls cut off by max_tokens aren't run, but answered as not run */
    gboolean run = !strcmp(stop, "tool_use") && k->calls->len;
    GPtrArray *calls = k->calls;
    k->calls = g_ptr_array_new_with_free_func((GDestroyNotify)backend_tool_call_free);
    turn_reset(k);
    k->base.events.turn_done(run ? calls : NULL, stop, NULL, k->base.data);
    g_ptr_array_unref(calls);
    g_free(stop);
}

static void request(Claude *k)
{
    turn_reset(k);
    k->attempt++;

    JsonObject *thinking = json_object_new();
    json_object_set_string_member(thinking, "type", "adaptive");
    json_object_set_string_member(thinking, "display", "updates");
    JsonObject *output = json_object_new();
    json_object_set_string_member(output, "effort", k->effort);
    JsonObject *cache = json_object_new();
    json_object_set_string_member(cache, "type", "ephemeral");

    JsonObject *body = json_object_new();
    json_object_set_string_member(body, "model", k->model);
    json_object_set_int_member(body, "max_tokens", MAX_TOKENS);
    json_object_set_boolean_member(body, "stream", TRUE);
    json_object_set_string_member(body, "system", k->system);
    json_object_set_array_member(body, "tools", json_array_ref(k->tools));
    json_object_set_array_member(body, "messages", json_array_ref(k->messages));
    json_object_set_object_member(body, "thinking", thinking);
    json_object_set_object_member(body, "output_config", output);
    json_object_set_string_member(body, "fallbacks", "default");
    json_object_set_object_member(body, "cache_control", cache);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, body);

    SoupMessage *message = soup_message_new(SOUP_METHOD_POST, k->url);
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_replace(headers, "x-api-key", k->key);
    soup_message_headers_replace(headers, "anthropic-version", API_VERSION);
    soup_message_headers_replace(headers, "anthropic-beta", BETAS);
    backend_set_json_body(message, node);
    json_node_unref(node);

    k->cancellable = g_cancellable_new();
    backend_sse(k->session, message, k->cancellable, on_event, on_done, k);
    g_object_unref(message);
}

static void claude_cancel(Backend *b)
{
    turn_reset((Claude *)b);
}

static void claude_free(Backend *b)
{
    Claude *k = (Claude *)b;
    turn_reset(k);
    g_ptr_array_unref(k->blocks);
    g_ptr_array_unref(k->calls);
    g_object_unref(k->session);
    g_free(k->url);
    if (k->key) {
        memset(k->key, 0, strlen(k->key));
    }
    g_free(k->key);
    g_free(k->model);
    g_free(k->effort);
    g_free(k->system);
    json_array_unref(k->tools);
    json_array_unref(k->messages);
    g_free(k);
}

Backend *backend_claude_new(const BackendConfig *config, const BackendEvents *events,
        gpointer data, GError **error)
{
    const char *key = g_getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "the Claude assistant needs an API key in ANTHROPIC_API_KEY");
        return NULL;
    }
    const char *base = g_getenv("ANTHROPIC_BASE_URL");
    Claude *k = g_new0(Claude, 1);
    k->base.name         = "claude";
    k->base.send         = claude_send;
    k->base.send_results = claude_send_results;
    k->base.cancel       = claude_cancel;
    k->base.free         = claude_free;
    k->base.events       = *events;
    k->base.data         = data;
    k->key    = g_strdup(key);
    k->url    = g_strconcat(base && *base ? base : DEFAULT_URL, "/v1/messages", NULL);
    k->model  = g_strdup(config->model && *config->model ? config->model : DEFAULT_MODEL);
    k->effort = g_strdup(config->effort && *config->effort ? config->effort : "medium");
    k->system = g_strdup(config->system);
    k->session = soup_session_new_with_options("user-agent", "vimb/" VERSION, "timeout", 300, NULL);
    k->messages = json_array_new();
    k->blocks   = g_ptr_array_new_with_free_func((GDestroyNotify)block_free);
    k->calls    = g_ptr_array_new_with_free_func((GDestroyNotify)backend_tool_call_free);

    /* the tools, as the API takes them; their input streams as it is made */
    k->tools = json_array_new();
    JsonArray *list = json_node_get_array(config->tools);
    for (guint i = 0; i < json_array_get_length(list); i++) {
        JsonObject *t = json_array_get_object_element(list, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "name", json_object_get_string_member(t, "name"));
        json_object_set_string_member(o, "description",
                json_object_get_string_member(t, "description"));
        json_object_set_member(o, "input_schema",
                json_node_copy(json_object_get_member(t, "inputSchema")));
        json_object_set_boolean_member(o, "eager_input_streaming", TRUE);
        json_array_add_object_element(k->tools, o);
    }
    return &k->base;
}
