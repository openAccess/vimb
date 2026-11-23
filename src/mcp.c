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
#include <gio/gunixinputstream.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "../version.h"
#include "archive/netcheck.h"
#include "mcp.h"

/* JSON-RPC's error codes. */
#define PARSE_ERROR      -32700
#define INVALID_REQUEST  -32600
#define METHOD_NOT_FOUND -32601
#define INVALID_PARAMS   -32602

#define HTTP_PATH        "/mcp"
#define KEEPALIVE_S      15
#define SESSIONS_MAX     64

static const char *versions[] = {   /* newest first */
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL
};

static const char instructions[] =
    "vimb is a web browser that archives what it loads as WACZ web archives. Tabs and "
    "archives are named by number. To archive pages: start_archive, then open_page with the "
    "archive's number (and scroll_page, so that what loads only as it is read is archived "
    "too), or crawl from seeds; then stop_archive, which saves the WACZ and says where. "
    "read_page, page_links and screenshot tell what a tab shows.";

/* Where an answer goes: takes message. */
typedef void (*Respond)(JsonObject *message, gpointer data);

typedef struct {
    char       *id;             /* over HTTP, its Mcp-Session-Id */
    const char *version;        /* as negotiated */
} Session;

static ToolPolicy policy;

static const char *supported_version(const char *asked)
{
    for (int i = 0; asked && versions[i]; i++) {
        if (!strcmp(asked, versions[i])) {
            return versions[i];
        }
    }
    return NULL;
}

/* ---- messages ----------------------------------------------------------- */

static JsonObject *envelope(JsonNode *id)
{
    JsonObject *message = json_object_new();
    json_object_set_string_member(message, "jsonrpc", "2.0");
    json_object_set_member(message, "id", id ? json_node_copy(id) : json_node_new(JSON_NODE_NULL));
    return message;
}

/* Takes result. */
static JsonObject *result_message(JsonNode *id, JsonObject *result)
{
    JsonObject *message = envelope(id);
    json_object_set_object_member(message, "result", result);
    return message;
}

static JsonObject *error_message(JsonNode *id, int code, const char *text)
{
    JsonObject *error = json_object_new();
    json_object_set_int_member(error, "code", code);
    json_object_set_string_member(error, "message", text);
    JsonObject *message = envelope(id);
    json_object_set_object_member(message, "error", error);
    return message;
}

/* Takes message. */
static char *message_text(JsonObject *message)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, message);
    char *text = json_to_string(node, FALSE);
    json_node_unref(node);
    return text;
}

/* ---- the methods -------------------------------------------------------- */

static JsonObject *initialize(Session *s, JsonObject *params)
{
    const char *asked = params && json_object_has_member(params, "protocolVersion")
        ? json_object_get_string_member(params, "protocolVersion") : NULL;
    s->version = supported_version(asked);
    if (!s->version) {
        s->version = versions[0];
    }
    JsonObject *tools = json_object_new();
    json_object_set_boolean_member(tools, "listChanged", FALSE);
    JsonObject *capabilities = json_object_new();
    json_object_set_object_member(capabilities, "tools", tools);
    JsonObject *info = json_object_new();
    json_object_set_string_member(info, "name", "vimb");
    json_object_set_string_member(info, "version", VERSION);
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "protocolVersion", s->version);
    json_object_set_object_member(result, "capabilities", capabilities);
    json_object_set_object_member(result, "serverInfo", info);
    json_object_set_string_member(result, "instructions", instructions);
    return result;
}

static gboolean tool_exists(const char *name)
{
    JsonNode *list = tools_list(&policy);
    JsonArray *array = json_node_get_array(list);
    gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(array) && !found; i++) {
        JsonObject *tool = json_array_get_object_element(array, i);
        found = !g_strcmp0(json_object_get_string_member(tool, "name"), name);
    }
    json_node_unref(list);
    return found;
}

typedef struct {
    JsonNode   *id;
    const char *version;
    Respond    respond;
    gpointer   data;
} Pending;

static void tool_done(const ToolResult *result, gpointer data)
{
    Pending *p = data;
    JsonArray *content = json_array_new();
    JsonObject *item   = json_object_new();
    JsonObject *answer = json_object_new();
    if (result->ok && result->image_data) {
        json_object_set_string_member(item, "type", "image");
        json_object_set_string_member(item, "data", result->image_data);
        json_object_set_string_member(item, "mimeType", result->image_mime);
    } else if (result->ok) {
        char *text = result->value ? json_to_string(result->value, FALSE) : g_strdup("null");
        json_object_set_string_member(item, "type", "text");
        json_object_set_string_member(item, "text", text);
        g_free(text);
        /* structured results came in with 2025-06-18 */
        if (result->value && JSON_NODE_HOLDS_OBJECT(result->value)
                && strcmp(p->version, "2025-06-18") >= 0) {
            json_object_set_member(answer, "structuredContent", json_node_copy(result->value));
        }
    } else {
        json_object_set_string_member(item, "type", "text");
        json_object_set_string_member(item, "text", result->error);
    }
    json_array_add_object_element(content, item);
    json_object_set_array_member(answer, "content", content);
    json_object_set_boolean_member(answer, "isError", !result->ok);
    p->respond(result_message(p->id, answer), p->data);
    json_node_unref(p->id);
    g_free(p);
}

static void call_tool(Session *s, JsonNode *id, JsonObject *params, Respond respond, gpointer data)
{
    JsonNode *name = params ? json_object_get_member(params, "name") : NULL;
    JsonNode *args = params ? json_object_get_member(params, "arguments") : NULL;
    if (!name || !JSON_NODE_HOLDS_VALUE(name) || json_node_get_value_type(name) != G_TYPE_STRING) {
        respond(error_message(id, INVALID_PARAMS, "tools/call needs the tool's name"), data);
        return;
    }
    if (args && !JSON_NODE_HOLDS_OBJECT(args) && !JSON_NODE_HOLDS_NULL(args)) {
        respond(error_message(id, INVALID_PARAMS, "a tool's arguments are an object"), data);
        return;
    }
    if (!tool_exists(json_node_get_string(name))) {
        char *text = g_strdup_printf("Unknown tool: %s", json_node_get_string(name));
        respond(error_message(id, INVALID_PARAMS, text), data);
        g_free(text);
        return;
    }
    Pending *p = g_new0(Pending, 1);
    p->id      = json_node_copy(id);
    p->version = s->version;
    p->respond = respond;
    p->data    = data;
    tools_call(json_node_get_string(name),
            args && JSON_NODE_HOLDS_OBJECT(args) ? json_node_get_object(args) : NULL,
            &policy, tool_done, p);
}

/* What a message is: whether it asks for an answer, and its method. */
static gboolean is_request(JsonNode *node, const char **method)
{
    JsonObject *m = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    JsonNode *mn  = m ? json_object_get_member(m, "method") : NULL;
    *method = mn && JSON_NODE_HOLDS_VALUE(mn) && json_node_get_value_type(mn) == G_TYPE_STRING
        ? json_node_get_string(mn) : NULL;
    return m && json_object_has_member(m, "id") && *method;
}

/* Handles a message. A request is answered by respond, once -- now or later;
 * a notification, or a response (vimb asks nothing), isn't. */
static void handle(Session *s, JsonNode *node, Respond respond, gpointer data)
{
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        respond(error_message(NULL, INVALID_REQUEST, "a message is a JSON object"), data);
        return;
    }
    JsonObject *message = json_node_get_object(node);
    JsonNode *id        = json_object_get_member(message, "id");
    JsonNode *method    = json_object_get_member(message, "method");
    JsonNode *params    = json_object_get_member(message, "params");
    JsonObject *p       = params && JSON_NODE_HOLDS_OBJECT(params) ? json_node_get_object(params)
        : NULL;
    if (!method) {
        return;                     /* a response */
    }
    if (!JSON_NODE_HOLDS_VALUE(method) || json_node_get_value_type(method) != G_TYPE_STRING) {
        if (id) {
            respond(error_message(id, INVALID_REQUEST, "method must be a string"), data);
        }
        return;
    }
    const char *name = json_node_get_string(method);
    if (!id) {
        return;                     /* notifications: initialized, cancelled -- none needs doing */
    }
    if (!strcmp(name, "initialize")) {
        respond(result_message(id, initialize(s, p)), data);
    } else if (!strcmp(name, "ping")) {
        respond(result_message(id, json_object_new()), data);
    } else if (!strcmp(name, "tools/list")) {
        JsonObject *result = json_object_new();
        json_object_set_member(result, "tools", tools_list(&policy));
        respond(result_message(id, result), data);
    } else if (!strcmp(name, "tools/call")) {
        call_tool(s, id, p, respond, data);
    } else {
        /* server/discover among them: the 2026 protocol, whose clients then
         * fall back to initialize */
        char *text = g_strdup_printf("Method not found: %s", name);
        respond(error_message(id, METHOD_NOT_FOUND, text), data);
        g_free(text);
    }
}

/* ---- stdio -------------------------------------------------------------- */

typedef struct {
    GDataInputStream *in;
    int              out;
    gboolean         broken;        /* out can't be written */
    Session          session;
} Stdio;

static Stdio *stdio;

static void stdio_respond(JsonObject *message, gpointer data)
{
    char *text   = message_text(message);
    gsize length = strlen(text);
    char *line   = g_realloc(text, length + 1);
    line[length++] = '\n';
    for (gsize done = 0; done < length && !stdio->broken;) {
        ssize_t n = write(stdio->out, line + done, length - done);
        if (n > 0) {
            done += n;
        } else if (n < 0 && errno != EINTR) {
            g_message("mcp: can't write: %s", g_strerror(errno));
            stdio->broken = TRUE;
        }
    }
    g_free(line);
}

static void stdio_read_next(void);

static void stdio_finish(void)
{
    g_message("mcp: input closed; saving and quitting");
    tools_release();
    vb_exit(EXIT_SUCCESS);
}

static void on_line(GObject *source, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    gsize length;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, &length,
            &error);
    if (!line) {
        if (error) {
            g_message("mcp: can't read: %s", error->message);
            g_error_free(error);
        }
        stdio_finish();
        return;
    }
    if (length && line[length - 1] == '\r') {
        line[--length] = '\0';
    }
    if (length) {
        JsonParser *parser = json_parser_new();
        if (!g_utf8_validate(line, length, NULL)) {
            stdio_respond(error_message(NULL, PARSE_ERROR, "a message must be UTF-8"), NULL);
        } else if (!json_parser_load_from_data(parser, line, length, &error)) {
            stdio_respond(error_message(NULL, PARSE_ERROR, error->message), NULL);
            g_clear_error(&error);
        } else {
            handle(&stdio->session, json_parser_get_root(parser), stdio_respond, NULL);
        }
        g_object_unref(parser);
    }
    g_free(line);
    if (stdio->broken) {
        stdio_finish();
    } else {
        stdio_read_next();
    }
}

static void stdio_read_next(void)
{
    g_data_input_stream_read_line_async(stdio->in, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

gboolean mcp_serve_stdio(int in_fd, int out_fd, const ToolPolicy *p, GError **error)
{
    if (out_fd == STDOUT_FILENO) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "MCP must not write to stdout itself");
        return FALSE;
    }
    policy = *p;
    stdio  = g_new0(Stdio, 1);
    stdio->out             = out_fd;
    stdio->session.version = versions[0];
    GInputStream *in = g_unix_input_stream_new(in_fd, FALSE);
    stdio->in = g_data_input_stream_new(in);
    g_object_unref(in);
    g_data_input_stream_set_newline_type(stdio->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    stdio_read_next();
    return TRUE;
}

/* ---- streamable HTTP ---------------------------------------------------- */

typedef struct {
    SoupServer *server;
    guint      port;
    GHashTable *sessions;           /* id -> Session */
    GQueue     *order;              /* their ids, oldest first */
} Http;

static Http *http;

/* A request awaiting its answer. */
typedef struct {
    int               refs;
    SoupServerMessage *msg;
    gboolean          gone;         /* the client went, or was answered */
    gboolean          in_handler;   /* answered before the handler returns: no pause */
    gboolean          answered;
    gboolean          sse;          /* the answer streams, as server-sent events */
    guint             keepalive;
    gulong            finished_id;
} Exchange;

static void exchange_unref(Exchange *x)
{
    if (--x->refs) {
        return;
    }
    if (x->keepalive) {
        g_source_remove(x->keepalive);
    }
    if (x->finished_id) {
        g_signal_handler_disconnect(x->msg, x->finished_id);
    }
    g_object_unref(x->msg);
    g_free(x);
}

static void on_finished(SoupServerMessage *msg, gpointer data)
{
    Exchange *x = data;
    x->gone = TRUE;
    if (x->keepalive) {
        g_source_remove(x->keepalive);
        x->keepalive = 0;
    }
}

static void session_free(Session *s)
{
    g_free(s->id);
    g_free(s);
}

static void http_status(SoupServerMessage *msg, guint status, const char *text)
{
    soup_server_message_set_status(msg, status, NULL);
    if (text) {
        soup_server_message_set_response(msg, "text/plain", SOUP_MEMORY_COPY, text, strlen(text));
    }
}

/* Answers with a JSON-RPC message, and an HTTP status. Takes message. */
static void http_json(SoupServerMessage *msg, guint status, JsonObject *message)
{
    char *text = message_text(message);
    soup_server_message_set_status(msg, status, NULL);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_TAKE, text, strlen(text));
}

static void sse_write(Exchange *x, const char *text, gboolean last)
{
    SoupMessageBody *body = soup_server_message_get_response_body(x->msg);
    soup_message_body_append(body, SOUP_MEMORY_COPY, text, strlen(text));
    if (last) {
        soup_message_body_complete(body);
    }
    soup_server_message_unpause(x->msg);
}

/* A comment now and then, so that a long call isn't taken for a dead one. */
static gboolean keepalive(gpointer data)
{
    Exchange *x = data;
    if (!x->gone) {
        sse_write(x, ": keepalive\n\n", FALSE);
    }
    return G_SOURCE_CONTINUE;
}

static void http_respond(JsonObject *message, gpointer data)
{
    Exchange *x = data;
    if (x->gone || x->answered) {
        json_object_unref(message);
        exchange_unref(x);
        return;
    }
    x->answered = TRUE;
    if (x->sse && !x->in_handler) {     /* answered at once, it is plain JSON */
        char *text  = message_text(message);
        char *event = g_strdup_printf("event: message\ndata: %s\n\n", text);
        sse_write(x, event, TRUE);
        g_free(event);
        g_free(text);
        if (x->keepalive) {
            g_source_remove(x->keepalive);
            x->keepalive = 0;
        }
    } else {
        http_json(x->msg, SOUP_STATUS_OK, message);
        if (!x->in_handler) {
            soup_server_message_unpause(x->msg);
        }
    }
    exchange_unref(x);
}

/* Whether a request may be served: from this user's own processes, and not
 * from a web page that has had some name made to point here. */
static gboolean http_allowed(SoupServerMessage *msg, const char **why)
{
    SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
    if (!archive_peer_addresses_same_user(soup_server_message_get_local_address(msg),
                soup_server_message_get_remote_address(msg))) {
        *why = "only this user's programs may use vimb's MCP";
        return FALSE;
    }
    const char *host   = soup_message_headers_get_one(h, "Host");
    const char *origin = soup_message_headers_get_one(h, "Origin");
    gboolean ok = FALSE;
    for (const char **name = (const char *[]){"127.0.0.1", "localhost", "[::1]", NULL};
            *name && !ok; name++) {
        char *want = g_strdup_printf("%s:%u", *name, http->port);
        ok = host && !g_ascii_strcasecmp(host, want);
        g_free(want);
    }
    if (!ok) {
        *why = "the Host isn't this machine";
        return FALSE;
    }
    if (origin && strcmp(origin, "null")) {
        ok = FALSE;
        for (const char **name = (const char *[]){"127.0.0.1", "localhost", "[::1]", NULL};
                *name && !ok; name++) {
            char *want = g_strdup_printf("http://%s:%u", *name, http->port);
            ok = !g_ascii_strcasecmp(origin, want);
            g_free(want);
        }
        if (!ok) {
            *why = "requests from web pages aren't served";
            return FALSE;
        }
    }
    return TRUE;
}

static Session *new_session(void)
{
    Session *s = g_new0(Session, 1);
    guint8 bytes[16];
    for (int i = 0; i < 16; i += 4) {
        guint32 r = g_random_int();
        memcpy(bytes + i, &r, 4);
    }
    GString *id = g_string_new(NULL);
    for (int i = 0; i < 16; i++) {
        g_string_append_printf(id, "%02x", bytes[i]);
    }
    s->id      = g_string_free(id, FALSE);
    s->version = versions[0];
    /* the oldest go, if there come to be many */
    while (g_queue_get_length(http->order) >= SESSIONS_MAX) {
        char *old = g_queue_pop_head(http->order);
        g_hash_table_remove(http->sessions, old);
        g_free(old);
    }
    g_hash_table_insert(http->sessions, s->id, s);
    g_queue_push_tail(http->order, g_strdup(s->id));
    return s;
}

static void http_post(SoupServerMessage *msg)
{
    SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
    SoupMessageBody *body = soup_server_message_get_request_body(msg);
    GBytes *bytes         = soup_message_body_flatten(body);
    gsize length;
    const char *data      = g_bytes_get_data(bytes, &length);
    JsonParser *parser    = json_parser_new();
    GError *error         = NULL;

    if (!length || !g_utf8_validate(data, length, NULL)
            || !json_parser_load_from_data(parser, data, length, &error)) {
        http_json(msg, SOUP_STATUS_BAD_REQUEST, error_message(NULL, PARSE_ERROR,
                    error ? error->message : "not a JSON message"));
        g_clear_error(&error);
        goto out;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_ARRAY(root)) {
        http_json(msg, SOUP_STATUS_BAD_REQUEST, error_message(NULL, INVALID_REQUEST,
                    "batches aren't served"));
        goto out;
    }
    const char *method;
    gboolean request = is_request(root, &method);

    /* the session: begun by initialize, named by every request after */
    Session *s     = NULL;
    Session once   = {.version = versions[0]};
    const char *sid = soup_message_headers_get_one(h, "Mcp-Session-Id");
    if (request && !g_strcmp0(method, "initialize")) {
        s = new_session();
        soup_message_headers_replace(soup_server_message_get_response_headers(msg),
                "Mcp-Session-Id", s->id);
    } else if (sid) {
        s = g_hash_table_lookup(http->sessions, sid);
        if (!s) {
            http_status(msg, SOUP_STATUS_NOT_FOUND, "no such session");
            goto out;
        }
    } else if (request && !g_strcmp0(method, "server/discover")) {
        s = &once;                  /* answered "not found", and the client initializes */
    } else {
        http_status(msg, SOUP_STATUS_BAD_REQUEST, "initialize first, then send Mcp-Session-Id");
        goto out;
    }
    const char *version = soup_message_headers_get_one(h, "MCP-Protocol-Version");
    if (version && !supported_version(version) && s != &once) {
        http_status(msg, SOUP_STATUS_BAD_REQUEST, "that protocol version isn't served");
        goto out;
    }

    if (!request) {
        handle(s, root, NULL, NULL);    /* a notification or a response: nothing to say */
        soup_server_message_set_status(msg, SOUP_STATUS_ACCEPTED, NULL);
        goto out;
    }

    Exchange *x  = g_new0(Exchange, 1);
    x->refs      = 2;               /* the answer's, and this handler's */
    x->msg       = g_object_ref(msg);
    x->in_handler = TRUE;
    x->finished_id = g_signal_connect(msg, "finished", G_CALLBACK(on_finished), x);
    /* a tool's call may take minutes: it streams, with keepalives */
    const char *accept = soup_message_headers_get_list(h, "Accept");
    x->sse = !strcmp(method, "tools/call") && accept && strstr(accept, "text/event-stream");
    handle(s, root, http_respond, x);
    x->in_handler = FALSE;
    if (!x->answered) {
        if (x->sse) {
            soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
            SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
            soup_message_headers_set_content_type(rh, "text/event-stream", NULL);
            soup_message_headers_replace(rh, "Cache-Control", "no-cache");
            soup_message_headers_set_encoding(rh, SOUP_ENCODING_CHUNKED);
            soup_message_body_append(soup_server_message_get_response_body(msg),
                    SOUP_MEMORY_STATIC, ": calling\n\n", 12);
            x->keepalive = g_timeout_add_seconds(KEEPALIVE_S, keepalive, x);
        } else {
            soup_server_message_pause(msg);
        }
    }
    exchange_unref(x);
out:
    g_object_unref(parser);
    g_bytes_unref(bytes);
}

static void http_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
        GHashTable *query, gpointer data)
{
    const char *why    = NULL;
    const char *method = soup_server_message_get_method(msg);
    if (!http_allowed(msg, &why)) {
        http_status(msg, SOUP_STATUS_FORBIDDEN, why);
        return;
    }
    if (strcmp(path, HTTP_PATH)) {
        http_status(msg, SOUP_STATUS_NOT_FOUND, "MCP is at " HTTP_PATH);
    } else if (!strcmp(method, "POST")) {
        http_post(msg);
    } else if (!strcmp(method, "DELETE")) {
        const char *sid = soup_message_headers_get_one(
                soup_server_message_get_request_headers(msg), "Mcp-Session-Id");
        if (sid && g_hash_table_remove(http->sessions, sid)) {
            for (GList *l = http->order->head; l; l = l->next) {
                if (!strcmp(l->data, sid)) {
                    g_free(l->data);
                    g_queue_delete_link(http->order, l);
                    break;
                }
            }
            soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        } else {
            http_status(msg, SOUP_STATUS_NOT_FOUND, "no such session");
        }
    } else {
        /* GET would open a stream for vimb to ask on; it asks nothing */
        soup_message_headers_replace(soup_server_message_get_response_headers(msg), "Allow",
                "POST, DELETE");
        http_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
    }
}

gboolean mcp_serve_http(guint port, const ToolPolicy *p, GError **error)
{
    if (!archive_peer_check_works()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "MCP over HTTP needs Linux, to tell this user's programs from others'; "
                "use --mcp over stdio");
        return FALSE;
    }
    if (http) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "MCP is served already");
        return FALSE;
    }
    policy = *p;
    http   = g_new0(Http, 1);
    http->port     = port;
    http->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
            (GDestroyNotify)session_free);
    http->order    = g_queue_new();
    http->server   = soup_server_new("server-header", "vimb/" VERSION, NULL);
    soup_server_add_handler(http->server, NULL, http_handler, NULL, NULL);
    if (!soup_server_listen_local(http->server, port, SOUP_SERVER_LISTEN_IPV4_ONLY, error)) {
        g_object_unref(http->server);
        g_hash_table_unref(http->sessions);
        g_queue_free_full(http->order, g_free);
        g_clear_pointer(&http, g_free);
        return FALSE;
    }
    g_message("mcp: serving http://127.0.0.1:%u" HTTP_PATH, port);
    return TRUE;
}
