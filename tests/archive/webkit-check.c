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

/* Loads pages in a WebKit view whose network session goes through the
 * archiving proxy, in-process, the way vimb will: TLS errors fail, and each
 * site's certificate is allowed only as the proxy first hands it out. Prints
 * every resource load, then the totals; exits 1 on any TLS failure.
 *
 *   webkit-check --dir DIR [--seconds N] [--allow-internal] [--no-verify]
 *                [--wacz OUT.wacz] URL...
 *
 * With --wacz it also does what vimb does at the end of a capture: keeps a
 * page list as pages load, and packages the capture as a WACZ.
 *
 * It needs a display; under Xvfb, set GDK_BACKEND=x11. */

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <webkit/webkit.h>

#include "../../src/archive/proxy.h"
#include "../../src/archive/wacz.h"

static char *opt_dir;
static int opt_seconds        = 20;
static gboolean opt_internal  = FALSE;
static gboolean opt_no_verify = FALSE;
static char *opt_wacz;
static GString *pages;          /* the page list, as pages load */
static char *page_url;          /* the page being loaded, until its title comes */
static char *page_ts;

static GMainLoop *loop;
static WebKitNetworkSession *session;
static gint64 t0;
static int n_ok, n_failed, n_tls, n_allowed;
static char **urls;
static int next_url;
static WebKitWebView *view;

static double now(void)
{
    return (g_get_monotonic_time() - t0) / 1e6;
}

static void on_barrier(GObject *manager, GAsyncResult *result, gpointer call)
{
    GList *data = webkit_website_data_manager_fetch_finish(
            WEBKIT_WEBSITE_DATA_MANAGER(manager), result, NULL);
    g_list_free_full(data, (GDestroyNotify)webkit_website_data_unref);
    archive_proxy_allow_call_done(call);
}

/* Called by the proxy, on this thread, before a site's certificate goes out.
 * The allowance is a message to WebKit's network process; a fetch from the
 * same session goes there after it, and its answer means it has arrived. */
static void allow_certificate(const char *host, GTlsCertificate *cert,
        ArchiveProxyAllowCall *call, gpointer data)
{
    webkit_network_session_allow_tls_certificate_for_host(session, cert, host);
    webkit_website_data_manager_fetch(webkit_network_session_get_website_data_manager(session),
            WEBKIT_WEBSITE_DATA_COOKIES, NULL, on_barrier, call);
    n_allowed++;
    printf("%6.2f ALLOW %s\n", now(), host);
}

static void on_res_finished(WebKitWebResource *r, gpointer d)
{
    WebKitURIResponse *resp = webkit_web_resource_get_response(r);
    n_ok++;
    printf("%6.2f RES %u %s\n", now(), resp ? webkit_uri_response_get_status_code(resp) : 0,
            webkit_web_resource_get_uri(r));
}

static void on_res_failed(WebKitWebResource *r, GError *e, gpointer d)
{
    n_failed++;
    printf("%6.2f RES FAIL [%s] %s\n", now(), e->message, webkit_web_resource_get_uri(r));
}

static void on_res_tls(WebKitWebResource *r, GTlsCertificate *c, GTlsCertificateFlags f,
        gpointer d)
{
    n_tls++;
    printf("%6.2f RES TLS [flags %#x] %s\n", now(), f, webkit_web_resource_get_uri(r));
}

static void on_resource(WebKitWebView *v, WebKitWebResource *r, WebKitURIRequest *q, gpointer d)
{
    g_signal_connect(r, "finished", G_CALLBACK(on_res_finished), NULL);
    g_signal_connect(r, "failed", G_CALLBACK(on_res_failed), NULL);
    g_signal_connect(r, "failed-with-tls-errors", G_CALLBACK(on_res_tls), NULL);
}

static gboolean load_next(gpointer data)
{
    if (urls[next_url]) {
        printf("%6.2f OPEN %s\n", now(), urls[next_url]);
        webkit_web_view_load_uri(view, urls[next_url++]);
        return G_SOURCE_CONTINUE;
    }
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

/* A page goes into the list with its title, once that comes -- or untitled,
 * when the next page starts first. */
static void flush_page(const char *title)
{
    if (page_url) {
        char *line = archive_page_line(page_url, page_ts, title);
        g_string_append_printf(pages, "%s\n", line);
        g_free(line);
        g_clear_pointer(&page_url, g_free);
        g_clear_pointer(&page_ts, g_free);
    }
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent event, gpointer d)
{
    if (event == WEBKIT_LOAD_COMMITTED) {
        flush_page(NULL);
        GDateTime *now_utc = g_date_time_new_now_utc();
        page_url = g_strdup(webkit_web_view_get_uri(v));
        page_ts  = g_date_time_format_iso8601(now_utc);
        g_date_time_unref(now_utc);
    }
}

static void on_title(WebKitWebView *v, GParamSpec *p, gpointer d)
{
    const char *title = webkit_web_view_get_title(v);
    printf("%6.2f TITLE %s\n", now(), title);
    if (title && *title) {
        flush_page(title);
    }
}

int main(int argc, char **argv)
{
    GOptionEntry entries[] = {
        {"dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_dir, "For the WARC and authority", "DIR"},
        {"seconds", 0, 0, G_OPTION_ARG_INT, &opt_seconds, "Time given each page", "N"},
        {"allow-internal", 0, 0, G_OPTION_ARG_NONE, &opt_internal, "Allow localhost", NULL},
        {"no-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_verify, "Archive bad certificates", NULL},
        {"wacz", 0, 0, G_OPTION_ARG_FILENAME, &opt_wacz, "Package the capture here", "FILE"},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &urls, NULL, "URL..."},
        {NULL}
    };
    GError *error = NULL;
    GOptionContext *context = g_option_context_new("URL...");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error) || !opt_dir || !urls) {
        fprintf(stderr, "%s\n", error ? error->message : "--dir and a URL are needed");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    gtk_init();
    t0 = g_get_monotonic_time();
    g_mkdir_with_parents(opt_dir, 0700);

    /* The capture's loose files, as vimb keeps them until it packages them. */
    char *capture = g_build_filename(opt_dir, "capture", NULL);
    char *warc    = g_build_filename(capture, "webkit-check.warc.gz", NULL);
    g_mkdir_with_parents(capture, 0700);
    g_remove(warc);
    pages = g_string_new(ARCHIVE_PAGES_HEADER "\n");
    ArchiveProxyOptions options = {
        .ca_dir            = opt_dir,
        .warc_path         = warc,
        .tmp_dir           = opt_dir,
        .software          = "vimb webkit-check",
        .verify_upstream   = !opt_no_verify,
        .allow_internal    = opt_internal,
        .check_peer_user   = TRUE,
        .allow_certificate = allow_certificate,
    };
    ArchiveProxy *proxy = archive_proxy_start(&options, &error);
    if (!proxy) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }

    session = webkit_network_session_new_ephemeral();
    char *proxy_uri = g_strdup_printf("http://127.0.0.1:%u", archive_proxy_get_port(proxy));
    WebKitNetworkProxySettings *settings = webkit_network_proxy_settings_new(proxy_uri, NULL);
    webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM,
            settings);
    webkit_network_proxy_settings_free(settings);
    webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    printf("%6.2f PROXY %s\n", now(), proxy_uri);

    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL));
    g_signal_connect(view, "resource-load-started", G_CALLBACK(on_resource), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    GtkWidget *window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 900);
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(view));
    gtk_window_present(GTK_WINDOW(window));

    loop = g_main_loop_new(NULL, FALSE);
    load_next(NULL);
    g_timeout_add_seconds(opt_seconds, load_next, NULL);
    g_main_loop_run(loop);

    guint recorded = archive_proxy_get_recorded(proxy);
    gboolean stopped = archive_proxy_stop(proxy, &error);
    printf("%6.2f DONE ok=%d failed=%d tls=%d allowed=%d recorded>=%u warc=%s%s%s\n", now(),
            n_ok, n_failed, n_tls, n_allowed, recorded, warc,
            stopped ? "" : " stop-error=", stopped ? "" : error->message);
    gboolean packaged = TRUE;
    if (opt_wacz && stopped) {
        flush_page(NULL);
        char *list = g_build_filename(capture, ARCHIVE_PAGES_FILE, NULL);
        g_file_set_contents(list, pages->str, (gssize)pages->len, NULL);
        g_free(list);
        packaged = archive_wacz_package(capture, opt_wacz, "vimb webkit-check", NULL, TRUE, &error);
        printf("%6.2f %s %s\n", now(), packaged ? "PACKAGED" : "PACKAGE-FAILED",
                packaged ? opt_wacz : error->message);
    }
    return (n_tls || !stopped || !packaged) ? 1 : 0;
}
