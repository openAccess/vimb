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

#include <libsoup/soup.h>
#include <string.h>

#include "crawl.h"

typedef struct {
    char  *url;
    guint depth;
} Item;

typedef struct {
    guint64       tab;
    CrawlOptions  o;
    GRegex        *regex;
    GQueue        *queue;
    GHashTable    *seen;        /* URLs without fragment */
    GHashTable    *last_load;   /* host -> monotonic time of its last load */
    GHashTable    *seed_hosts;
    GHashTable    *seed_sites;
    CrawlPageFunc page;
    OpsCallback   done;
    gpointer      data;
    gint64        start;
    guint         pages;
    guint         loaded;
    guint         failed;
    Item          *current;
    guint         timer;
} Crawl;

static void item_free(gpointer data)
{
    Item *item = data;
    if (item) {
        g_free(item->url);
        g_free(item);
    }
}

static char *host_of(const char *url)
{
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, NULL);
    char *host = NULL;
    if (uri) {
        const char *scheme = g_uri_get_scheme(uri);
        if (g_uri_get_host(uri) && (!g_ascii_strcasecmp(scheme, "http")
                    || !g_ascii_strcasecmp(scheme, "https"))) {
            host = g_ascii_strdown(g_uri_get_host(uri), -1);
            gsize len = strlen(host);
            if (len && host[len - 1] == '.') {
                host[len - 1] = '\0';
            }
        }
        g_uri_unref(uri);
    }
    return host;
}

/* The registrable domain -- "bbc.co.uk" for "www.bbc.co.uk" -- or the host
 * itself where there is none: an address, "localhost". */
static char *site_of(const char *host)
{
    const char *base = soup_tld_get_base_domain(host, NULL);
    return g_strdup(base ? base : host);
}

static char *without_fragment(const char *url)
{
    const char *hash = strchr(url, '#');
    return hash ? g_strndup(url, hash - url) : g_strdup(url);
}

static gboolean in_scope(Crawl *k, const char *url, guint from_depth)
{
    char *host      = host_of(url);
    gboolean result = FALSE;
    if (host) {
        switch (k->o.scope) {
            case CRAWL_SCOPE_PAGE_LINKS:
                result = from_depth == 0;
                break;
            case CRAWL_SCOPE_SITE: {
                char *site = site_of(host);
                result = g_hash_table_contains(k->seed_sites, site);
                g_free(site);
                break;
            }
            case CRAWL_SCOPE_HOST:
                result = g_hash_table_contains(k->seed_hosts, host);
                break;
            case CRAWL_SCOPE_PATTERN:
                result = g_regex_match(k->regex, url, 0, NULL);
                break;
            case CRAWL_SCOPE_ANY:
                result = TRUE;
                break;
        }
    }
    g_free(host);
    return result;
}

static void enqueue(Crawl *k, const char *url, guint depth)
{
    char *key = without_fragment(url);
    if (g_hash_table_contains(k->seen, key)) {
        g_free(key);
        return;
    }
    Item *item  = g_new0(Item, 1);
    item->url   = g_strdup(key);
    item->depth = depth;
    g_hash_table_add(k->seen, key);
    g_queue_push_tail(k->queue, item);
}

static void crawl_free(Crawl *k)
{
    if (k->timer) {
        g_source_remove(k->timer);
    }
    item_free(k->current);
    g_queue_free_full(k->queue, item_free);
    g_hash_table_unref(k->seen);
    g_hash_table_unref(k->last_load);
    g_hash_table_unref(k->seed_hosts);
    g_hash_table_unref(k->seed_sites);
    if (k->regex) {
        g_regex_unref(k->regex);
    }
    g_strfreev(k->o.seeds);
    g_free(k->o.pattern);
    g_free(k);
}

static void finish(Crawl *k, const char *stopped, const char *error)
{
    JsonNode *value = NULL;
    if (!error) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "pages");
        json_builder_add_int_value(b, k->pages);
        json_builder_set_member_name(b, "loaded");
        json_builder_add_int_value(b, k->loaded);
        json_builder_set_member_name(b, "failed");
        json_builder_add_int_value(b, k->failed);
        json_builder_set_member_name(b, "left");
        json_builder_add_int_value(b, g_queue_get_length(k->queue));
        json_builder_set_member_name(b, "seconds");
        json_builder_add_double_value(b, (g_get_monotonic_time() - k->start) / 1e6);
        json_builder_set_member_name(b, "stopped");
        json_builder_add_string_value(b, stopped);
        json_builder_end_object(b);
        value = json_builder_get_root(b);
        g_object_unref(b);
    }
    k->done(error == NULL, error, value, k->data);
    if (value) {
        json_node_unref(value);
    }
    crawl_free(k);
}

static void next(Crawl *k);

static void advance(Crawl *k)
{
    item_free(k->current);
    k->current = NULL;
    next(k);
}

static void on_links(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Crawl *k = data;
    if (ok && value && JSON_NODE_HOLDS_ARRAY(value)) {
        JsonArray *links = json_node_get_array(value);
        for (guint i = 0; i < json_array_get_length(links); i++) {
            JsonNode *node = json_array_get_element(links, i);
            if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING) {
                const char *url = json_node_get_string(node);
                if (in_scope(k, url, k->current->depth)) {
                    enqueue(k, url, k->current->depth + 1);
                }
            }
        }
    }
    advance(k);
}

/* Links are followed from a page that loaded, short of the depth -- and
 * for page-links scope, only from the seeds. */
static void after_page(Crawl *k, gboolean ok)
{
    Client *c = ops_find_tab(k->tab);
    gboolean follow = ok && c && k->current->depth < k->o.depth
        && (k->o.scope != CRAWL_SCOPE_PAGE_LINKS || k->current->depth == 0);
    if (follow) {
        ops_links(c, on_links, k);
    } else {
        advance(k);
    }
}

static void on_scrolled(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Crawl *k = data;
    if (ok && value && JSON_NODE_HOLDS_OBJECT(value)) {
        JsonObject *o = json_node_get_object(value);
        g_debug("crawl: read %s: %" G_GINT64_FORMAT " screens, %s", k->current->url,
                json_object_get_int_member_with_default(o, "screens", 0),
                json_object_get_boolean_member_with_default(o, "reached_end", FALSE)
                ? "to its end" : "not to its end");
    }
    after_page(k, TRUE);
}

static void on_loaded(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Crawl *k     = data;
    guint status = 0;
    const char *title = NULL;
    if (ok && value && JSON_NODE_HOLDS_OBJECT(value)) {
        JsonObject *o = json_node_get_object(value);
        status = (guint)json_object_get_int_member_with_default(o, "status", 0);
        title  = json_object_get_string_member_with_default(o, "title", NULL);
    }
    if (ok) {
        k->loaded++;
    } else {
        k->failed++;
    }
    if (k->page) {
        k->page(k->current->url, k->current->depth, ok, status, title, error, k->data);
    }
    Client *c = ops_find_tab(k->tab);
    if (ok && k->o.scroll && c) {
        ops_scroll(c, on_scrolled, k);
    } else {
        after_page(k, ok);
    }
}

static gboolean load_current(gpointer data)
{
    Crawl *k  = data;
    Client *c = ops_find_tab(k->tab);
    k->timer  = 0;
    if (!c) {
        finish(k, NULL, "the crawl's tab closed");
        return G_SOURCE_REMOVE;
    }
    char *host = host_of(k->current->url);
    if (host) {
        g_hash_table_insert(k->last_load, host, g_memdup2(&(gint64){g_get_monotonic_time()},
                    sizeof(gint64)));
    }
    k->pages++;
    ops_load(c, k->current->url, k->o.timeout, on_loaded, k);
    return G_SOURCE_REMOVE;
}

static void next(Crawl *k)
{
    if (k->o.max_pages && k->pages >= k->o.max_pages) {
        finish(k, "max_pages", NULL);
        return;
    }
    if (k->o.max_seconds > 0
            && (g_get_monotonic_time() - k->start) / 1e6 >= k->o.max_seconds) {
        finish(k, "max_seconds", NULL);
        return;
    }
    k->current = g_queue_pop_head(k->queue);
    if (!k->current) {
        finish(k, "done", NULL);
        return;
    }
    /* A host is given delay seconds between the starts of its pages. */
    char *host    = host_of(k->current->url);
    gint64 *last  = host ? g_hash_table_lookup(k->last_load, host) : NULL;
    gint64 wait   = 0;
    if (last) {
        wait = (gint64)(k->o.delay * 1000) - (g_get_monotonic_time() - *last) / 1000;
    }
    g_free(host);
    if (wait > 0) {
        k->timer = g_timeout_add((guint)wait, load_current, k);
    } else {
        k->timer = g_idle_add(load_current, k);
    }
}

static gboolean start(gpointer data)
{
    Crawl *k = data;
    k->timer = 0;
    next(k);            /* which may finish, and free, the crawl */
    return G_SOURCE_REMOVE;
}

typedef struct {
    OpsCallback done;
    gpointer    data;
    char        *error;
} Failure;

static gboolean fail_later(gpointer data)
{
    Failure *f = data;
    f->done(FALSE, f->error, NULL, f->data);
    g_free(f->error);
    g_free(f);
    return G_SOURCE_REMOVE;
}

void crawl_run(Client *c, const CrawlOptions *options, CrawlPageFunc page, OpsCallback done,
        gpointer data)
{
    GError *error = NULL;
    GRegex *regex = NULL;
    if (options->scope == CRAWL_SCOPE_PATTERN
            && !(regex = g_regex_new(options->pattern ? options->pattern : "", 0, 0, &error))) {
        Failure *f = g_new0(Failure, 1);
        f->done    = done;
        f->data    = data;
        f->error   = g_strdup_printf("bad pattern: %s", error->message);
        g_error_free(error);
        g_idle_add(fail_later, f);
        return;
    }

    Crawl *k      = g_new0(Crawl, 1);
    k->tab        = ops_tab_id(c);
    k->o          = *options;
    k->o.seeds    = g_strdupv(options->seeds);
    k->o.pattern  = g_strdup(options->pattern);
    k->regex      = regex;
    k->queue      = g_queue_new();
    k->seen       = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    k->last_load  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    k->seed_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    k->seed_sites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    k->page       = page;
    k->done       = done;
    k->data       = data;
    k->start      = g_get_monotonic_time();

    for (char **s = k->o.seeds; s && *s; s++) {
        char *host = host_of(*s);
        if (host) {
            g_hash_table_add(k->seed_sites, site_of(host));
            g_hash_table_add(k->seed_hosts, host);
            enqueue(k, *s, 0);
        }
    }
    k->timer = g_idle_add(start, k);
}
