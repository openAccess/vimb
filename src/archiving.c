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
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "archiving.h"
#include "main.h"
#include "util.h"
#include "../version.h"
#include "archive/proxy.h"
#include "archive/wacz.h"

#define CAPTURE_SUFFIX  ".capture"
#define LOCK_FILE       ".lock"
#define NAME_FILE       "name"
#define STATUS_NAME_MAX 24
/* How long a changed address must stand, with no load starting, to be taken
 * for a single-page app's next page: WebKit changes a tab's address a moment
 * before it says that a load has started. */
#define SPA_SETTLE_MS   300

extern struct Vimb vb;

typedef enum {
    ARCHIVE_RECORDING,
    ARCHIVE_PAUSED,
    ARCHIVE_SAVING,     /* being packaged */
    ARCHIVE_DONE,       /* packaged, or failed to be; tabs may remain */
} ArchiveState;

struct Archive {
    char                 *name;         /* what the person calls it */
    char                 *archives_dir;
    char                 *dir;          /* the capture's loose files */
    char                 *warc_name;
    int                  lock_fd;
    gboolean             verifies;      /* the proxy checks sites' certificates */
    ArchiveProxy         *proxy;
    WebKitNetworkSession *session;
    ArchiveState         state;
    guint                tabs;
    FILE                 *pages;
    char                 *status;
    GThread              *packager;
    guint                done_source;   /* package_done(), queued by the packager */
    char                 *saved;        /* where it was packaged */
    GError               *error;        /* why it wasn't */
};

/* Where each archiving tab is in listing its pages. */
typedef struct {
    struct Archive *archive;
    Client         *client;
    char           *url;        /* the page whose title is awaited */
    char           *ts;
    char           *last;       /* the last page listed, without fragment */
    gboolean       loading;     /* between a load's start and its commit */
    guint          spa_source;  /* the check that an address change stood */
    char           *spa_uri;
    char           *spa_title;  /* the title when the address changed */
} PageState;

static GList *archives;
static GHashTable *page_states;     /* Client * -> PageState * */
static GThread *recovery;

static char *now_iso8601(void)
{
    GDateTime *now = g_date_time_new_now_utc();
    char *text     = g_date_time_format_iso8601(now);
    g_date_time_unref(now);
    return text;
}

static char *without_fragment(const char *uri)
{
    const char *hash = strchr(uri, '#');
    return hash ? g_strndup(uri, hash - uri) : g_strdup(uri);
}

/* A name made fit for a file name: no slashes, no control characters, no
 * leading dot. */
static char *file_name(const char *name)
{
    GString *out = g_string_new(NULL);
    for (const char *p = name; *p; p++) {
        guchar ch = (guchar)*p;
        g_string_append_c(out, (ch < 0x20 || ch == '/' || ch == '\\' || ch == 0x7f) ? '_' : *p);
    }
    char *stripped = g_strstrip(g_string_free(out, FALSE));
    while (*stripped == '.') {
        memmove(stripped, stripped + 1, strlen(stripped));
    }
    if (!*stripped) {
        g_free(stripped);
        return g_strdup("Archive");
    }
    return stripped;
}

/* dir/base+suffix, or with " (2)", " (3)"... added, for a path not taken. */
static char *unique_path(const char *dir, const char *base, const char *suffix)
{
    char *path = g_strdup_printf("%s/%s%s", dir, base, suffix);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path);
        path = g_strdup_printf("%s/%s (%d)%s", dir, base, n, suffix);
    }
    return path;
}

static char *archives_dir(Client *c)
{
    const char *setting = c ? GET_CHAR(c, "archive-dir") : NULL;
    char *dir;
    if (setting && *setting) {
        dir = g_str_has_prefix(setting, "~/")
            ? g_build_filename(g_get_home_dir(), setting + 2, NULL) : g_strdup(setting);
    } else {
        char *data = util_get_data_dir();
        dir = g_build_filename(data, "archives", NULL);
        g_free(data);
    }
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

/* Holds a capture directory's lock, so that no other vimb takes it for one
 * left behind. -1 if another holds it. */
static int lock_capture(const char *dir)
{
    char *path = g_build_filename(dir, LOCK_FILE, NULL);
    int fd     = g_open(path, O_RDWR | O_CREAT, 0600);
    g_free(path);
    if (fd < 0) {
        return -1;
    }
    struct flock lock = {0};
    lock.l_type   = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Removes what packaging leaves of a capture directory, and the directory. */
static void remove_capture_dir(const char *dir)
{
    const char *leftovers[] = {LOCK_FILE, NAME_FILE};
    for (gsize i = 0; i < G_N_ELEMENTS(leftovers); i++) {
        char *path = g_build_filename(dir, leftovers[i], NULL);
        g_unlink(path);
        g_free(path);
    }
    g_rmdir(dir);
}

static char *read_capture_name(const char *dir)
{
    char *path = g_build_filename(dir, NAME_FILE, NULL);
    char *name = NULL;
    if (g_file_get_contents(path, &name, NULL, NULL)) {
        g_strstrip(name);
    }
    g_free(path);
    return name;
}

/* ---- the status bar ---------------------------------------------------- */

static void update_status(struct Archive *a)
{
    const char *label = NULL;
    switch (a->state) {
        case ARCHIVE_RECORDING: label = "REC";    break;
        case ARCHIVE_PAUSED:    label = "PAUSED"; break;
        case ARCHIVE_SAVING:    label = "SAVING"; break;
        case ARCHIVE_DONE:      label = NULL;     break;
    }
    g_clear_pointer(&a->status, g_free);
    if (label) {
        gboolean long_name = g_utf8_strlen(a->name, -1) > STATUS_NAME_MAX;
        char *shown = long_name ? g_utf8_substring(a->name, 0, STATUS_NAME_MAX - 1)
            : g_strdup(a->name);
        a->status = g_strdup_printf("[%s %s%s]", label, shown, long_name ? "…" : "");
        g_free(shown);
    }
    for (Client *c = vb.clients; c; c = c->next) {
        if (c->archive == a) {
            vb_statusbar_update(c);
        }
    }
}

const char *archiving_status(Client *c)
{
    return c->archive ? c->archive->status : NULL;
}

gboolean archiving_tls_checked(Client *c)
{
    return c->archive && c->archive->verifies;
}

/* ---- the page list ----------------------------------------------------- */

static void page_flush(PageState *ps, const char *title)
{
    if (ps->url && ps->archive->pages) {
        char *line = archive_page_line(ps->url, ps->ts, title);
        fprintf(ps->archive->pages, "%s\n", line);
        fflush(ps->archive->pages);
        g_free(line);
    }
    g_clear_pointer(&ps->url, g_free);
    g_clear_pointer(&ps->ts, g_free);
}

static PageState *page_state(Client *c)
{
    return page_states ? g_hash_table_lookup(page_states, c) : NULL;
}

static gboolean listing(Client *c, PageState *ps)
{
    return ps && c->archive && c->archive->state == ARCHIVE_RECORDING;
}

/* A page is awaited until its title comes, and listed then -- or untitled,
 * if another page comes first. */
static void page_begin(PageState *ps, const char *uri)
{
    page_flush(ps, NULL);
    if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")) {
        return;
    }
    ps->url = g_strdup(uri);
    ps->ts  = now_iso8601();
    g_free(ps->last);
    ps->last = without_fragment(uri);
}

static void spa_cancel(PageState *ps)
{
    if (ps->spa_source) {
        g_source_remove(ps->spa_source);
        ps->spa_source = 0;
    }
    g_clear_pointer(&ps->spa_uri, g_free);
    g_clear_pointer(&ps->spa_title, g_free);
}

void archiving_load_changed(Client *c, WebKitLoadEvent event)
{
    PageState *ps = page_state(c);
    if (!ps) {
        return;
    }
    if (event == WEBKIT_LOAD_STARTED) {
        spa_cancel(ps);         /* it was this load, not a page of the app */
        ps->loading = TRUE;
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        ps->loading = FALSE;
        const char *uri = webkit_web_view_get_uri(c->webview);
        if (listing(c, ps) && uri) {
            page_begin(ps, uri);
        }
    }
}

void archiving_title_changed(Client *c)
{
    PageState *ps   = page_state(c);
    const char *title = webkit_web_view_get_title(c->webview);
    if (listing(c, ps) && ps->url && title && *title) {
        page_flush(ps, title);
    }
}

static gboolean spa_check(gpointer data)
{
    PageState *ps   = data;
    Client *c       = ps->client;
    const char *uri = webkit_web_view_get_uri(c->webview);

    ps->spa_source = 0;
    if (listing(c, ps) && !ps->loading && !webkit_web_view_is_loading(c->webview) && uri
            && !g_strcmp0(uri, ps->spa_uri)) {
        page_begin(ps, uri);
        /* An app that retitled its page along with the address has done so
         * already: that title is this page's. */
        const char *title = webkit_web_view_get_title(c->webview);
        if (title && *title && g_strcmp0(title, ps->spa_title)) {
            page_flush(ps, title);
        }
    }
    g_clear_pointer(&ps->spa_uri, g_free);
    g_clear_pointer(&ps->spa_title, g_free);
    return G_SOURCE_REMOVE;
}

/* A page that changes its address without loading another -- a single-page
 * app's next "page" -- is a page of its own, unless only the fragment
 * changed; once the change has stood a moment (see SPA_SETTLE_MS). */
void archiving_uri_changed(Client *c)
{
    PageState *ps   = page_state(c);
    const char *uri = webkit_web_view_get_uri(c->webview);
    if (!listing(c, ps) || ps->loading || webkit_web_view_is_loading(c->webview) || !uri
            || !ps->last) {
        return;
    }
    char *base = without_fragment(uri);
    if (strcmp(base, ps->last)) {
        spa_cancel(ps);
        ps->spa_uri    = g_strdup(uri);
        ps->spa_title  = g_strdup(webkit_web_view_get_title(c->webview));
        ps->spa_source = g_timeout_add(SPA_SETTLE_MS, spa_check, ps);
    }
    g_free(base);
}

/* ---- packaging --------------------------------------------------------- */

static void archive_free(struct Archive *a)
{
    archives = g_list_remove(archives, a);
    if (a->proxy) {
        archive_proxy_stop(a->proxy, NULL);
    }
    g_clear_object(&a->session);
    if (a->pages) {
        fclose(a->pages);
    }
    if (a->lock_fd >= 0) {
        close(a->lock_fd);
    }
    g_clear_error(&a->error);
    g_free(a->saved);
    g_free(a->status);
    g_free(a->warc_name);
    g_free(a->dir);
    g_free(a->archives_dir);
    g_free(a->name);
    g_free(a);
}

/* Packages a capture directory as <archives_dir>/<name>.wacz, and removes
 * the directory if that worked. Any thread. */
static char *package_capture(const char *dir, const char *archives_dir, const char *name,
        GError **error)
{
    char *base = file_name(name);
    char *wacz = unique_path(archives_dir, base, ".wacz");
    g_free(base);
    if (!archive_wacz_package(dir, wacz, "vimb " VERSION, name, TRUE, error)) {
        g_free(wacz);
        return NULL;
    }
    return wacz;
}

static gboolean package_done(gpointer data)
{
    struct Archive *a = data;
    Client *c         = vb_get_current_client();

    g_thread_join(a->packager);
    a->packager    = NULL;
    a->done_source = 0;
    a->state       = ARCHIVE_DONE;
    if (a->saved) {
        remove_capture_dir(a->dir);
        if (c) {
            vb_echo(c, MSG_NORMAL, FALSE, "Archive saved: %s", a->saved);
        }
    } else if (c) {
        vb_echo(c, MSG_ERROR, FALSE, "Archive '%s' could not be packaged: %s (its files are in %s)",
                a->name, a->error ? a->error->message : "unknown error", a->dir);
    }
    update_status(a);
    if (!a->tabs) {
        archive_free(a);
    }
    return G_SOURCE_REMOVE;
}

static gpointer package_run(gpointer data)
{
    struct Archive *a = data;
    a->saved       = package_capture(a->dir, a->archives_dir, a->name, &a->error);
    a->done_source = g_idle_add(package_done, a);
    return NULL;
}

/* Lists the pages still awaiting their titles, untitled, and stops the
 * listing. */
static void close_page_list(struct Archive *a)
{
    if (page_states) {
        GHashTableIter iter;
        gpointer ps;
        g_hash_table_iter_init(&iter, page_states);
        while (g_hash_table_iter_next(&iter, NULL, &ps)) {
            if (((PageState *)ps)->archive == a) {
                page_flush(ps, NULL);
            }
        }
    }
    if (a->pages) {
        fclose(a->pages);
        a->pages = NULL;
    }
}

/* Ends recording and packages the capture in the background. */
static void archive_stop(struct Archive *a)
{
    GError *error = NULL;
    if (a->state != ARCHIVE_RECORDING && a->state != ARCHIVE_PAUSED) {
        return;
    }
    close_page_list(a);
    if (!archive_proxy_finish_recording(a->proxy, &error)) {
        g_warning("archive: %s", error->message);
        g_clear_error(&error);
    }
    a->state    = ARCHIVE_SAVING;
    a->packager = g_thread_new("archive-package", package_run, a);
    update_status(a);
}

/* ---- starting ---------------------------------------------------------- */

static void on_barrier(GObject *manager, GAsyncResult *result, gpointer call)
{
    GList *data = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(manager),
            result, NULL);
    g_list_free_full(data, (GDestroyNotify)webkit_website_data_unref);
    archive_proxy_allow_call_done(call);
}

/* The proxy is about to hand the browser a site certificate of its own
 * making: the session is told to accept it, and the proxy is told when that
 * has reached WebKit's network process -- which a fetch from the session,
 * sent after it, answering says. */
static void on_allow_certificate(const char *host, GTlsCertificate *cert,
        ArchiveProxyAllowCall *call, gpointer data)
{
    struct Archive *a = data;
    webkit_network_session_allow_tls_certificate_for_host(a->session, cert, host);
    webkit_website_data_manager_fetch(webkit_network_session_get_website_data_manager(a->session),
            WEBKIT_WEBSITE_DATA_COOKIES, NULL, on_barrier, call);
}

static void apply_cookie_policy(WebKitNetworkSession *session, const char *policy)
{
    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(session);
    if (!g_strcmp0(policy, "origin")) {
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    } else if (!g_strcmp0(policy, "never")) {
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NEVER);
    } else {
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }
}

static struct Archive *archive_start(Client *c, const char *name, GError **error)
{
    struct Archive *a = g_new0(struct Archive, 1);
    a->lock_fd = -1;
    a->state   = ARCHIVE_RECORDING;
    if (name && *name) {
        a->name = g_strdup(name);
    } else {
        GDateTime *now = g_date_time_new_now_local();
        a->name = g_date_time_format(now, "Archive %Y-%m-%d %H-%M-%S");
        g_date_time_unref(now);
    }
    a->archives_dir = archives_dir(c);
    char *base      = file_name(a->name);
    a->dir          = unique_path(a->archives_dir, base, CAPTURE_SUFFIX);
    a->warc_name    = g_strdup("data.warc.gz");     /* as Webrecorder names it */
    g_free(base);

    if (g_mkdir(a->dir, 0700) < 0 || (a->lock_fd = lock_capture(a->dir)) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not make %s: %s", a->dir, g_strerror(errno));
        goto fail;
    }
    char *name_file = g_build_filename(a->dir, NAME_FILE, NULL);
    g_file_set_contents(name_file, a->name, -1, NULL);
    g_free(name_file);

    char *pages_file = g_build_filename(a->dir, ARCHIVE_PAGES_FILE, NULL);
    a->pages = g_fopen(pages_file, "w");
    g_free(pages_file);
    if (a->pages) {
        fprintf(a->pages, "%s\n", ARCHIVE_PAGES_HEADER);
        fflush(a->pages);
    }

    char *data_dir = util_get_data_dir();
    char *ca_dir   = g_build_filename(data_dir, "archive-ca", NULL);
    char *warc     = g_build_filename(a->dir, a->warc_name, NULL);
    a->verifies    = GET_BOOL(c, "strict-ssl");
    ArchiveProxyOptions options = {
        .ca_dir            = ca_dir,
        .warc_path         = warc,
        .tmp_dir           = a->dir,
        .software          = "vimb " VERSION,
        .verify_upstream   = a->verifies,
        .allow_internal    = GET_BOOL(c, "archive-allow-internal"),
        .check_peer_user   = TRUE,
        .allow_certificate = on_allow_certificate,
        .allow_data        = a,
    };
    a->proxy = archive_proxy_start(&options, error);
    g_free(warc);
    g_free(ca_dir);
    g_free(data_dir);
    if (!a->proxy) {
        goto fail;
    }

    /* Starting from nothing: an ephemeral session, through the proxy. TLS
     * errors fail -- the proxy's certificates are accepted host by host, as
     * it hands them out, and nothing else is. */
    a->session = webkit_network_session_new_ephemeral();
    char *proxy_uri = g_strdup_printf("http://127.0.0.1:%u", archive_proxy_get_port(a->proxy));
    WebKitNetworkProxySettings *settings = webkit_network_proxy_settings_new(proxy_uri, NULL);
    webkit_network_session_set_proxy_settings(a->session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM,
            settings);
    webkit_network_proxy_settings_free(settings);
    g_free(proxy_uri);
    webkit_network_session_set_tls_errors_policy(a->session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    webkit_network_session_set_itp_enabled(a->session,
            GET_BOOL(c, "intelligent-tracking-prevention"));
    apply_cookie_policy(a->session, GET_CHAR(c, "cookie-accept"));
    vb_session_setup(a->session);

    update_status(a);
    archives = g_list_prepend(archives, a);
    return a;

fail:
    if (a->pages) {
        fclose(a->pages);
        a->pages = NULL;
    }
    if (a->lock_fd >= 0) {
        char *pages = g_build_filename(a->dir, ARCHIVE_PAGES_FILE, NULL);
        g_unlink(pages);
        g_free(pages);
        remove_capture_dir(a->dir);
    }
    archives = g_list_prepend(archives, a);     /* archive_free() removes it */
    archive_free(a);
    return NULL;
}

WebKitNetworkSession *archiving_session(struct Archive *archive)
{
    return archive->session;
}

void archiving_tab_added(Client *c)
{
    if (!page_states) {
        page_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    PageState *ps = g_new0(PageState, 1);
    ps->archive   = c->archive;
    ps->client    = c;
    g_hash_table_insert(page_states, c, ps);
    c->archive->tabs++;
}

void archiving_tab_closed(Client *c)
{
    struct Archive *a = c->archive;
    PageState *ps     = page_state(c);
    if (ps) {
        if (listing(c, ps)) {
            page_flush(ps, NULL);
        }
        spa_cancel(ps);
        g_hash_table_remove(page_states, c);
        g_free(ps->url);
        g_free(ps->ts);
        g_free(ps->last);
        g_free(ps);
    }
    c->archive = NULL;
    if (!a || --a->tabs) {
        return;
    }
    /* The last tab gone: what was recording is packaged, and what is done
     * goes, once it has. */
    if (a->state == ARCHIVE_RECORDING || a->state == ARCHIVE_PAUSED) {
        archive_stop(a);
    } else if (a->state == ARCHIVE_DONE) {
        archive_free(a);
    }
}

/* ---- the command ------------------------------------------------------- */

/* The archive a command is about: the current tab's, or the one there is. */
static struct Archive *target(Client *c)
{
    if (c->archive) {
        return c->archive;
    }
    struct Archive *only = NULL;
    for (GList *l = archives; l; l = l->next) {
        struct Archive *a = l->data;
        if (a->state == ARCHIVE_RECORDING || a->state == ARCHIVE_PAUSED) {
            if (only) {
                return NULL;        /* more than one: which? */
            }
            only = a;
        }
    }
    return only;
}

static VbCmdResult start(Client *c, const char *name)
{
    GError *error     = NULL;
    struct Archive *a = archive_start(c, name, &error);
    if (!a) {
        vb_echo(c, MSG_ERROR, TRUE, "Could not start archiving: %s", error->message);
        g_error_free(error);
        return CMD_ERROR;
    }
    const char *uri = c->state.uri && *c->state.uri ? c->state.uri : GET_CHAR(c, "home-page");
    Client *tab     = vb_tab_new_archive(a, uri);
    if (tab) {
        vb_echo(tab, MSG_NORMAL, FALSE, "Archiving as '%s'", a->name);
    }
    return CMD_SUCCESS;
}

VbCmdResult archiving_command(Client *c, const char *args)
{
    char *text = g_strstrip(g_strdup(args ? args : ""));
    char *rest = text + strcspn(text, " \t");
    char *word = g_strndup(text, rest - text);
    /* Skipped over, not stripped: g_strstrip() would shift text itself. */
    rest += strspn(rest, " \t");
    VbCmdResult result = CMD_SUCCESS;

    if (!*word || !strcmp(word, "start")) {
        result = start(c, rest);
    } else if (!strcmp(word, "pause") || !strcmp(word, "resume") || !strcmp(word, "stop")
            || !strcmp(word, "name") || !strcmp(word, "status")) {
        struct Archive *a = target(c);
        if (!a) {
            vb_echo(c, MSG_ERROR, TRUE, archives ? "Which archive? Use this from one of its tabs"
                    : "Nothing is being archived");
            result = CMD_ERROR;
        } else if (!strcmp(word, "status")) {
            static const char *states[] = {"recording", "paused", "being saved", "done"};
            vb_echo(c, MSG_NORMAL, FALSE, "'%s': %s, %u responses%s%s", a->name, states[a->state],
                    a->proxy ? archive_proxy_get_recorded(a->proxy) : 0,
                    a->saved ? ", saved as " : "", a->saved ? a->saved : "");
        } else if (a->state != ARCHIVE_RECORDING && a->state != ARCHIVE_PAUSED) {
            vb_echo(c, MSG_ERROR, TRUE, "'%s' is no longer recording", a->name);
            result = CMD_ERROR;
        } else if (!strcmp(word, "pause") || !strcmp(word, "resume")) {
            gboolean pause = !strcmp(word, "pause");
            if (pause && a->state == ARCHIVE_RECORDING) {
                /* The page awaiting its title is listed: it was archived. */
                close_page_list(a);
                char *pages_file = g_build_filename(a->dir, ARCHIVE_PAGES_FILE, NULL);
                a->pages = g_fopen(pages_file, "a");
                g_free(pages_file);
            }
            archive_proxy_set_paused(a->proxy, pause);
            a->state = pause ? ARCHIVE_PAUSED : ARCHIVE_RECORDING;
            update_status(a);
        } else if (!strcmp(word, "stop")) {
            archive_stop(a);
            vb_echo(c, MSG_NORMAL, FALSE, "Saving '%s'...", a->name);
        } else if (!*rest) {
            vb_echo(c, MSG_NORMAL, FALSE, "Archive name: %s", a->name);
        } else {
            g_free(a->name);
            a->name = g_strdup(rest);
            char *name_file = g_build_filename(a->dir, NAME_FILE, NULL);
            g_file_set_contents(name_file, a->name, -1, NULL);
            g_free(name_file);
            update_status(a);
        }
    } else {
        /* :archive NAME */
        result = start(c, text);
    }
    g_free(word);
    g_free(text);
    return result;
}

/* ---- leftovers, and the end -------------------------------------------- */

typedef struct {
    char      *archives_dir;
    GPtrArray *messages;
} Recovery;

static gboolean recovery_done(gpointer data)
{
    Recovery *r = data;
    Client *c   = vb_get_current_client();
    if (recovery) {
        g_thread_join(recovery);
        recovery = NULL;
    }
    for (guint i = 0; i < r->messages->len && c; i++) {
        vb_echo(c, MSG_NORMAL, FALSE, "%s", (char *)g_ptr_array_index(r->messages, i));
    }
    g_ptr_array_unref(r->messages);
    g_free(r->archives_dir);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer recovery_run(gpointer data)
{
    Recovery *r = data;
    GDir *dir   = g_dir_open(r->archives_dir, 0, NULL);
    const char *entry;
    while (dir && (entry = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(entry, CAPTURE_SUFFIX)) {
            continue;
        }
        char *path = g_build_filename(r->archives_dir, entry, NULL);
        int lock   = lock_capture(path);
        if (lock >= 0) {
            char *name = read_capture_name(path);
            if (!name) {
                name = g_strndup(entry, strlen(entry) - strlen(CAPTURE_SUFFIX));
            }
            GError *error = NULL;
            char *wacz    = package_capture(path, r->archives_dir, name, &error);
            close(lock);
            if (wacz) {
                remove_capture_dir(path);
                g_ptr_array_add(r->messages, g_strdup_printf("Recovered archive: %s", wacz));
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
                g_ptr_array_add(r->messages, g_strdup_printf("Could not recover %s: %s", path,
                            error->message));
            } else {
                /* Nothing was recorded before it ended. */
                char *pages = g_build_filename(path, ARCHIVE_PAGES_FILE, NULL);
                g_unlink(pages);
                g_free(pages);
                remove_capture_dir(path);
            }
            g_clear_error(&error);
            g_free(wacz);
            g_free(name);
        }
        g_free(path);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_idle_add(recovery_done, r);
    return NULL;
}

void archiving_recover(Client *c)
{
    if (recovery) {
        return;
    }
    Recovery *r     = g_new0(Recovery, 1);
    r->archives_dir = archives_dir(c);
    r->messages     = g_ptr_array_new_with_free_func(g_free);
    recovery        = g_thread_new("archive-recover", recovery_run, r);
}

void archiving_shutdown(void)
{
    if (recovery) {
        g_thread_join(recovery);
        recovery = NULL;
    }
    while (archives) {
        struct Archive *a = archives->data;
        if (a->state == ARCHIVE_RECORDING || a->state == ARCHIVE_PAUSED) {
            close_page_list(a);
            archive_proxy_finish_recording(a->proxy, NULL);
            a->state    = ARCHIVE_SAVING;
            a->packager = g_thread_new("archive-package", package_run, a);
        }
        if (a->packager) {
            g_thread_join(a->packager);
            a->packager = NULL;
            /* Reported here instead, with no main loop left to run it. */
            if (a->done_source) {
                g_source_remove(a->done_source);
                a->done_source = 0;
            }
            if (a->saved) {
                remove_capture_dir(a->dir);
                fprintf(stderr, "vimb: archive saved: %s\n", a->saved);
            } else {
                fprintf(stderr, "vimb: archive '%s' could not be packaged: %s (its files are in %s)\n",
                        a->name, a->error ? a->error->message : "unknown error", a->dir);
            }
        }
        archive_free(a);
    }
    if (page_states) {
        GHashTableIter iter;
        gpointer ps;
        g_hash_table_iter_init(&iter, page_states);
        while (g_hash_table_iter_next(&iter, NULL, &ps)) {
            spa_cancel(ps);
            g_free(((PageState *)ps)->url);
            g_free(((PageState *)ps)->ts);
            g_free(((PageState *)ps)->last);
            g_free(ps);
        }
        g_clear_pointer(&page_states, g_hash_table_unref);
    }
}
