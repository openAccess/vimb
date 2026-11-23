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
#include <string.h>
#include <unistd.h>

#include "../version.h"
#include "mcp.h"

/* JSON-RPC's error codes. */
#define PARSE_ERROR      -32700
#define INVALID_REQUEST  -32600
#define METHOD_NOT_FOUND -32601
#define INVALID_PARAMS   -32602

static const char *versions[] = {   /* newest first */
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL
};

static const char instructions[] =
    "vimb is a web browser that archives what it loads as WACZ web archives. Tabs and "
    "archives are named by number. To archive pages: start_archive, then open_page with the "
    "archive's number (and scroll_page, so that what loads only as it is read is archived "
    "too), or crawl from seeds; then stop_archive, which saves the WACZ and says where. "
    "read_page, page_links and screenshot tell what a tab shows.";

typedef struct {
    GDataInputStream *in;
    int              out;
    ToolPolicy       policy;
    const char       *version;      /* as negotiated */
    gboolean         broken;        /* out can't be written */
} Server;

static Server *server;

/* ---- writing ------------------------------------------------------------ */

static void write_line(const char *text)
{
    gsize length = strlen(text);
    char *line   = g_malloc(length + 1);
    memcpy(line, text, length);
    line[length++] = '\n';
    for (gsize done = 0; done < length && !server->broken;) {
        ssize_t n = write(server->out, line + done, length - done);
        if (n > 0) {
            done += n;
        } else if (n < 0 && errno != EINTR) {
            g_message("mcp: can't write: %s", g_strerror(errno));
            server->broken = TRUE;
        }
    }
    g_free(line);
}

/* Takes message. */
static void send_message(JsonObject *message)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, message);
    char *text = json_to_string(node, FALSE);
    write_line(text);
    g_free(text);
    json_node_unref(node);
}

static JsonObject *envelope(JsonNode *id)
{
    JsonObject *message = json_object_new();
    json_object_set_string_member(message, "jsonrpc", "2.0");
    json_object_set_member(message, "id", id ? json_node_copy(id) : json_node_new(JSON_NODE_NULL));
    return message;
}

/* Takes result. */
static void respond(JsonNode *id, JsonObject *result)
{
    JsonObject *message = envelope(id);
    json_object_set_object_member(message, "result", result);
    send_message(message);
}

static void respond_error(JsonNode *id, int code, const char *text)
{
    JsonObject *error = json_object_new();
    json_object_set_int_member(error, "code", code);
    json_object_set_string_member(error, "message", text);
    JsonObject *message = envelope(id);
    json_object_set_object_member(message, "error", error);
    send_message(message);
}

/* ---- the methods ---------------------------------------------------------- */

static void initialize(JsonNode *id, JsonObject *params)
{
    const char *asked = params && json_object_has_member(params, "protocolVersion")
        ? json_object_get_string_member(params, "protocolVersion") : NULL;
    server->version = versions[0];
    for (int i = 0; asked && versions[i]; i++) {
        if (!strcmp(asked, versions[i])) {
            server->version = versions[i];
        }
    }

    JsonObject *tools = json_object_new();
    json_object_set_boolean_member(tools, "listChanged", FALSE);
    JsonObject *capabilities = json_object_new();
    json_object_set_object_member(capabilities, "tools", tools);
    JsonObject *info = json_object_new();
    json_object_set_string_member(info, "name", "vimb");
    json_object_set_string_member(info, "version", VERSION);
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "protocolVersion", server->version);
    json_object_set_object_member(result, "capabilities", capabilities);
    json_object_set_object_member(result, "serverInfo", info);
    json_object_set_string_member(result, "instructions", instructions);
    respond(id, result);
}

static void list_tools(JsonNode *id)
{
    JsonObject *result = json_object_new();
    json_object_set_member(result, "tools", tools_list(&server->policy));
    respond(id, result);
}

static gboolean tool_exists(const char *name)
{
    JsonNode *list = tools_list(&server->policy);
    JsonArray *array = json_node_get_array(list);
    gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(array) && !found; i++) {
        JsonObject *tool = json_array_get_object_element(array, i);
        found = !g_strcmp0(json_object_get_string_member(tool, "name"), name);
    }
    json_node_unref(list);
    return found;
}

static void tool_done(const ToolResult *result, gpointer data)
{
    JsonNode *id = data;
    if (!server || server->broken) {
        json_node_unref(id);
        return;
    }
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
                && strcmp(server->version, "2025-06-18") >= 0) {
            json_object_set_member(answer, "structuredContent", json_node_copy(result->value));
        }
    } else {
        json_object_set_string_member(item, "type", "text");
        json_object_set_string_member(item, "text", result->error);
    }
    json_array_add_object_element(content, item);
    json_object_set_array_member(answer, "content", content);
    json_object_set_boolean_member(answer, "isError", !result->ok);
    respond(id, answer);
    json_node_unref(id);
}

static void call_tool(JsonNode *id, JsonObject *params)
{
    JsonNode *name = params ? json_object_get_member(params, "name") : NULL;
    JsonNode *args = params ? json_object_get_member(params, "arguments") : NULL;
    if (!name || !JSON_NODE_HOLDS_VALUE(name) || json_node_get_value_type(name) != G_TYPE_STRING) {
        respond_error(id, INVALID_PARAMS, "tools/call needs the tool's name");
        return;
    }
    if (args && !JSON_NODE_HOLDS_OBJECT(args) && !JSON_NODE_HOLDS_NULL(args)) {
        respond_error(id, INVALID_PARAMS, "a tool's arguments are an object");
        return;
    }
    if (!tool_exists(json_node_get_string(name))) {
        char *text = g_strdup_printf("Unknown tool: %s", json_node_get_string(name));
        respond_error(id, INVALID_PARAMS, text);
        g_free(text);
        return;
    }
    tools_call(json_node_get_string(name),
            args && JSON_NODE_HOLDS_OBJECT(args) ? json_node_get_object(args) : NULL,
            &server->policy, tool_done, json_node_copy(id));
}

static void handle(JsonNode *node)
{
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        respond_error(NULL, INVALID_REQUEST, "a message is a JSON object");
        return;
    }
    JsonObject *message = json_node_get_object(node);
    JsonNode *id        = json_object_get_member(message, "id");
    JsonNode *method    = json_object_get_member(message, "method");
    JsonNode *params    = json_object_get_member(message, "params");
    JsonObject *p       = params && JSON_NODE_HOLDS_OBJECT(params) ? json_node_get_object(params)
        : NULL;
    if (!method) {
        return;                     /* a response; vimb asks nothing */
    }
    if (!JSON_NODE_HOLDS_VALUE(method) || json_node_get_value_type(method) != G_TYPE_STRING) {
        respond_error(id, INVALID_REQUEST, "method must be a string");
        return;
    }
    const char *name = json_node_get_string(method);
    if (!id) {
        return;                     /* notifications: initialized, cancelled -- none needs doing */
    }
    if (!strcmp(name, "initialize")) {
        initialize(id, p);
    } else if (!strcmp(name, "ping")) {
        respond(id, json_object_new());
    } else if (!strcmp(name, "tools/list")) {
        list_tools(id);
    } else if (!strcmp(name, "tools/call")) {
        call_tool(id, p);
    } else {
        /* server/discover among them: the 2026 protocol, whose clients then
         * fall back to initialize */
        char *text = g_strdup_printf("Method not found: %s", name);
        respond_error(id, METHOD_NOT_FOUND, text);
        g_free(text);
    }
}

/* ---- reading ------------------------------------------------------------ */

static void read_next(void);

static void finish(void)
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
        finish();
        return;
    }
    if (length && line[length - 1] == '\r') {
        line[--length] = '\0';
    }
    if (length) {
        JsonParser *parser = json_parser_new();
        if (!g_utf8_validate(line, length, NULL)) {
            respond_error(NULL, PARSE_ERROR, "a message must be UTF-8");
        } else if (!json_parser_load_from_data(parser, line, length, &error)) {
            respond_error(NULL, PARSE_ERROR, error->message);
            g_clear_error(&error);
        } else {
            handle(json_parser_get_root(parser));
        }
        g_object_unref(parser);
    }
    g_free(line);
    if (server->broken) {
        finish();
    } else {
        read_next();
    }
}

static void read_next(void)
{
    g_data_input_stream_read_line_async(server->in, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

gboolean mcp_serve_stdio(int in_fd, int out_fd, const ToolPolicy *policy, GError **error)
{
    if (out_fd == STDOUT_FILENO) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "MCP must not write to stdout itself");
        return FALSE;
    }
    server         = g_new0(Server, 1);
    server->out    = out_fd;
    server->policy = *policy;
    server->version = versions[0];
    GInputStream *in = g_unix_input_stream_new(in_fd, FALSE);
    server->in = g_data_input_stream_new(in);
    g_object_unref(in);
    g_data_input_stream_set_newline_type(server->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    read_next();
    return TRUE;
}
