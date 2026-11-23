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

/* The assistant on Pi (pi.dev), with the user's own Pi set-up and login: Pi
 * runs as a child process, in its RPC mode, spoken to over its stdin and
 * stdout.
 *
 * Pi is a coding agent, and doesn't sandbox what it is asked to do; so it
 * runs with none of its own tools -- no files, no shell -- and none of what
 * it would load from the user's set-up or the directory it runs in. Its
 * tools are vimb's, given it by an extension written for the purpose, each
 * of which asks vimb through the one part of Pi's protocol that carries a
 * request from an extension out and an answer back: an "editor" dialog,
 * titled for the purpose, holding the call. Nothing else crosses -- no
 * socket, no file. Pi keeps the conversation; vimb answers the calls, one at
 * a time, each as a turn of its own. */

#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "backend.h"

#define CHANNEL         "vimb-tool"
#define EXTENSION_FILE  "vimb-tools.ts"
#define LOG_FILE        "pi.log"
#define LOG_TAIL        600

static const char extension_source[] =
    "const CHANNEL = \"" CHANNEL "\";\n"
    "const TOOLS = %s;\n"
    "\n"
    "async function ask(ctx: any, tool: string, params: unknown) {\n"
    "  const reply = await ctx.ui.editor(CHANNEL, JSON.stringify({ tool, params }));\n"
    "  if (reply === undefined) {\n"
    "    throw new Error(\"vimb did not answer.\");\n"
    "  }\n"
    "  const answer = JSON.parse(reply);\n"
    "  if (!answer.ok) {\n"
    "    throw new Error(answer.error);\n"
    "  }\n"
    "  if (answer.image) {\n"
    "    return { content: [{ type: \"image\" as const, data: answer.image.data,\n"
    "                         mimeType: answer.image.mime }], details: {} };\n"
    "  }\n"
    "  return { content: [{ type: \"text\" as const, text: answer.text }], details: {} };\n"
    "}\n"
    "\n"
    "export default function (pi: any) {\n"
    "  for (const t of TOOLS) {\n"
    "    pi.registerTool({\n"
    "      name: t.name,\n"
    "      label: t.name,\n"
    "      description: t.description,\n"
    "      parameters: t.inputSchema,\n"
    "      execute: (_id: string, params: unknown, _signal: unknown, _update: unknown, ctx: any) =>\n"
    "        ask(ctx, t.name, params),\n"
    "    });\n"
    "  }\n"
    "}\n";

typedef struct {
    Backend          base;
    char             *command;
    char             *model;
    char             *thinking;
    char             *system;
    JsonNode         *tools;
    /* the process */
    char             *dir;
    GSubprocess      *process;
    GOutputStream    *to_pi;
    GDataInputStream *from_pi;
    GCancellable     *cancellable;
    /* the turn */
    gboolean         busy;
    gboolean         aborting;      /* aborted, and not yet settled */
    char             *waiting;      /* asked meanwhile */
    GQueue           *requests;     /* calls not yet given vimb */
    char             *current;      /* the call vimb is answering */
    char             *error;
    guint            next_id;
} Pi;

static void read_next(Pi *p);
static void dispatch_next(Pi *p);

/* ---- talking to Pi ------------------------------------------------------- */

/* Takes message. */
static void send_message(Pi *p, JsonObject *message)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, message);
    if (p->to_pi) {
        char *text = json_to_string(node, FALSE);
        char *line = g_strconcat(text, "\n", NULL);
        GError *error = NULL;
        if (!g_output_stream_write_all(p->to_pi, line, strlen(line), NULL, NULL, &error)) {
            g_message("assistant: can't write to pi: %s", error->message);
            g_error_free(error);
        }
        g_free(line);
        g_free(text);
    }
    json_node_unref(node);
}

static void answer_dialog(Pi *p, const char *id, const char *value)
{
    JsonObject *r = json_object_new();
    json_object_set_string_member(r, "type", "extension_ui_response");
    json_object_set_string_member(r, "id", id);
    if (value) {
        json_object_set_string_member(r, "value", value);
    } else {
        json_object_set_boolean_member(r, "cancelled", TRUE);
    }
    send_message(p, r);
}

static void send_prompt(Pi *p, const char *text)
{
    JsonObject *m = json_object_new();
    char *id = g_strdup_printf("ask-%u", ++p->next_id);
    json_object_set_string_member(m, "id", id);
    json_object_set_string_member(m, "type", "prompt");
    json_object_set_string_member(m, "message", text);
    send_message(p, m);
    g_free(id);
}

static char *log_tail(Pi *p)
{
    char *path = g_build_filename(p->dir, LOG_FILE, NULL);
    char *text = NULL;
    gsize length = 0;
    g_file_get_contents(path, &text, &length, NULL);
    g_free(path);
    if (!text) {
        return NULL;
    }
    char *tail = g_strstrip(g_strdup(length > LOG_TAIL ? text + length - LOG_TAIL : text));
    g_free(text);
    return tail;
}

static void forget_process(Pi *p)
{
    if (p->cancellable) {
        g_cancellable_cancel(p->cancellable);
        g_clear_object(&p->cancellable);
    }
    if (p->process) {
        g_subprocess_force_exit(p->process);
        g_clear_object(&p->process);
    }
    g_clear_object(&p->to_pi);
    g_clear_object(&p->from_pi);
}

static void turn_end(Pi *p, const char *stop, const char *error)
{
    p->busy = FALSE;
    g_clear_pointer(&p->current, g_free);
    g_queue_clear_full(p->requests, (GDestroyNotify)json_object_unref);
    char *why = g_strdup(error);
    g_clear_pointer(&p->error, g_free);
    p->base.events.turn_done(NULL, stop, why, p->base.data);
    g_free(why);
}

/* ---- what Pi says ------------------------------------------------------- */

static void on_ui_request(Pi *p, JsonObject *e)
{
    const char *id     = json_object_get_string_member_with_default(e, "id", NULL);
    const char *method = json_object_get_string_member_with_default(e, "method", "");
    if (!id) {
        return;
    }
    gboolean dialog = !strcmp(method, "select") || !strcmp(method, "confirm")
        || !strcmp(method, "input") || !strcmp(method, "editor");
    if (!dialog) {
        return;                     /* the rest only informs */
    }
    const char *title   = json_object_get_string_member_with_default(e, "title", "");
    const char *prefill = json_object_get_string_member_with_default(e, "prefill", NULL);
    JsonParser *parser  = json_parser_new();
    JsonObject *query   = NULL;
    if (!strcmp(method, "editor") && !strcmp(title, CHANNEL) && prefill
            && json_parser_load_from_data(parser, prefill, -1, NULL)
            && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        query = json_node_get_object(json_parser_get_root(parser));
    }
    if (!query || !json_object_has_member(query, "tool") || p->aborting) {
        answer_dialog(p, id, NULL); /* the tools ask nothing of the user */
    } else {
        json_object_set_string_member(query, "id", id);
        g_queue_push_tail(p->requests, json_object_ref(query));
        if (!p->current) {
            dispatch_next(p);
        }
    }
    g_object_unref(parser);
}

/* Gives vimb the next call: a turn of one call. */
static void dispatch_next(Pi *p)
{
    JsonObject *query = g_queue_pop_head(p->requests);
    if (!query) {
        return;
    }
    JsonNode *params = json_object_get_member(query, "params");
    BackendToolCall *call = g_new0(BackendToolCall, 1);
    call->id    = g_strdup(json_object_get_string_member(query, "id"));
    call->name  = g_strdup(json_object_get_string_member_with_default(query, "tool", ""));
    call->input = params && JSON_NODE_HOLDS_OBJECT(params)
        ? json_object_ref(json_node_get_object(params)) : json_object_new();
    p->current = g_strdup(call->id);
    json_object_unref(query);
    GPtrArray *calls = g_ptr_array_new_with_free_func((GDestroyNotify)backend_tool_call_free);
    g_ptr_array_add(calls, call);
    p->base.events.turn_done(calls, "tool_use", NULL, p->base.data);
    g_ptr_array_unref(calls);
}

static void on_line(Pi *p, const char *line)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, NULL)
            || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_object_unref(parser);
        return;
    }
    JsonObject *e    = json_node_get_object(json_parser_get_root(parser));
    const char *type = json_object_get_string_member_with_default(e, "type", "");
    JsonNode *node;

    if (!strcmp(type, "message_update") && !p->aborting) {
        JsonObject *u = (node = json_object_get_member(e, "assistantMessageEvent"))
            && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        const char *ut    = u ? json_object_get_string_member_with_default(u, "type", "") : "";
        const char *delta = u ? json_object_get_string_member_with_default(u, "delta", "") : "";
        if (!strcmp(ut, "text_delta")) {
            p->base.events.text(delta, p->base.data);
        } else if (!strcmp(ut, "thinking_delta")) {
            p->base.events.note(delta, p->base.data);
        }
    } else if (!strcmp(type, "message_end")) {
        /* a model that couldn't be reached ends its message with an error */
        JsonObject *m = (node = json_object_get_member(e, "message"))
            && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (m && !g_strcmp0(json_object_get_string_member_with_default(m, "role", ""), "assistant")
                && !g_strcmp0(json_object_get_string_member_with_default(m, "stopReason", ""),
                    "error")) {
            g_free(p->error);
            p->error = g_strdup(json_object_get_string_member_with_default(m, "errorMessage",
                        "the model failed"));
        }
    } else if (!strcmp(type, "extension_ui_request")) {
        on_ui_request(p, e);
    } else if (!strcmp(type, "extension_error")) {
        g_free(p->error);
        p->error = g_strdup(json_object_get_string_member_with_default(e, "error", "a tool failed"));
    } else if (!strcmp(type, "response")) {
        if (!json_object_get_boolean_member_with_default(e, "success", TRUE)
                && !g_strcmp0(json_object_get_string_member_with_default(e, "command", ""),
                    "prompt") && p->busy && !p->aborting) {
            turn_end(p, NULL, json_object_get_string_member_with_default(e, "error",
                        "pi refused the prompt"));
        }
    } else if (!strcmp(type, "agent_settled")) {
        if (p->aborting) {
            p->aborting = FALSE;
            g_clear_pointer(&p->error, g_free);
            if (p->waiting) {       /* asked while the abort settled */
                char *text = p->waiting;
                p->waiting = NULL;
                send_prompt(p, text);
                g_free(text);
            }
        } else if (p->busy) {
            turn_end(p, p->error ? NULL : "end_turn", p->error);
        }
    }
    g_object_unref(parser);
}

static void on_read(GObject *source, GAsyncResult *res, gpointer data)
{
    Pi *p         = data;
    GError *error = NULL;
    gsize length;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), res,
            &length, &error);
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);        /* p may be gone */
        return;
    }
    if (!line) {
        /* Pi has stopped: it is started again when next asked */
        char *tail = log_tail(p);
        char *why  = g_strdup_printf("pi stopped%s%s", tail && *tail ? ": " : "", tail ? tail : "");
        g_free(tail);
        g_clear_error(&error);
        forget_process(p);
        gboolean busy = p->busy && !p->aborting;
        p->aborting = FALSE;
        g_clear_pointer(&p->waiting, g_free);
        if (busy) {
            turn_end(p, NULL, why);
        } else {
            g_message("assistant: %s", why);
        }
        g_free(why);
        return;
    }
    on_line(p, line);
    g_free(line);
    if (p->from_pi) {
        read_next(p);
    }
}

static void read_next(Pi *p)
{
    g_data_input_stream_read_line_async(p->from_pi, G_PRIORITY_DEFAULT, p->cancellable, on_read, p);
}

/* ---- starting Pi -------------------------------------------------------- */

static void child_setup(gpointer data)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);   /* Pi goes when vimb does, however it goes */
#endif
}

static gboolean start(Pi *p, GError **error)
{
    if (p->process) {
        return TRUE;
    }
    if (!p->dir && !(p->dir = g_dir_make_tmp("vimb-pi-XXXXXX", error))) {
        return FALSE;
    }
    /* the tools, by name and by what they take */
    JsonArray *list = json_node_get_array(p->tools);
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(list); i++) {
        g_string_append_printf(names, "%s%s", i ? "," : "",
                json_object_get_string_member(json_array_get_object_element(list, i), "name"));
    }
    char *tools_json = json_to_string(p->tools, FALSE);
    char *source     = g_strdup_printf(extension_source, tools_json);
    char *extension  = g_build_filename(p->dir, EXTENSION_FILE, NULL);
    char *log        = g_build_filename(p->dir, LOG_FILE, NULL);
    gboolean ok      = g_file_set_contents(extension, source, -1, error);
    g_free(source);
    g_free(tools_json);

    if (ok) {
        GPtrArray *argv = g_ptr_array_new();
        const char *fixed[] = {p->command, "--mode", "rpc",
            "--no-session",             /* nothing is kept */
            "--no-builtin-tools",       /* nothing of Pi's own: no files, no shell */
            "--no-extensions", "--no-skills", "--no-prompt-templates", "--no-themes",
            "--no-context-files", "--no-approve",
            "--offline",                /* no update checks; the model is still reached */
            "--extension", extension, "--tools", names->str, "--system-prompt", p->system, NULL};
        for (int i = 0; fixed[i]; i++) {
            g_ptr_array_add(argv, (gpointer)fixed[i]);
        }
        if (p->model && *p->model) {
            g_ptr_array_add(argv, "--model");
            g_ptr_array_add(argv, p->model);
        }
        if (p->thinking && *p->thinking) {
            g_ptr_array_add(argv, "--thinking");
            g_ptr_array_add(argv, p->thinking);
        }
        g_ptr_array_add(argv, NULL);

        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
                G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
        g_subprocess_launcher_set_cwd(launcher, p->dir);
        g_subprocess_launcher_set_stderr_file_path(launcher, log);
        g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
        p->process = g_subprocess_launcher_spawnv(launcher, (const char * const *)argv->pdata, error);
        g_object_unref(launcher);
        g_ptr_array_free(argv, TRUE);
        ok = p->process != NULL;
    }
    g_free(extension);
    g_free(log);
    g_string_free(names, TRUE);
    if (!ok) {
        return FALSE;
    }
    p->to_pi       = g_object_ref(g_subprocess_get_stdin_pipe(p->process));
    p->from_pi     = g_data_input_stream_new(g_subprocess_get_stdout_pipe(p->process));
    p->cancellable = g_cancellable_new();
    /* events carry whole messages, which can be large */
    g_data_input_stream_set_newline_type(p->from_pi, G_DATA_STREAM_NEWLINE_TYPE_LF);
    read_next(p);
    return TRUE;
}

/* ---- the backend --------------------------------------------------------- */

static void pi_send(Backend *b, const char *text)
{
    Pi *p = (Pi *)b;
    GError *error = NULL;
    if (!start(p, &error)) {
        char *why = g_strdup_printf("can't start %s: %s", p->command, error->message);
        g_error_free(error);
        p->busy = TRUE;
        turn_end(p, NULL, why);
        g_free(why);
        return;
    }
    p->busy = TRUE;
    g_clear_pointer(&p->error, g_free);
    if (p->aborting) {
        g_free(p->waiting);
        p->waiting = g_strdup(text);
    } else {
        send_prompt(p, text);
    }
}

static void pi_send_results(Backend *b, GPtrArray *results)
{
    Pi *p = (Pi *)b;
    for (guint i = 0; i < results->len; i++) {
        BackendToolResult *r = g_ptr_array_index(results, i);
        if (g_strcmp0(r->id, p->current)) {
            continue;
        }
        JsonObject *answer = json_object_new();
        json_object_set_boolean_member(answer, "ok", r->ok);
        if (r->image_data) {
            JsonObject *image = json_object_new();
            json_object_set_string_member(image, "mime", r->image_mime);
            json_object_set_string_member(image, "data", r->image_data);
            json_object_set_object_member(answer, "image", image);
        } else {
            json_object_set_string_member(answer, r->ok ? "text" : "error", r->text ? r->text : "");
        }
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, answer);
        char *value = json_to_string(node, FALSE);
        answer_dialog(p, r->id, value);
        g_free(value);
        json_node_unref(node);
    }
    g_clear_pointer(&p->current, g_free);
    dispatch_next(p);
}

static void pi_cancel(Backend *b)
{
    Pi *p = (Pi *)b;
    if (!p->busy || !p->process) {
        p->busy = FALSE;
        return;
    }
    JsonObject *abort = json_object_new();
    json_object_set_string_member(abort, "type", "abort");
    send_message(p, abort);
    /* a call waiting on vimb would wait for ever: abort doesn't end it */
    if (p->current) {
        answer_dialog(p, p->current, NULL);
        g_clear_pointer(&p->current, g_free);
    }
    JsonObject *query;
    while ((query = g_queue_pop_head(p->requests))) {
        answer_dialog(p, json_object_get_string_member(query, "id"), NULL);
        json_object_unref(query);
    }
    p->aborting = TRUE;
    p->busy     = FALSE;
    g_clear_pointer(&p->error, g_free);
}

static void pi_free(Backend *b)
{
    Pi *p = (Pi *)b;
    forget_process(p);
    if (p->dir) {
        char *extension = g_build_filename(p->dir, EXTENSION_FILE, NULL);
        char *log       = g_build_filename(p->dir, LOG_FILE, NULL);
        g_remove(extension);
        g_remove(log);
        g_rmdir(p->dir);
        g_free(extension);
        g_free(log);
        g_free(p->dir);
    }
    g_queue_free_full(p->requests, (GDestroyNotify)json_object_unref);
    g_free(p->current);
    g_free(p->error);
    g_free(p->waiting);
    g_free(p->command);
    g_free(p->model);
    g_free(p->thinking);
    g_free(p->system);
    json_node_unref(p->tools);
    g_free(p);
}

/* The pi to run: $VIMB_PI_COMMAND, pi on the PATH, or ~/.local/bin/pi. */
static char *find_pi(void)
{
    const char *env = g_getenv("VIMB_PI_COMMAND");
    if (env && *env) {
        return g_strdup(env);
    }
    char *found = g_find_program_in_path("pi");
    if (found) {
        return found;
    }
    found = g_build_filename(g_get_home_dir(), ".local", "bin", "pi", NULL);
    if (g_file_test(found, G_FILE_TEST_IS_EXECUTABLE)) {
        return found;
    }
    g_free(found);
    return NULL;
}

Backend *backend_pi_new(const BackendConfig *config, const BackendEvents *events,
        gpointer data, GError **error)
{
    char *command = find_pi();
    if (!command) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "the Pi assistant needs pi (pi.dev) on the PATH, or VIMB_PI_COMMAND");
        return NULL;
    }
    Pi *p = g_new0(Pi, 1);
    p->base.name         = "pi";
    p->base.send         = pi_send;
    p->base.send_results = pi_send_results;
    p->base.cancel       = pi_cancel;
    p->base.free         = pi_free;
    p->base.events       = *events;
    p->base.data         = data;
    p->command  = command;
    p->model    = g_strdup(config->model);
    p->thinking = g_strdup(config->effort);
    p->system   = g_strdup(config->system);
    p->tools    = json_node_copy(config->tools);
    p->requests = g_queue_new();
    return &p->base;
}
