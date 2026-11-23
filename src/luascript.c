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

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

#include "archiving.h"
#include "crawl.h"
#include "luascript.h"
#include "main.h"
#include "ops.h"
#include "../version.h"

#define TAB_META     "vimb.Tab"
#define ARCHIVE_META "vimb.Archive"

extern struct Vimb vb;

typedef struct {
    lua_State *co;          /* the script's coroutine */
    int       ref;          /* keeps it from being collected while it runs */
    gboolean  exit_when_done;
    char      *name;
} Script;

typedef struct {
    guint64 id;
} LuaTab;

typedef struct {
    struct Archive *archive;
} LuaArchive;

static lua_State *L;        /* the main state, whose threads scripts are */

static Script *script_of(lua_State *co)
{
    return *(Script **)lua_getextraspace(co);
}

static void report(Client *c, gboolean error, const char *message)
{
    fprintf(error ? stderr : stdout, "%s\n", message);
    fflush(error ? stderr : stdout);
    if (c || (c = vb_get_current_client())) {
        vb_echo(c, error ? MSG_ERROR : MSG_NORMAL, FALSE, "%s", message);
    }
}

/* ---- running ----------------------------------------------------------- */

static void script_end(Script *s, int status)
{
    if (status != LUA_OK) {
        const char *message = lua_tostring(s->co, -1);
        luaL_traceback(L, s->co, message ? message : "error", 0);
        char *text = g_strdup_printf("vimb: %s: %s", s->name, lua_tostring(L, -1));
        report(NULL, TRUE, text);
        g_free(text);
        lua_pop(L, 1);
    }
    gboolean exit_when_done = s->exit_when_done;
    luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
    g_free(s->name);
    g_free(s);
    if (exit_when_done) {
        vb_exit(status == LUA_OK ? 0 : 1);
    }
}

/* Resumes a script with nargs values on its stack: the results of what it
 * was waiting for. */
static void script_resume(Script *s, int nargs)
{
    int nresults = 0;
    int status   = lua_resume(s->co, L, nargs, &nresults);
    if (status == LUA_YIELD) {
        lua_pop(s->co, nresults);   /* waiting again */
        return;
    }
    script_end(s, status);
}

/* Suspends the script until a callback resumes it. */
static int wait_for(lua_State *co)
{
    if (!lua_isyieldable(co)) {
        return luaL_error(co, "can't wait here (inside a callback or a metamethod)");
    }
    return lua_yield(co, 0);
}

/* ---- values ------------------------------------------------------------ */

static void push_json(lua_State *co, JsonNode *node)
{
    if (!node) {
        lua_pushnil(co);
        return;
    }
    switch (JSON_NODE_TYPE(node)) {
        case JSON_NODE_NULL:
            lua_pushnil(co);
            break;
        case JSON_NODE_VALUE:
            switch (json_node_get_value_type(node)) {
                case G_TYPE_STRING:  lua_pushstring(co, json_node_get_string(node));   break;
                case G_TYPE_INT64:   lua_pushinteger(co, json_node_get_int(node));     break;
                case G_TYPE_DOUBLE:  lua_pushnumber(co, json_node_get_double(node));   break;
                case G_TYPE_BOOLEAN: lua_pushboolean(co, json_node_get_boolean(node)); break;
                default:             lua_pushnil(co);                                  break;
            }
            break;
        case JSON_NODE_ARRAY: {
            JsonArray *array = json_node_get_array(node);
            guint n = json_array_get_length(array);
            lua_createtable(co, (int)n, 0);
            for (guint i = 0; i < n; i++) {
                push_json(co, json_array_get_element(array, i));
                lua_rawseti(co, -2, (lua_Integer)i + 1);
            }
            break;
        }
        case JSON_NODE_OBJECT: {
            JsonObjectIter iter;
            const char *name;
            JsonNode *member;
            lua_newtable(co);
            json_object_iter_init_ordered(&iter, json_node_get_object(node));
            while (json_object_iter_next_ordered(&iter, &name, &member)) {
                push_json(co, member);
                lua_setfield(co, -2, name);
            }
            break;
        }
    }
}

/* Resumes with the value, or with nil and why not. */
static void resume_value(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    Script *s = data;
    if (ok) {
        push_json(s->co, value);
        script_resume(s, 1);
    } else {
        lua_pushnil(s->co);
        lua_pushstring(s->co, error);
        script_resume(s, 2);
    }
}

static void push_tab(lua_State *co, Client *c)
{
    LuaTab *t = lua_newuserdatauv(co, sizeof(LuaTab), 0);
    t->id     = ops_tab_id(c);
    luaL_setmetatable(co, TAB_META);
}

static Client *check_tab(lua_State *co, int index)
{
    LuaTab *t = luaL_checkudata(co, index, TAB_META);
    Client *c = ops_find_tab(t->id);
    if (!c) {
        luaL_error(co, "the tab is closed");
    }
    return c;
}

static struct Archive *check_archive(lua_State *co, int index)
{
    LuaArchive *u = luaL_checkudata(co, index, ARCHIVE_META);
    if (!archiving_alive(u->archive)) {
        luaL_error(co, "the archive is gone");
    }
    return u->archive;
}

static double opt_number(lua_State *co, int index, const char *field, double fallback)
{
    double value = fallback;
    if (lua_istable(co, index)) {
        lua_getfield(co, index, field);
        value = luaL_optnumber(co, -1, fallback);
        lua_pop(co, 1);
    }
    return value;
}

static gboolean opt_boolean(lua_State *co, int index, const char *field, gboolean fallback)
{
    gboolean value = fallback;
    if (lua_istable(co, index)) {
        lua_getfield(co, index, field);
        if (!lua_isnil(co, -1)) {
            value = lua_toboolean(co, -1);
        }
        lua_pop(co, 1);
    }
    return value;
}

/* -1 for a field not given, to follow the settings. */
static int opt_tristate(lua_State *co, int index, const char *field)
{
    int value = -1;
    if (lua_istable(co, index)) {
        lua_getfield(co, index, field);
        if (!lua_isnil(co, -1)) {
            value = lua_toboolean(co, -1);
        }
        lua_pop(co, 1);
    }
    return value;
}

/* ---- vimb -------------------------------------------------------------- */

static int l_log(lua_State *co)
{
    luaL_Buffer b;
    int n = lua_gettop(co);
    luaL_buffinit(co, &b);
    for (int i = 1; i <= n; i++) {
        luaL_tolstring(co, i, NULL);
        luaL_addvalue(&b);
        if (i < n) {
            luaL_addchar(&b, '\t');
        }
    }
    luaL_pushresult(&b);
    report(NULL, FALSE, lua_tostring(co, -1));
    return 0;
}

static gboolean sleep_done(gpointer data)
{
    script_resume(data, 0);
    return G_SOURCE_REMOVE;
}

static int l_sleep(lua_State *co)
{
    double seconds = luaL_checknumber(co, 1);
    g_timeout_add((guint)(MAX(seconds, 0) * 1000), sleep_done, script_of(co));
    return wait_for(co);
}

static int l_quit(lua_State *co)
{
    vb_exit((int)luaL_optinteger(co, 1, 0));
    return wait_for(co);            /* and is never resumed */
}

static int l_tab(lua_State *co)
{
    Client *c = vb_get_current_client();
    if (!c) {
        lua_pushnil(co);
    } else {
        push_tab(co, c);
    }
    return 1;
}

static int l_tabs(lua_State *co)
{
    int i = 1;
    lua_newtable(co);
    for (Client *c = vb.clients; c; c = c->next) {
        push_tab(co, c);
        lua_rawseti(co, -2, i++);
    }
    return 1;
}

typedef struct {
    Script  *script;
    guint64 tab;
} TabCall;

/* Resumes an open with the tab and what loaded -- or nil, why not, and the
 * tab, which is left open. */
static void resume_open(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    TabCall *call = data;
    Script *s     = call->script;
    Client *c     = ops_find_tab(call->tab);
    g_free(call);
    if (ok && c) {
        push_tab(s->co, c);
        push_json(s->co, value);
        script_resume(s, 2);
    } else {
        lua_pushnil(s->co);
        lua_pushstring(s->co, error ? error : "the tab closed");
        if (c) {
            push_tab(s->co, c);
        } else {
            lua_pushnil(s->co);
        }
        script_resume(s, 3);
    }
}

static int open_in(lua_State *co, Client *c, const char *url, int options)
{
    TabCall *call = g_new0(TabCall, 1);
    call->script  = script_of(co);
    call->tab     = ops_tab_id(c);
    ops_load(c, url, opt_number(co, options, "timeout", 60), resume_open, call);
    return wait_for(co);
}

/* vimb.open(url [, {timeout=}]) -> tab, info -- in a new tab, not archived */
static int l_open(lua_State *co)
{
    const char *url = luaL_checkstring(co, 1);
    Client *c       = vb_tab_new(NULL, NULL, TRUE);
    return open_in(co, c, url, 2);
}

/* vimb.archive.start([name | {name=, strict_ssl=, allow_internal=}]) */
static int l_archive_start(lua_State *co)
{
    const char *name = lua_isstring(co, 1) ? lua_tostring(co, 1) : NULL;
    if (lua_istable(co, 1)) {
        lua_getfield(co, 1, "name");
        name = lua_tostring(co, -1);
        lua_pop(co, 1);
    }
    GError *error = NULL;
    Client *c     = vb_get_current_client();
    struct Archive *a = c ? archiving_create(c, name, opt_tristate(co, 1, "strict_ssl"),
            opt_tristate(co, 1, "allow_internal"), &error) : NULL;
    if (!a) {
        lua_pushnil(co);
        lua_pushstring(co, error ? error->message : "no tab to start from");
        g_clear_error(&error);
        return 2;
    }
    archiving_hold(a);
    LuaArchive *u = lua_newuserdatauv(co, sizeof(LuaArchive), 0);
    u->archive    = a;
    luaL_setmetatable(co, ARCHIVE_META);
    return 1;
}

/* ---- tabs -------------------------------------------------------------- */

/* t:load(url [, {timeout=}]) -> info | nil, error */
static int l_tab_load(lua_State *co)
{
    Client *c       = check_tab(co, 1);
    const char *url = luaL_checkstring(co, 2);
    ops_load(c, url, opt_number(co, 3, "timeout", 60), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_scroll(lua_State *co)
{
    ops_scroll(check_tab(co, 1), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_text(lua_State *co)
{
    ops_text(check_tab(co, 1), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_html(lua_State *co)
{
    ops_html(check_tab(co, 1), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_source(lua_State *co)
{
    ops_source(check_tab(co, 1), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_links(lua_State *co)
{
    ops_links(check_tab(co, 1), resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_eval(lua_State *co)
{
    Client *c          = check_tab(co, 1);
    const char *script = luaL_checkstring(co, 2);
    ops_eval(c, script, resume_value, script_of(co));
    return wait_for(co);
}

static int l_tab_close(lua_State *co)
{
    LuaTab *t = luaL_checkudata(co, 1, TAB_META);
    Client *c = ops_find_tab(t->id);
    if (c) {
        vb_tab_close(c);
    }
    return 0;
}

static int l_tab_index(lua_State *co)
{
    LuaTab *t       = luaL_checkudata(co, 1, TAB_META);
    const char *key = luaL_checkstring(co, 2);

    /* Methods first; they say for themselves if the tab has closed. */
    lua_getmetatable(co, 1);
    lua_getfield(co, -1, "methods");
    lua_getfield(co, -1, key);
    if (!lua_isnil(co, -1)) {
        return 1;
    }
    lua_pop(co, 3);

    Client *c = ops_find_tab(t->id);
    if (!strcmp(key, "closed")) {
        lua_pushboolean(co, c == NULL);
    } else if (!c) {
        return luaL_error(co, "the tab is closed");
    } else if (!strcmp(key, "url")) {
        lua_pushstring(co, webkit_web_view_get_uri(c->webview));
    } else if (!strcmp(key, "title")) {
        lua_pushstring(co, webkit_web_view_get_title(c->webview));
    } else if (!strcmp(key, "status")) {
        lua_pushinteger(co, ops_status(c));
    } else if (!strcmp(key, "archiving")) {
        lua_pushboolean(co, c->archive != NULL);
    } else {
        lua_pushnil(co);
    }
    return 1;
}

/* ---- archives ---------------------------------------------------------- */

/* a:open(url [, {timeout=}]) -> tab, info | nil, error, tab */
static int l_archive_open(lua_State *co)
{
    struct Archive *a = check_archive(co, 1);
    const char *url   = luaL_checkstring(co, 2);
    Client *c         = vb_tab_new_archive(a, NULL);
    if (!c) {
        lua_pushnil(co);
        lua_pushstring(co, "could not open a tab");
        return 2;
    }
    return open_in(co, c, url, 3);
}

static void resume_saved(const char *path, const char *error, gpointer data)
{
    Script *s = data;
    if (path) {
        lua_pushstring(s->co, path);
        script_resume(s, 1);
    } else {
        lua_pushnil(s->co);
        lua_pushstring(s->co, error);
        script_resume(s, 2);
    }
}

/* a:stop() -> path of the WACZ | nil, error */
static int l_archive_stop(lua_State *co)
{
    archiving_stop_async(check_archive(co, 1), resume_saved, script_of(co));
    return wait_for(co);
}

static int l_archive_pause(lua_State *co)
{
    lua_pushboolean(co, archiving_set_paused(check_archive(co, 1), TRUE));
    return 1;
}

static int l_archive_resume(lua_State *co)
{
    lua_pushboolean(co, archiving_set_paused(check_archive(co, 1), FALSE));
    return 1;
}

static int l_archive_rename(lua_State *co)
{
    archiving_rename(check_archive(co, 1), luaL_checkstring(co, 2));
    return 0;
}

typedef struct {
    Script  *script;
    guint64 tab;
    int     on_page;        /* registry ref of the script's function, or LUA_NOREF */
    gboolean log;
    gboolean keep_tab;
} CrawlCall;

/* Told of each page: logged if asked, and passed to on_page -- called on the
 * main state, since the script itself is suspended; it may not wait. */
static void crawl_page(const char *url, guint depth, gboolean ok, guint status,
        const char *title, const char *error, gpointer data)
{
    CrawlCall *call = data;
    if (call->log) {
        char *line = ok ? g_strdup_printf("crawl: %u %u %s %s", depth, status, url, title ? title : "")
            : g_strdup_printf("crawl: %u failed %s: %s", depth, url, error ? error : "");
        report(NULL, FALSE, line);
        g_free(line);
    }
    if (call->on_page != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, call->on_page);
        lua_newtable(L);
        lua_pushstring(L, url);
        lua_setfield(L, -2, "url");
        lua_pushinteger(L, depth);
        lua_setfield(L, -2, "depth");
        lua_pushboolean(L, ok);
        lua_setfield(L, -2, "ok");
        lua_pushinteger(L, status);
        lua_setfield(L, -2, "status");
        lua_pushstring(L, title);
        lua_setfield(L, -2, "title");
        lua_pushstring(L, error);
        lua_setfield(L, -2, "error");
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            char *text = g_strdup_printf("vimb: crawl on_page: %s", lua_tostring(L, -1));
            report(NULL, TRUE, text);
            g_free(text);
            lua_pop(L, 1);
        }
    }
}

static void crawl_done(gboolean ok, const char *error, JsonNode *value, gpointer data)
{
    CrawlCall *call = data;
    Client *c       = ops_find_tab(call->tab);
    if (c && !call->keep_tab) {
        vb_tab_close(c);
    }
    if (call->on_page != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, call->on_page);
    }
    Script *s = call->script;
    g_free(call);
    resume_value(ok, error, value, s);
}

static CrawlScope scope_of(lua_State *co, const char *name)
{
    static const char *names[] = {"page-links", "site", "host", "pattern", "any", NULL};
    static const CrawlScope scopes[] = {CRAWL_SCOPE_PAGE_LINKS, CRAWL_SCOPE_SITE,
        CRAWL_SCOPE_HOST, CRAWL_SCOPE_PATTERN, CRAWL_SCOPE_ANY};
    for (int i = 0; names[i]; i++) {
        if (!strcmp(name, names[i])) {
            return scopes[i];
        }
    }
    luaL_error(co, "scope must be page-links, site, host, pattern or any, not %s", name);
    return CRAWL_SCOPE_HOST;
}

/* a:crawl{seeds={...} | seed=, depth=, scope=, pattern=, max_pages=,
 *         max_seconds=, delay=, timeout=, scroll=, log=, keep_tab=, on_page=}
 *   -> summary | nil, error */
static int l_archive_crawl(lua_State *co)
{
    struct Archive *a = check_archive(co, 1);
    luaL_checktype(co, 2, LUA_TTABLE);

    GPtrArray *seeds = g_ptr_array_new_with_free_func(g_free);
    lua_getfield(co, 2, "seeds");
    if (lua_istable(co, -1)) {
        for (lua_Integer i = 1; lua_rawgeti(co, -1, i) == LUA_TSTRING; i++) {
            g_ptr_array_add(seeds, g_strdup(lua_tostring(co, -1)));
            lua_pop(co, 1);
        }
        lua_pop(co, 1);
    }
    lua_pop(co, 1);
    lua_getfield(co, 2, "seed");
    if (lua_isstring(co, -1)) {
        g_ptr_array_add(seeds, g_strdup(lua_tostring(co, -1)));
    }
    lua_pop(co, 1);
    if (!seeds->len) {
        g_ptr_array_free(seeds, TRUE);
        return luaL_error(co, "a crawl needs seeds={...} or seed=");
    }
    g_ptr_array_add(seeds, NULL);

    lua_getfield(co, 2, "scope");
    const char *scope = luaL_optstring(co, -1, "host");
    CrawlOptions options = {
        .seeds       = (char **)seeds->pdata,
        .depth       = (guint)opt_number(co, 2, "depth", 1),
        .scope       = scope_of(co, scope),
        .max_pages   = (guint)opt_number(co, 2, "max_pages", 100),
        .max_seconds = opt_number(co, 2, "max_seconds", 0),
        .delay       = opt_number(co, 2, "delay", 1),
        .timeout     = opt_number(co, 2, "timeout", 60),
        .scroll      = opt_boolean(co, 2, "scroll", TRUE),
    };
    lua_pop(co, 1);
    lua_getfield(co, 2, "pattern");
    char *pattern = g_strdup(lua_tostring(co, -1));
    lua_pop(co, 1);
    options.pattern = pattern;
    if (options.scope == CRAWL_SCOPE_PATTERN && !pattern) {
        g_free(pattern);
        g_ptr_array_free(seeds, TRUE);
        return luaL_error(co, "scope pattern needs pattern=");
    }

    CrawlCall *call = g_new0(CrawlCall, 1);
    call->script    = script_of(co);
    call->log       = opt_boolean(co, 2, "log", FALSE);
    call->keep_tab  = opt_boolean(co, 2, "keep_tab", FALSE);
    call->on_page   = LUA_NOREF;
    lua_getfield(co, 2, "on_page");
    if (lua_isfunction(co, -1)) {
        call->on_page = luaL_ref(co, LUA_REGISTRYINDEX);
    } else {
        lua_pop(co, 1);
    }
    Client *c  = vb_tab_new_archive(a, NULL);
    call->tab  = ops_tab_id(c);
    crawl_run(c, &options, crawl_page, crawl_done, call);
    g_free(pattern);
    g_ptr_array_free(seeds, TRUE);
    return wait_for(co);
}

static int l_archive_index(lua_State *co)
{
    LuaArchive *u   = luaL_checkudata(co, 1, ARCHIVE_META);
    const char *key = luaL_checkstring(co, 2);

    lua_getmetatable(co, 1);
    lua_getfield(co, -1, "methods");
    lua_getfield(co, -1, key);
    if (!lua_isnil(co, -1)) {
        return 1;
    }
    lua_pop(co, 3);

    if (!archiving_alive(u->archive)) {
        return luaL_error(co, "the archive is gone");
    }
    if (!strcmp(key, "name")) {
        lua_pushstring(co, archiving_name(u->archive));
    } else if (!strcmp(key, "state")) {
        lua_pushstring(co, archiving_state(u->archive));
    } else if (!strcmp(key, "responses")) {
        lua_pushinteger(co, archiving_recorded(u->archive));
    } else if (!strcmp(key, "saved")) {
        lua_pushstring(co, archiving_saved(u->archive));
    } else {
        lua_pushnil(co);
    }
    return 1;
}

static int l_archive_gc(lua_State *co)
{
    LuaArchive *u = luaL_checkudata(co, 1, ARCHIVE_META);
    if (archiving_alive(u->archive)) {
        archiving_release(u->archive);
    }
    u->archive = NULL;
    return 0;
}

/* ---- setting up -------------------------------------------------------- */

static void new_class(const char *meta, const luaL_Reg *methods, lua_CFunction index,
        lua_CFunction gc)
{
    luaL_newmetatable(L, meta);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "methods");
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    if (gc) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

static void init(void)
{
    static const luaL_Reg tab_methods[] = {
        {"load", l_tab_load}, {"scroll", l_tab_scroll}, {"text", l_tab_text},
        {"html", l_tab_html}, {"source", l_tab_source}, {"links", l_tab_links},
        {"eval", l_tab_eval}, {"close", l_tab_close}, {NULL, NULL}
    };
    static const luaL_Reg archive_methods[] = {
        {"open", l_archive_open}, {"crawl", l_archive_crawl}, {"stop", l_archive_stop},
        {"pause", l_archive_pause}, {"resume", l_archive_resume},
        {"rename", l_archive_rename}, {NULL, NULL}
    };
    static const luaL_Reg vimb_functions[] = {
        {"log", l_log}, {"sleep", l_sleep}, {"quit", l_quit}, {"tab", l_tab},
        {"tabs", l_tabs}, {"open", l_open}, {NULL, NULL}
    };

    L = luaL_newstate();
    luaL_openlibs(L);
    new_class(TAB_META, tab_methods, l_tab_index, NULL);
    new_class(ARCHIVE_META, archive_methods, l_archive_index, l_archive_gc);

    lua_newtable(L);
    luaL_setfuncs(L, vimb_functions, 0);
    lua_newtable(L);
    lua_pushcfunction(L, l_archive_start);
    lua_setfield(L, -2, "start");
    lua_setfield(L, -2, "archive");
    lua_pushstring(L, VERSION);
    lua_setfield(L, -2, "version");
    lua_pushboolean(L, vb.scripted);
    lua_setfield(L, -2, "scripted");
    lua_setglobal(L, "vimb");
}

/* Makes a script of the chunk on top of L's stack, and starts it. */
static void start(int load_status, const char *name, gboolean exit_when_done, Client *c)
{
    if (load_status != LUA_OK) {
        char *text = g_strdup_printf("vimb: %s", lua_tostring(L, -1));
        report(c, TRUE, text);
        g_free(text);
        lua_pop(L, 1);
        if (exit_when_done) {
            vb_exit(1);
        }
        return;
    }
    Script *s         = g_new0(Script, 1);
    s->name           = g_strdup(name);
    s->exit_when_done = exit_when_done;
    s->co             = lua_newthread(L);
    *(Script **)lua_getextraspace(s->co) = s;
    lua_insert(L, -2);                  /* thread below the chunk */
    lua_xmove(L, s->co, 1);             /* the chunk onto the thread */
    s->ref = luaL_ref(L, LUA_REGISTRYINDEX);  /* the thread */
    script_resume(s, 0);
}

void luascript_run_file(Client *c, const char *path, gboolean exit_when_done)
{
    if (!L) {
        init();
    }
    start(luaL_loadfile(L, path), path, exit_when_done, c);
}

void luascript_run_string(Client *c, const char *code)
{
    if (!L) {
        init();
    }
    start(luaL_loadbuffer(L, code, strlen(code), "=:lua"), ":lua", FALSE, c);
}
