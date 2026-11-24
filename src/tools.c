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

#include <stdarg.h>
#include <string.h>

#include "archiving.h"
#include "crawl.h"
#include "luascript.h"
#include "ops.h"
#include "tools.h"

#define DEFAULT_READ_LIMIT 50000
#define SCRIPT_RESULT_MAX  100000
#define DEFAULT_TIMEOUT    60

extern struct Vimb vb;

typedef struct Call Call;
typedef void (*ToolFunc)(Call *call);

typedef struct {
    const char *name;
    const char *description;
    const char *schema;
    ToolFunc   run;
    gboolean   needs_eval;
    gboolean   needs_lua;
} Tool;

struct Call {
    const Tool       *tool;
    JsonObject       *args;
    const ToolPolicy *policy;
    ToolDone         done;
    gpointer         data;
    guint            tab;       /* for results that name their tab */
    guint            archive;
    const char       *wrap;     /* the member a non-object value goes in */
    char             *format;   /* read_page */
    gint64           offset;
    gint64           limit;
    LuaRun           *script;   /* run_lua */
};

static GList *scripts;              /* the Calls whose scripts run */

/* ---- numbering tabs and archives --------------------------------------- */

static GHashTable *tab_numbers;     /* webview id -> number */
static GHashTable *tab_ids;         /* number -> webview id */
static guint next_tab = 1;
static GHashTable *archives;        /* number -> struct Archive *, held */
static guint next_archive = 1;

static guint tab_number(Client *c)
{
    if (!tab_numbers) {
        tab_numbers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        tab_ids     = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
    gint64 id = (gint64)ops_tab_id(c);
    guint n   = GPOINTER_TO_UINT(g_hash_table_lookup(tab_numbers, &id));
    if (!n) {
        n = next_tab++;
        g_hash_table_insert(tab_numbers, g_memdup2(&id, sizeof(id)), GUINT_TO_POINTER(n));
        g_hash_table_insert(tab_ids, GUINT_TO_POINTER(n), g_memdup2(&id, sizeof(id)));
    }
    return n;
}

static Client *tab_by_number(guint n)
{
    gint64 *id = tab_ids ? g_hash_table_lookup(tab_ids, GUINT_TO_POINTER(n)) : NULL;
    return id ? ops_find_tab((guint64)*id) : NULL;
}

static struct Archive *archive_by_number(guint n)
{
    struct Archive *a = archives ? g_hash_table_lookup(archives, GUINT_TO_POINTER(n)) : NULL;
    return archiving_alive(a) ? a : NULL;
}

/* ---- results ----------------------------------------------------------- */

static void call_free(Call *call)
{
    json_object_unref(call->args);
    g_free(call->format);
    g_free(call);
}

/* Takes value. */
static void call_ok(Call *call, JsonNode *value)
{
    ToolResult result = {.ok = TRUE, .value = value};
    call->done(&result, call->data);
    if (value) {
        json_node_unref(value);
    }
    call_free(call);
}

static void call_fail(Call *call, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void call_fail(Call *call, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *message = g_strdup_vprintf(format, args);
    va_end(args);
    ToolResult result = {.ok = FALSE, .error = message};
    call->done(&result, call->data);
    g_free(message);
    call_free(call);
}

/* Passes an operation's result on as the tool's. */
static void pass_result(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Call *call = data;
    if (ok) {
        call_ok(call, value ? json_node_ref(value) : NULL);
    } else {
        call_fail(call, "%s", error);
    }
}

static JsonNode *object_node(JsonObject *object)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    return node;
}

/* Passes an operation's result on as an object naming the tab: an object's
 * members, or the value as call->wrap. */
static void tab_result(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Call *call = data;
    if (!ok) {
        call_fail(call, "%s (tab %u)", error, call->tab);
        return;
    }
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "tab", call->tab);
    if (value && JSON_NODE_HOLDS_OBJECT(value) && !call->wrap) {
        JsonObjectIter iter;
        const char *name;
        JsonNode *member;
        json_object_iter_init(&iter, json_node_get_object(value));
        while (json_object_iter_next(&iter, &name, &member)) {
            json_object_set_member(o, name, json_node_copy(member));
        }
    } else if (call->wrap) {
        json_object_set_member(o, call->wrap,
                value ? json_node_copy(value) : json_node_new(JSON_NODE_NULL));
    }
    call_ok(call, object_node(o));
}

/* ---- arguments --------------------------------------------------------- */

static const char *arg_string(Call *call, const char *name)
{
    JsonNode *node = json_object_get_member(call->args, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static gboolean arg_present(Call *call, const char *name)
{
    JsonNode *node = json_object_get_member(call->args, name);
    return node && !JSON_NODE_HOLDS_NULL(node);
}

static double arg_number(Call *call, const char *name, double fallback)
{
    JsonNode *node = json_object_get_member(call->args, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) {
        return fallback;
    }
    GType type = json_node_get_value_type(node);
    return type == G_TYPE_INT64 || type == G_TYPE_DOUBLE ? json_node_get_double(node) : fallback;
}

static gboolean arg_boolean(Call *call, const char *name, gboolean fallback)
{
    JsonNode *node = json_object_get_member(call->args, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN
        ? json_node_get_boolean(node) : fallback;
}

/* The tab the call names, or NULL, having failed the call. */
static Client *arg_tab(Call *call)
{
    guint n   = (guint)arg_number(call, "tab", 0);
    Client *c = n ? tab_by_number(n) : NULL;
    if (!c) {
        call_fail(call, n ? "there is no tab %u (it may have closed)" : "say which tab", n);
        return NULL;
    }
    call->tab = n;
    return c;
}

static struct Archive *arg_archive(Call *call)
{
    guint n           = (guint)arg_number(call, "archive", 0);
    struct Archive *a = n ? archive_by_number(n) : NULL;
    if (!a) {
        call_fail(call, n ? "there is no archive %u" : "say which archive", n);
        return NULL;
    }
    call->archive = n;
    return a;
}

/* Whether the policy lets url be opened; why not, if not. */
static gboolean url_allowed(Call *call, const char *url, char **why)
{
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
    const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
    const char *host   = uri ? g_uri_get_host(uri) : NULL;
    gboolean ok = scheme && host && *host
        && (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "https"));
    if (!ok) {
        *why = g_strdup_printf("%s is not an http or https URL", url);
    }
    if (ok && call->policy->allowed_domains) {
        ok = FALSE;
        for (char **d = call->policy->allowed_domains; *d && !ok; d++) {
            gsize hl = strlen(host), dl = strlen(*d);
            ok = !g_ascii_strcasecmp(host, *d)
                || (hl > dl && host[hl - dl - 1] == '.' && !g_ascii_strcasecmp(host + hl - dl, *d));
        }
        if (!ok) {
            *why = g_strdup_printf("%s is not on a site this is allowed to open", host);
        }
    }
    if (uri) {
        g_uri_unref(uri);
    }
    if (ok && call->policy->url_check) {
        ok = call->policy->url_check(url, why, call->policy->url_check_data);
    }
    return ok;
}

/* ---- the tools --------------------------------------------------------- */

static void tool_list_tabs(Call *call)
{
    JsonArray *tabs = json_array_new();
    for (Client *c = vb.clients; c; c = c->next) {
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "tab", tab_number(c));
        json_object_set_string_member(o, "url", webkit_web_view_get_uri(c->webview));
        json_object_set_string_member(o, "title", webkit_web_view_get_title(c->webview));
        json_object_set_boolean_member(o, "archiving", c->archive != NULL);
        json_array_add_object_element(tabs, o);
    }
    JsonObject *o = json_object_new();
    json_object_set_array_member(o, "tabs", tabs);
    call_ok(call, object_node(o));
}

static void tool_open_page(Call *call)
{
    const char *url = arg_string(call, "url");
    char *why       = NULL;
    if (!url) {
        call_fail(call, "say which url");
        return;
    }
    if (!url_allowed(call, url, &why)) {
        call_fail(call, "%s", why);
        g_free(why);
        return;
    }
    Client *c;
    if (arg_present(call, "archive")) {
        struct Archive *a = arg_archive(call);
        if (!a) {
            return;
        }
        c = vb_tab_new_archive(a, NULL);
    } else {
        c = vb_tab_new(NULL, NULL, TRUE);
    }
    call->tab = tab_number(c);
    ops_load(c, url, arg_number(call, "timeout", DEFAULT_TIMEOUT), tab_result, call);
}

static void tool_load_page(Call *call)
{
    const char *url = arg_string(call, "url");
    char *why       = NULL;
    Client *c       = arg_tab(call);
    if (!c) {
        return;
    }
    if (!url) {
        call_fail(call, "say which url");
        return;
    }
    if (!url_allowed(call, url, &why)) {
        call_fail(call, "%s", why);
        g_free(why);
        return;
    }
    ops_load(c, url, arg_number(call, "timeout", DEFAULT_TIMEOUT), tab_result, call);
}

/* A page's text, HTML or source, a window of it at a time: a page can run
 * to megabytes, which neither a model nor a pipe wants at once. */
static void read_done(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Call *call = data;
    if (!ok) {
        call_fail(call, "%s", error);
        return;
    }
    const char *text = value && JSON_NODE_HOLDS_VALUE(value)
        && json_node_get_value_type(value) == G_TYPE_STRING ? json_node_get_string(value) : "";
    glong length     = g_utf8_strlen(text, -1);
    glong start      = (glong)MIN((gint64)length, MAX(call->offset, 0));
    glong end        = (glong)MIN((gint64)length, start + MAX(call->limit, 0));
    char *window     = g_utf8_substring(text, start, end);
    JsonObject *o    = json_object_new();
    json_object_set_int_member(o, "tab", call->tab);
    json_object_set_string_member(o, "format", call->format);
    json_object_set_int_member(o, "offset", start);
    json_object_set_int_member(o, "length", length);
    json_object_set_boolean_member(o, "truncated", end < length);
    json_object_set_string_member(o, "text", window);
    g_free(window);
    call_ok(call, object_node(o));
}

static void tool_read_page(Call *call)
{
    Client *c = arg_tab(call);
    if (!c) {
        return;
    }
    const char *format = arg_string(call, "format");
    call->format = g_strdup(format ? format : "text");
    call->offset = (gint64)arg_number(call, "offset", 0);
    call->limit  = (gint64)arg_number(call, "limit", DEFAULT_READ_LIMIT);
    if (!strcmp(call->format, "text")) {
        ops_text(c, read_done, call);
    } else if (!strcmp(call->format, "html")) {
        ops_html(c, read_done, call);
    } else if (!strcmp(call->format, "source")) {
        ops_source(c, read_done, call);
    } else {
        call_fail(call, "format must be text, html or source");
    }
}

static void tool_page_links(Call *call)
{
    static const char script[] =
        "(() => { const seen = new Set(), out = [];"
        "  for (const a of document.querySelectorAll('a[href], area[href]')) {"
        "    const url = a.href.split('#')[0];"
        "    if (!/^https?:/i.test(url) || seen.has(url)) continue;"
        "    seen.add(url);"
        "    out.push({url: url, text: (a.innerText || a.title || a.alt || '').trim().slice(0, 200)});"
        "  }"
        "  return out; })()";
    Client *c = arg_tab(call);
    if (c) {
        call->wrap = "links";
        ops_eval(c, script, tab_result, call);
    }
}

static void tool_scroll_page(Call *call)
{
    Client *c = arg_tab(call);
    if (c) {
        ops_scroll(c, tab_result, call);
    }
}

static void screenshot_done(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Call *call = data;
    if (!ok || !value || !JSON_NODE_HOLDS_OBJECT(value)) {
        call_fail(call, "%s", error ? error : "no screenshot");
        return;
    }
    JsonObject *o = json_node_get_object(value);
    ToolResult result = {
        .ok         = TRUE,
        .image_mime = (char *)json_object_get_string_member(o, "mime"),
        .image_data = (char *)json_object_get_string_member(o, "data"),
    };
    call->done(&result, call->data);
    call_free(call);
}

static void tool_screenshot(Call *call)
{
    Client *c = arg_tab(call);
    if (c) {
        ops_screenshot(c, screenshot_done, call);
    }
}

static void tool_run_javascript(Call *call)
{
    Client *c          = arg_tab(call);
    const char *script = c ? arg_string(call, "script") : NULL;
    if (!c) {
        return;
    }
    if (!script) {
        call_fail(call, "say what script to run");
        return;
    }
    call->wrap = "value";
    ops_eval(c, script, tab_result, call);
}

static void tool_close_tab(Call *call)
{
    Client *c = arg_tab(call);
    if (!c) {
        return;
    }
    if (vb.clients == c && !c->next) {
        call_fail(call, "tab %u is the last tab, and closing it would quit vimb", call->tab);
        return;
    }
    guint64 id = ops_tab_id(c);
    vb_tab_close(c);                /* frees c, unless a download keeps it open */
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "tab", call->tab);
    json_object_set_boolean_member(o, "closed", ops_find_tab(id) == NULL);
    call_ok(call, object_node(o));
}

static JsonNode *archive_info(guint n, struct Archive *a)
{
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "archive", n);
    json_object_set_string_member(o, "name", archiving_name(a));
    json_object_set_string_member(o, "state", archiving_state(a));
    json_object_set_int_member(o, "responses", archiving_recorded(a));
    if (archiving_saved(a)) {
        json_object_set_string_member(o, "saved", archiving_saved(a));
    }
    return object_node(o);
}

static void tool_start_archive(Call *call)
{
    Client *c = vb_get_current_client();
    GError *error = NULL;
    int verify = -1, internal = -1;
    if (call->policy->allow_archive_options) {
        verify   = arg_present(call, "strict_ssl") ? arg_boolean(call, "strict_ssl", TRUE) : -1;
        internal = arg_present(call, "allow_internal")
            ? arg_boolean(call, "allow_internal", FALSE) : -1;
    }
    gboolean signed_in = call->policy->allow_signins && arg_boolean(call, "signed_in", FALSE);
    struct Archive *a = c ? archiving_create(c, arg_string(call, "name"), verify, internal,
            signed_in, &error) : NULL;
    if (!a) {
        call_fail(call, "%s", error ? error->message : "no tab to start from");
        g_clear_error(&error);
        return;
    }
    if (!archives) {
        archives = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    archiving_hold(a);
    guint n = next_archive++;
    g_hash_table_insert(archives, GUINT_TO_POINTER(n), a);
    call_ok(call, archive_info(n, a));
}

static void tool_archive_status(Call *call)
{
    struct Archive *a = arg_archive(call);
    if (a) {
        call_ok(call, archive_info(call->archive, a));
    }
}

static void stopped(const char *path, const char *error, gpointer data)
{
    Call *call        = data;
    struct Archive *a = archive_by_number(call->archive);
    if (a) {
        g_hash_table_remove(archives, GUINT_TO_POINTER(call->archive));
        archiving_release(a);
    }
    if (!path) {
        call_fail(call, "the archive could not be saved: %s", error);
        return;
    }
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "archive", call->archive);
    json_object_set_string_member(o, "saved", path);
    call_ok(call, object_node(o));
}

static void tool_stop_archive(Call *call)
{
    struct Archive *a = arg_archive(call);
    if (a) {
        archiving_stop_async(a, stopped, call);
    }
}

static void crawled(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Call *call = data;
    Client *c  = tab_by_number(call->tab);
    if (c) {
        vb_tab_close(c);
    }
    pass_result(ok, error, value, call);
}

static void tool_crawl(Call *call)
{
    static const char *scope_names[] = {"page-links", "site", "host", "pattern", "any", NULL};
    struct Archive *a = arg_archive(call);
    if (!a) {
        return;
    }
    JsonNode *seeds_node = json_object_get_member(call->args, "seeds");
    if (!seeds_node || !JSON_NODE_HOLDS_ARRAY(seeds_node)
            || !json_array_get_length(json_node_get_array(seeds_node))) {
        call_fail(call, "say which seeds to start from");
        return;
    }
    JsonArray *seed_array = json_node_get_array(seeds_node);
    GPtrArray *seeds = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < json_array_get_length(seed_array); i++) {
        const char *seed = json_array_get_string_element(seed_array, i);
        char *why = NULL;
        if (!seed || !url_allowed(call, seed, &why)) {
            call_fail(call, "%s", why ? why : "a seed is not a URL");
            g_free(why);
            g_ptr_array_free(seeds, TRUE);
            return;
        }
        g_ptr_array_add(seeds, g_strdup(seed));
    }
    g_ptr_array_add(seeds, NULL);

    const char *scope = arg_string(call, "scope");
    int s = 2;                                      /* host */
    if (scope) {
        for (s = 0; scope_names[s] && strcmp(scope, scope_names[s]); s++) {
        }
        if (!scope_names[s]) {
            call_fail(call, "scope must be page-links, site, host, pattern or any");
            g_ptr_array_free(seeds, TRUE);
            return;
        }
    }
    CrawlOptions options = {
        .seeds       = (char **)seeds->pdata,
        .depth       = (guint)arg_number(call, "depth", 1),
        .scope       = (CrawlScope)s,
        .pattern     = (char *)arg_string(call, "pattern"),
        .max_pages   = (guint)arg_number(call, "max_pages", 100),
        .max_seconds = arg_number(call, "max_seconds", 0),
        .delay       = arg_number(call, "delay", 1),
        .timeout     = arg_number(call, "timeout", DEFAULT_TIMEOUT),
        .scroll      = arg_boolean(call, "scroll", TRUE),
    };
    if (options.scope == CRAWL_SCOPE_PATTERN && !options.pattern) {
        call_fail(call, "scope pattern needs a pattern");
        g_ptr_array_free(seeds, TRUE);
        return;
    }
    Client *c = vb_tab_new_archive(a, NULL);
    call->tab = tab_number(c);
    crawl_run(c, &options, NULL, crawled, call);
    g_ptr_array_free(seeds, TRUE);
}

/* ---- replay ------------------------------------------------------------- */

/* Whether the policy lets path be replayed: any WACZ, or only one this run
 * saved. */
static gboolean replay_allowed(Call *call, const char *path)
{
    if (call->policy->replay_any) {
        return TRUE;
    }
    GPtrArray *saved = archiving_saved_all();
    char *real = g_canonicalize_filename(path, NULL);
    gboolean ok = FALSE;
    for (guint i = 0; i < saved->len && !ok; i++) {
        char *s = g_canonicalize_filename(g_ptr_array_index(saved, i), NULL);
        ok = !strcmp(s, real);
        g_free(s);
    }
    g_free(real);
    return ok;
}

static void tool_replay_archive(Call *call)
{
    const char *path = arg_string(call, "path");
    const char *url  = arg_string(call, "url");
    if (!path) {
        call_fail(call, "say which archive: the path of a WACZ");
        return;
    }
    if (!replay_allowed(call, path)) {
        call_fail(call, "only archives saved in this run can be replayed here");
        return;
    }
    GError *error = NULL;
    struct Archive *a = archiving_replay_open(vb_get_current_client(), path, &error);
    if (!a) {
        call_fail(call, "%s", error->message);
        g_error_free(error);
        return;
    }
    Client *c = vb_tab_new_archive(a, NULL);
    call->tab = tab_number(c);
    ops_load(c, url ? url : archiving_replay_landing(a), arg_number(call, "timeout", DEFAULT_TIMEOUT),
            tab_result, call);
}

/* ---- scripts ------------------------------------------------------------ */

static gboolean script_url_check(const char *url, char **why, gpointer data)
{
    return url_allowed(data, url, why);
}

static void script_learn(JsonNode *value, gpointer data)
{
    Call *call = data;
    if (call->policy->learn) {
        call->policy->learn(value, call->policy->url_check_data);
    }
}

static void script_done(gboolean ok, const char *error, JsonNode *value, const char *log,
        gpointer data)
{
    Call *call = data;
    scripts = g_list_remove(scripts, call);
    call->script = NULL;
    if (!ok) {
        call_fail(call, "the script failed: %s%s%s", error, log && *log ? "\nIt logged:\n" : "",
                log ? log : "");
        return;
    }
    JsonObject *o = json_object_new();
    char *text = value ? json_to_string(value, FALSE) : NULL;
    if (text && strlen(text) > SCRIPT_RESULT_MAX) {
        char *cut = g_strdup_printf("%.*s... (cut: %zu characters in all)", SCRIPT_RESULT_MAX,
                text, strlen(text));
        json_object_set_string_member(o, "value", cut);
        g_free(cut);
    } else {
        json_object_set_member(o, "value", value ? json_node_copy(value) : json_node_new(JSON_NODE_NULL));
    }
    g_free(text);
    json_object_set_string_member(o, "log", log ? log : "");
    call_ok(call, object_node(o));
}

static void script_approved(gboolean yes, gpointer data)
{
    Call *call = data;
    if (!yes) {
        call_fail(call, "the person didn't let the script run");
        return;
    }
    LuaPolicy policy = {.url_check = script_url_check, .learn = script_learn, .data = call};
    scripts = g_list_prepend(scripts, call);
    LuaRun *run = luascript_run_sandboxed(arg_string(call, "code"), &policy, script_done, call);
    /* a script that never waits is done -- and call with it -- already */
    if (g_list_find(scripts, call)) {
        call->script = run;
    }
}

static void tool_run_lua(Call *call)
{
    const char *code = arg_string(call, "code");
    if (!code || !*code) {
        call_fail(call, "say what code to run");
        return;
    }
    const char *description = arg_string(call, "description");
    call->policy->approve_lua(code, description ? description : "", script_approved, call,
            call->policy->url_check_data);
}

void tools_cancel_scripts(void)
{
    for (GList *l = scripts; l; l = l->next) {
        Call *call = l->data;
        if (call->script) {
            luascript_cancel(call->script);
        }
    }
}

#define TAB "\"tab\": {\"type\": \"integer\", \"description\": \"The tab, by its number.\"}"
#define ARCHIVE "\"archive\": {\"type\": \"integer\", \"description\": \"The archive, by its number.\"}"

static const Tool tools[] = {
    {"list_tabs", "List the open tabs: their numbers, URLs and titles, and whether they are archiving.",
        "{\"type\": \"object\", \"properties\": {}}", tool_list_tabs, FALSE},
    {"open_page", "Open a URL in a new tab and wait for it to load. With archive, the tab "
        "archives into that archive (see start_archive), and so does any tab opened from it.",
        "{\"type\": \"object\", \"properties\": {"
        "\"url\": {\"type\": \"string\"}, " ARCHIVE ", "
        "\"timeout\": {\"type\": \"number\", \"description\": \"Seconds to wait; default 60.\"}},"
        " \"required\": [\"url\"]}", tool_open_page, FALSE},
    {"load_page", "Load a URL in a tab and wait for it to load.",
        "{\"type\": \"object\", \"properties\": {" TAB ", \"url\": {\"type\": \"string\"}, "
        "\"timeout\": {\"type\": \"number\"}}, \"required\": [\"tab\", \"url\"]}",
        tool_load_page, FALSE},
    {"read_page", "Read a tab's page: its text, its HTML as it is now (after scripts), or the HTML "
        "the server sent -- a window of it at a time, by offset and limit in characters.",
        "{\"type\": \"object\", \"properties\": {" TAB ", "
        "\"format\": {\"type\": \"string\", \"enum\": [\"text\", \"html\", \"source\"]}, "
        "\"offset\": {\"type\": \"integer\"}, \"limit\": {\"type\": \"integer\", "
        "\"description\": \"Default 50000.\"}}, \"required\": [\"tab\"]}", tool_read_page, FALSE},
    {"page_links", "List the http(s) links on a tab's page, with their text.",
        "{\"type\": \"object\", \"properties\": {" TAB "}, \"required\": [\"tab\"]}",
        tool_page_links, FALSE},
    {"scroll_page", "Read a tab's page to its end, so that what loads only as it is read loads "
        "(and, in an archiving tab, is archived).",
        "{\"type\": \"object\", \"properties\": {" TAB "}, \"required\": [\"tab\"]}",
        tool_scroll_page, FALSE},
    {"screenshot", "Take a picture of what a tab shows.",
        "{\"type\": \"object\", \"properties\": {" TAB "}, \"required\": [\"tab\"]}",
        tool_screenshot, FALSE},
    {"run_javascript", "Run JavaScript in a tab's page, in a script world of its own that the "
        "page's scripts can't see, and return its value.",
        "{\"type\": \"object\", \"properties\": {" TAB ", \"script\": {\"type\": \"string\"}}, "
        "\"required\": [\"tab\", \"script\"]}", tool_run_javascript, TRUE},
    {"close_tab", "Close a tab.",
        "{\"type\": \"object\", \"properties\": {" TAB "}, \"required\": [\"tab\"]}",
        tool_close_tab, FALSE},
    {"start_archive", "Start a web archive (a WACZ). Pages are archived by opening them with it "
        "(open_page with archive) or crawling (crawl). It is saved by stop_archive.",
        "{\"type\": \"object\", \"properties\": {\"name\": {\"type\": \"string\"}, "
        "\"strict_ssl\": {\"type\": \"boolean\"}, \"allow_internal\": {\"type\": \"boolean\"}, "
        "\"signed_in\": {\"type\": \"boolean\", \"description\": \"Start with the cookies of the "
        "sites the person signed in to for archiving (:archive signin), to archive what they "
        "see signed in. Only when the person asks for it.\"}}}",
        tool_start_archive, FALSE},
    {"archive_status", "How an archive stands: recording, paused, saving or done; how many "
        "responses it holds; where it was saved.",
        "{\"type\": \"object\", \"properties\": {" ARCHIVE "}, \"required\": [\"archive\"]}",
        tool_archive_status, FALSE},
    {"stop_archive", "Stop an archive and save it; returns where.",
        "{\"type\": \"object\", \"properties\": {" ARCHIVE "}, \"required\": [\"archive\"]}",
        tool_stop_archive, FALSE},
    {"crawl", "Archive pages by crawling: from the seeds, follow links within the scope -- "
        "page-links (the links on the seeds), site, host (the default), pattern (URLs matching "
        "pattern) or any -- to depth links away, up to max_pages, reading each to its end.",
        "{\"type\": \"object\", \"properties\": {" ARCHIVE ", "
        "\"seeds\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}}, "
        "\"scope\": {\"type\": \"string\", \"enum\": [\"page-links\", \"site\", \"host\", "
        "\"pattern\", \"any\"]}, \"pattern\": {\"type\": \"string\"}, "
        "\"depth\": {\"type\": \"integer\"}, \"max_pages\": {\"type\": \"integer\"}, "
        "\"max_seconds\": {\"type\": \"number\"}, \"delay\": {\"type\": \"number\"}, "
        "\"scroll\": {\"type\": \"boolean\"}}, \"required\": [\"archive\", \"seeds\"]}",
        tool_crawl, FALSE},
    {"replay_archive", "Open a tab replaying a WACZ archive: it shows what the archive holds, "
        "and nothing else -- nothing is fetched from the web. Without url, it begins at the "
        "archive's only page, or at a list of its pages. Links followed from the tab stay in "
        "the replay. Read it as any tab (read_page, page_links, screenshot) to check what was "
        "archived; a page that wasn't says \"Not in the archive\".",
        "{\"type\": \"object\", \"properties\": {"
        "\"path\": {\"type\": \"string\", \"description\": \"The WACZ, as stop_archive gave it.\"}, "
        "\"url\": {\"type\": \"string\"}, \"timeout\": {\"type\": \"number\"}}, "
        "\"required\": [\"path\"]}", tool_replay_archive, FALSE},
    {"run_lua", "Run a Lua 5.4 script in the browser, once the person has approved it -- for work "
        "that would take the other tools many calls, such as visiting pages in a loop. It has "
        "string, table, math, utf8, os.time/clock/date, and the vimb table: vimb.log(...), "
        "vimb.sleep(seconds), vimb.tab(), vimb.tabs(), vimb.open(url) -> tab, info; "
        "vimb.archive.start(name) -> a; a:open(url) -> tab, info; a:crawl{seeds={...}, scope=, "
        "depth=, max_pages=, delay=} -> summary; a:stop() -> where it was saved; t:load(url) -> "
        "info; t:scroll(); t:text(); t:html(); t:source(); t:links() -> URLs; t:close(); t.url, "
        "t.title, t.status. Calls that fail return nil and why. The URLs it opens are held to "
        "the same rule as yours. What it logs and returns comes back to you; an archive it "
        "doesn't stop is saved once its tabs are closed.",
        "{\"type\": \"object\", \"properties\": {"
        "\"code\": {\"type\": \"string\"}, "
        "\"description\": {\"type\": \"string\", \"description\": \"In a sentence, what "
        "the script does, for the person deciding whether to let it run.\"}}, "
        "\"required\": [\"code\", \"description\"]}", tool_run_lua, FALSE, TRUE},
};

JsonNode *tools_list(const ToolPolicy *policy)
{
    JsonArray *list = json_array_new();
    for (gsize i = 0; i < G_N_ELEMENTS(tools); i++) {
        if ((tools[i].needs_eval && !policy->allow_eval)
                || (tools[i].needs_lua && !policy->approve_lua)) {
            continue;
        }
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "name", tools[i].name);
        json_object_set_string_member(o, "description", tools[i].description);
        JsonNode *schema = json_from_string(tools[i].schema, NULL);
        if (!policy->allow_archive_options && JSON_NODE_HOLDS_OBJECT(schema)
                && !strcmp(tools[i].name, "start_archive")) {
            JsonObject *props = json_object_get_object_member(json_node_get_object(schema),
                    "properties");
            json_object_remove_member(props, "strict_ssl");
            json_object_remove_member(props, "allow_internal");
        }
        if (!policy->allow_signins && JSON_NODE_HOLDS_OBJECT(schema)
                && !strcmp(tools[i].name, "start_archive")) {
            json_object_remove_member(json_object_get_object_member(json_node_get_object(schema),
                        "properties"), "signed_in");
        }
        json_object_set_member(o, "inputSchema", schema);
        json_array_add_object_element(list, o);
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, list);
    return node;
}

void tools_call(const char *name, JsonObject *arguments, const ToolPolicy *policy,
        ToolDone done, gpointer data)
{
    Call *call   = g_new0(Call, 1);
    call->args   = arguments ? json_object_ref(arguments) : json_object_new();
    call->policy = policy;
    call->done   = done;
    call->data   = data;
    for (gsize i = 0; i < G_N_ELEMENTS(tools); i++) {
        if (!strcmp(tools[i].name, name) && (!tools[i].needs_eval || policy->allow_eval)
                && (!tools[i].needs_lua || policy->approve_lua)) {
            call->tool = &tools[i];
            tools[i].run(call);
            return;
        }
    }
    call_fail(call, "there is no tool %s", name);
}

guint tools_tab_number(Client *c)
{
    return tab_number(c);
}

void tools_release(void)
{
    if (!archives) {
        return;
    }
    GHashTableIter iter;
    gpointer a;
    g_hash_table_iter_init(&iter, archives);
    while (g_hash_table_iter_next(&iter, NULL, &a)) {
        if (archiving_alive(a)) {
            archiving_release(a);
        }
    }
    g_hash_table_remove_all(archives);
}
