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

#include "ops.h"

/* The script world vimb's own page scripts run in. */
#define WORLD               "vimb"
#define SCROLL_MAX_SCREENS  40
#define SCROLL_INTERVAL_MS  350
#define SCROLL_STEP_MS      3000
/* At its end, a page is left a moment -- and then until the images near the
 * screen have arrived, up to a limit -- for what reading down to it started
 * loading (a lazy image) to be archived before the page is left. A fixed
 * wait would be too short on a slow machine or network. */
#define SCROLL_SETTLE_MS    500

extern struct Vimb vb;

/* Down most of a screen; whether that was the end: it didn't move, or the
 * bottom of the screen is at the bottom of the page. Instant, not smooth, so
 * the page has moved by the time it is asked how far it got. */
static const char scroll_step_script[] =
    "(() => {"
    "  const page = document.scrollingElement || document.documentElement;"
    "  if (!page) return true;"
    "  const before = page.scrollTop;"
    "  window.scrollBy({top: Math.max(200, Math.floor(innerHeight * 0.8)), behavior: 'instant'});"
    "  return page.scrollTop === before || page.scrollTop + innerHeight >= page.scrollHeight - 2;"
    "})()";
static const char scroll_top_script[] = "window.scrollTo({top: 0, behavior: 'instant'})";
/* The body of an async function, run at the end of a page: what the page
 * would load lazily -- images and frames marked loading="lazy" -- it loads
 * now, since a reader at the end would have loaded it, rather than leave it
 * to WebKit, which sees to it only when it next renders and can be late.
 * Then waits until no image is still loading, for at most five seconds, and
 * returns how many still are. */
static const char settle_script[] =
    "for (const e of document.querySelectorAll('img[loading=lazy], iframe[loading=lazy]'))"
    "  e.loading = 'eager';"
    "const deadline = Date.now() + 5000;"
    "for (;;) {"
    "  const pending = Array.from(document.images).filter(i => !i.complete).length;"
    "  if (!pending || Date.now() > deadline) return pending;"
    "  await new Promise(resolve => setTimeout(resolve, 100));"
    "}";
static const char text_script[] =
    "document.body ? document.body.innerText : document.documentElement.innerText";
static const char html_script[] =
    "(document.doctype ? new XMLSerializer().serializeToString(document.doctype) + '\\n' : '')"
    " + document.documentElement.outerHTML";
static const char links_script[] =
    "Array.from(new Set(Array.from(document.querySelectorAll('a[href], area[href]'),"
    " a => a.href).filter(h => /^https?:/i.test(h)).map(h => h.split('#')[0])))";

guint64 ops_tab_id(Client *c)
{
    return c->webview_id;
}

Client *ops_find_tab(guint64 id)
{
    for (Client *c = vb.clients; c; c = c->next) {
        if (c->webview_id == id) {
            return c;
        }
    }
    return NULL;
}

guint ops_status(Client *c)
{
    WebKitWebResource *resource = webkit_web_view_get_main_resource(c->webview);
    WebKitURIResponse *response = resource ? webkit_web_resource_get_response(resource) : NULL;
    return response ? webkit_uri_response_get_status_code(response) : 0;
}

/* ---- calling back, always from the main loop --------------------------- */

typedef struct {
    OpsCallback callback;
    gpointer    data;
    gboolean    ok;
    char        *error;
    JsonNode    *value;
} Result;

static gboolean deliver(gpointer data)
{
    Result *r = data;
    r->callback(r->ok, r->error, r->value, r->data);
    if (r->value) {
        json_node_unref(r->value);
    }
    g_free(r->error);
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* Calls back from an idle, never from within the call that started the
 * operation: a script yields after starting one, and must have yielded
 * before it is resumed. Takes the value. */
static void complete(OpsCallback callback, gpointer data, gboolean ok, const char *error,
        JsonNode *value)
{
    Result *r   = g_new0(Result, 1);
    r->callback = callback;
    r->data     = data;
    r->ok       = ok;
    r->error    = g_strdup(error);
    r->value    = value;
    g_idle_add(deliver, r);
}

static JsonNode *string_node(const char *text)
{
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, text ? text : "");
    return node;
}

/* ---- loads ------------------------------------------------------------- */

typedef struct {
    Client      *c;
    OpsCallback callback;
    gpointer    data;
    gulong      changed_id;
    gulong      failed_id;
    guint       timeout_id;
    double      timeout;
    gboolean    started;
    char        *error;
} LoadWait;

static GHashTable *loads;       /* Client * -> LoadWait * */

static JsonNode *load_value(Client *c, const char *title)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "url");
    json_builder_add_string_value(b, webkit_web_view_get_uri(c->webview));
    json_builder_set_member_name(b, "status");
    json_builder_add_int_value(b, ops_status(c));
    json_builder_set_member_name(b, "title");
    json_builder_add_string_value(b, title ? title : "");
    json_builder_end_object(b);
    JsonNode *value = json_builder_get_root(b);
    g_object_unref(b);
    return value;
}

typedef struct {
    guint64     tab;
    OpsCallback callback;
    gpointer    data;
} TitleCall;

/* The title WebKit hadn't told of yet, read from the page itself. */
static void on_title(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    TitleCall *call   = data;
    Client *c         = ops_find_tab(call->tab);
    const char *title = ok && value && JSON_NODE_HOLDS_VALUE(value)
        && json_node_get_value_type(value) == G_TYPE_STRING ? json_node_get_string(value) : NULL;
    if (c) {
        complete(call->callback, call->data, TRUE, NULL, load_value(c, title));
    } else {
        complete(call->callback, call->data, FALSE, "the tab closed", NULL);
    }
    g_free(call);
}

static void load_end(LoadWait *w, gboolean ok, const char *error)
{
    JsonNode *value = NULL;

    g_hash_table_remove(loads, w->c);
    g_signal_handler_disconnect(w->c->webview, w->changed_id);
    g_signal_handler_disconnect(w->c->webview, w->failed_id);
    if (w->timeout_id) {
        g_source_remove(w->timeout_id);
    }
    const char *title = webkit_web_view_get_title(w->c->webview);
    if (ok && (!title || !*title)) {
        /* WebKit may tell of the title a moment after the load. */
        TitleCall *call = g_new0(TitleCall, 1);
        call->tab       = ops_tab_id(w->c);
        call->callback  = w->callback;
        call->data      = w->data;
        ops_eval(w->c, "document.title", on_title, call);
    } else {
        if (ok) {
            value = load_value(w->c, title);
        }
        complete(w->callback, w->data, ok, error, value);
    }
    g_free(w->error);
    g_free(w);
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    LoadWait *w = data;
    if (event == WEBKIT_LOAD_STARTED) {
        w->started = TRUE;
    } else if (event == WEBKIT_LOAD_FINISHED && w->started) {
        /* The load-failed signal, if any, came first. */
        char *error = g_strdup(w->error);
        load_end(w, error == NULL, error);
        g_free(error);
    }
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri,
        GError *error, gpointer data)
{
    LoadWait *w = data;
    g_free(w->error);
    w->error = g_strdup_printf("%s: %s", uri, error->message);
    return FALSE;
}

static gboolean on_load_timeout(gpointer data)
{
    LoadWait *w   = data;
    char *message = g_strdup_printf("the page didn't finish loading within %g seconds", w->timeout);
    w->timeout_id = 0;
    load_end(w, FALSE, message);
    g_free(message);
    return G_SOURCE_REMOVE;
}

void ops_load(Client *c, const char *url, double timeout, OpsCallback callback, gpointer data)
{
    if (!loads) {
        loads = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    LoadWait *previous = g_hash_table_lookup(loads, c);
    if (previous) {
        load_end(previous, FALSE, "superseded by another load");
    }
    LoadWait *w   = g_new0(LoadWait, 1);
    w->c          = c;
    w->callback   = callback;
    w->data       = data;
    w->timeout    = timeout;
    w->changed_id = g_signal_connect(c->webview, "load-changed", G_CALLBACK(on_load_changed), w);
    w->failed_id  = g_signal_connect(c->webview, "load-failed", G_CALLBACK(on_load_failed), w);
    if (timeout > 0) {
        w->timeout_id = g_timeout_add((guint)(timeout * 1000), on_load_timeout, w);
    }
    g_hash_table_insert(loads, c, w);
    webkit_web_view_load_uri(c->webview, url);
}

void ops_tab_closed(Client *c)
{
    LoadWait *w = loads ? g_hash_table_lookup(loads, c) : NULL;
    if (w) {
        load_end(w, FALSE, "the tab closed");
    }
}

/* ---- scripts ----------------------------------------------------------- */

typedef struct {
    OpsCallback callback;
    gpointer    data;
} EvalCall;

static JsonNode *value_to_node(JSCValue *value)
{
    if (jsc_value_is_undefined(value) || jsc_value_is_null(value)) {
        return json_node_new(JSON_NODE_NULL);
    }
    char *json     = jsc_value_to_json(value, 0);
    JsonNode *node = json ? json_from_string(json, NULL) : NULL;
    g_free(json);
    if (!node) {
        char *text = jsc_value_to_string(value);
        node = string_node(text);
        g_free(text);
    }
    return node;
}

static void on_eval(GObject *source, GAsyncResult *result, gpointer data)
{
    EvalCall *call = data;
    GError *error  = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result,
            &error);
    if (value) {
        JsonNode *node = value_to_node(value);
        call->callback(TRUE, NULL, node, call->data);
        json_node_unref(node);
        g_object_unref(value);
    } else {
        call->callback(FALSE, error->message, NULL, call->data);
        g_error_free(error);
    }
    g_free(call);
}

void ops_eval(Client *c, const char *script, OpsCallback callback, gpointer data)
{
    EvalCall *call = g_new0(EvalCall, 1);
    call->callback = callback;
    call->data     = data;
    webkit_web_view_evaluate_javascript(c->webview, script, -1, WORLD, NULL, NULL, on_eval, call);
}

void ops_text(Client *c, OpsCallback callback, gpointer data)
{
    ops_eval(c, text_script, callback, data);
}

void ops_html(Client *c, OpsCallback callback, gpointer data)
{
    ops_eval(c, html_script, callback, data);
}

void ops_links(Client *c, OpsCallback callback, gpointer data)
{
    ops_eval(c, links_script, callback, data);
}

/* ---- the source -------------------------------------------------------- */

static void on_source(GObject *source, GAsyncResult *result, gpointer data)
{
    EvalCall *call = data;
    GError *error  = NULL;
    gsize len      = 0;
    guchar *bytes  = webkit_web_resource_get_data_finish(WEBKIT_WEB_RESOURCE(source), result,
            &len, &error);
    if (!bytes) {
        call->callback(FALSE, error->message, NULL, call->data);
        g_error_free(error);
    } else {
        /* As text: UTF-8 when it is, and otherwise read as Latin-1, which
         * any bytes are. */
        char *text = g_utf8_validate((const char *)bytes, (gssize)len, NULL)
            ? g_strndup((const char *)bytes, len)
            : g_convert((const char *)bytes, (gssize)len, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        JsonNode *node = string_node(text);
        call->callback(TRUE, NULL, node, call->data);
        json_node_unref(node);
        g_free(text);
        g_free(bytes);
    }
    g_free(call);
}

void ops_source(Client *c, OpsCallback callback, gpointer data)
{
    WebKitWebResource *resource = webkit_web_view_get_main_resource(c->webview);
    if (!resource) {
        complete(callback, data, FALSE, "the tab has no page", NULL);
        return;
    }
    EvalCall *call = g_new0(EvalCall, 1);
    call->callback = callback;
    call->data     = data;
    webkit_web_resource_get_data(resource, NULL, on_source, call);
}

/* ---- scrolling --------------------------------------------------------- */

typedef struct {
    guint64     tab;
    OpsCallback callback;
    gpointer    data;
    int         screens;
    guint       timer;
    gboolean    settling;   /* left at the end a moment; steps are over */
    gboolean    settled;
    gboolean    reached_end;
    gboolean    done;
    int         refs;       /* this, and a step in flight */
} Scroll;

static void scroll_unref(Scroll *s)
{
    if (--s->refs == 0) {
        g_free(s);
    }
}

static void scroll_finish(Scroll *s, gboolean reached_end, const char *error);

static void on_settled(GObject *source, GAsyncResult *result, gpointer data)
{
    Scroll *s = data;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(
            WEBKIT_WEB_VIEW(source), result, NULL);
    g_clear_object(&value);
    s->settled = TRUE;
    scroll_finish(s, s->reached_end, NULL);
    scroll_unref(s);
}

static gboolean scroll_settled(gpointer data)
{
    Scroll *s = data;
    Client *c = ops_find_tab(s->tab);
    s->timer  = 0;
    if (!c) {
        s->settled = TRUE;
        scroll_finish(s, s->reached_end, NULL);
        return G_SOURCE_REMOVE;
    }
    s->refs++;
    webkit_web_view_call_async_javascript_function(c->webview, settle_script, -1, NULL, WORLD,
            NULL, NULL, on_settled, s);
    return G_SOURCE_REMOVE;
}

static void scroll_finish(Scroll *s, gboolean reached_end, const char *error)
{
    if (s->done) {
        return;
    }
    if (!error && !s->settled) {
        /* Left a moment at the end first, for what that started loading. */
        if (s->timer) {
            g_source_remove(s->timer);
        }
        s->reached_end = reached_end;
        s->settling    = TRUE;
        s->timer       = g_timeout_add(SCROLL_SETTLE_MS, scroll_settled, s);
        return;
    }
    s->done = TRUE;
    if (s->timer) {
        g_source_remove(s->timer);
        s->timer = 0;
    }
    Client *c = ops_find_tab(s->tab);
    if (c) {
        webkit_web_view_evaluate_javascript(c->webview, scroll_top_script, -1, WORLD, NULL, NULL,
                NULL, NULL);
    }
    JsonNode *value = NULL;
    if (!error) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "screens");
        json_builder_add_int_value(b, s->screens);
        json_builder_set_member_name(b, "reached_end");
        json_builder_add_boolean_value(b, reached_end);
        json_builder_end_object(b);
        value = json_builder_get_root(b);
        g_object_unref(b);
    }
    complete(s->callback, s->data, error == NULL, error, value);
    scroll_unref(s);
}

static gboolean scroll_step(gpointer data);

static gboolean scroll_step_timeout(gpointer data)
{
    Scroll *s = data;
    s->timer  = 0;
    scroll_finish(s, FALSE, NULL);      /* too busy to answer: as far as it goes */
    return G_SOURCE_REMOVE;
}

static void on_scroll_step(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Scroll *s = data;
    if (!s->done && !s->settling) {
        if (s->timer) {
            g_source_remove(s->timer);
            s->timer = 0;
        }
        gboolean at_end = !ok || !value || JSON_NODE_TYPE(value) != JSON_NODE_VALUE
            || json_node_get_value_type(value) != G_TYPE_BOOLEAN || json_node_get_boolean(value);
        if (at_end) {
            scroll_finish(s, ok, NULL);
        } else {
            s->timer = g_timeout_add(SCROLL_INTERVAL_MS, scroll_step, s);
        }
    }
    scroll_unref(s);
}

static gboolean scroll_step(gpointer data)
{
    Scroll *s = data;
    Client *c = ops_find_tab(s->tab);
    s->timer  = 0;
    if (!c) {
        scroll_finish(s, FALSE, "the tab closed");
        return G_SOURCE_REMOVE;
    }
    if (s->screens >= SCROLL_MAX_SCREENS) {
        scroll_finish(s, FALSE, NULL);
        return G_SOURCE_REMOVE;
    }
    s->screens++;
    s->refs++;
    ops_eval(c, scroll_step_script, on_scroll_step, s);
    s->timer = g_timeout_add(SCROLL_STEP_MS, scroll_step_timeout, s);
    return G_SOURCE_REMOVE;
}

void ops_scroll(Client *c, OpsCallback callback, gpointer data)
{
    Scroll *s   = g_new0(Scroll, 1);
    s->tab      = ops_tab_id(c);
    s->callback = callback;
    s->data     = data;
    s->refs     = 1;
    s->timer    = g_idle_add(scroll_step, s);
}
