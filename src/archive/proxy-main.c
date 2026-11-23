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

/* vimb-archive-proxy: the archiving proxy on its own, for testing it with
 * curl and other clients. Not installed.
 *
 *   vimb-archive-proxy --dir DIR [--no-verify] [--allow-internal] ...
 *   curl --proxy http://127.0.0.1:PORT --cacert DIR/vimb-archive-ca.pem URL
 *
 * Interrupt it to finish the WARC file. */

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>

#include "proxy.h"

static gboolean on_signal(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    char *dir               = NULL;
    gboolean no_verify      = FALSE;
    gboolean allow_internal = FALSE;
    gboolean no_record      = FALSE;
    gboolean any_user       = FALSE;
    gint64 max_record_size  = 0;
    GError *error           = NULL;
    GOptionEntry entries[]  = {
        {"dir", 'd', 0, G_OPTION_ARG_FILENAME, &dir,
            "Where the WARC file and the authority go", "DIR"},
        {"no-verify", 0, 0, G_OPTION_ARG_NONE, &no_verify,
            "Archive sites whose certificates fail verification", NULL},
        {"allow-internal", 0, 0, G_OPTION_ARG_NONE, &allow_internal,
            "Let requests reach loopback and private addresses", NULL},
        {"no-record", 0, 0, G_OPTION_ARG_NONE, &no_record,
            "Pass traffic on without recording it", NULL},
        {"any-user", 0, 0, G_OPTION_ARG_NONE, &any_user,
            "Serve other users' processes too", NULL},
        {"max-record-size", 0, 0, G_OPTION_ARG_INT64, &max_record_size,
            "Longest response recorded whole", "BYTES"},
        {NULL}
    };
    GOptionContext *context = g_option_context_new("- vimb's archiving proxy");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error) || !dir) {
        fprintf(stderr, "%s\n", error ? error->message : "--dir is needed");
        return 2;
    }
    g_option_context_free(context);
    g_mkdir_with_parents(dir, 0700);

    GDateTime *now  = g_date_time_new_now_utc();
    char *stamp     = g_date_time_format(now, "%Y%m%d%H%M%S");
    char *name      = g_strdup_printf("vimb-%s.warc.gz", stamp);
    char *warc_path = g_build_filename(dir, name, NULL);
    g_date_time_unref(now);
    g_free(stamp);
    g_free(name);

    ArchiveProxyOptions options = {
        .ca_dir          = dir,
        .warc_path       = no_record ? NULL : warc_path,
        .tmp_dir         = dir,
        .software        = "vimb-archive-proxy",
        .verify_upstream = !no_verify,
        .allow_internal  = allow_internal,
        .check_peer_user = !any_user,
        .max_record_size = max_record_size > 0 ? (guint64)max_record_size : 0,
    };
    ArchiveProxy *proxy = archive_proxy_start(&options, &error);
    if (!proxy) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    printf("listening 127.0.0.1:%u\n", archive_proxy_get_port(proxy));
    printf("ca %s\n", archive_proxy_get_ca_file(proxy));
    if (!no_record) {
        printf("warc %s\n", warc_path);
    }
    fflush(stdout);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal, loop);
    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_main_loop_run(loop);

    guint recorded = archive_proxy_get_recorded(proxy);
    if (!archive_proxy_stop(proxy, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    printf("stopped; %u or more exchanges recorded\n", recorded);
    g_main_loop_unref(loop);
    g_free(warc_path);
    g_free(dir);
    return 0;
}
