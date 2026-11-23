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

#ifndef _ARCHIVE_PROXY_H
#define _ARCHIVE_PROXY_H

#include <gio/gio.h>

#include "replay.h"

/* The archiving proxy: an HTTP proxy on the loopback address that an
 * archiving tab's network session is pointed at, and that writes what passes
 * through it to a WARC file.
 *
 * It reads HTTPS by terminating the browser's TLS itself, with a certificate
 * for each site signed by its own authority (see ca.h), and fetches from the
 * site over a TLS connection of its own. What it records is what the site
 * sent, byte for byte -- status line, headers as they came, chunked coding --
 * because it speaks HTTP/1.1 to the site itself rather than through a library
 * that would hand back only what it parsed.
 *
 * It runs on threads of its own: one accepting connections, one per browser
 * connection, one writing the WARC. */
typedef struct ArchiveProxy ArchiveProxy;

/* Called, on the owner's main context, before the browser is first given the
 * certificate for host: the time to tell its network session to accept it.
 * The proxy holds the browser's connection until the owner calls
 * archive_proxy_allow_call_done() on call.
 *
 * Telling WebKit is a message to its network process, so returning is not
 * enough: the browser's TLS handshake could reach that process before the
 * message does. The owner should first make a round trip there -- WebKit
 * handles its messages in order -- and then call done. */
typedef struct ArchiveProxyAllowCall ArchiveProxyAllowCall;
typedef void (*ArchiveProxyAllowFunc)(const char *host, GTlsCertificate *certificate,
        ArchiveProxyAllowCall *call, gpointer user_data);

/* Says the browser now accepts the certificate. Exactly once per call; any
 * thread; safe even after the proxy has stopped. */
void archive_proxy_allow_call_done(ArchiveProxyAllowCall *call);

typedef struct {
    const char            *ca_dir;          /* where the authority is kept */
    const char            *warc_path;       /* NULL: pass traffic on unrecorded */
    const char            *tmp_dir;         /* for bodies too big for memory */
    const char            *software;        /* for the warcinfo record */
    gboolean              verify_upstream;  /* refuse sites whose certificates fail */
    gboolean              allow_internal;   /* let pages reach internal addresses */
    gboolean              check_peer_user;  /* serve only this user's processes */
    guint64               max_record_size;  /* longest response kept whole; 0: 4 GiB */
    ArchiveProxyAllowFunc allow_certificate;
    gpointer              allow_data;
    GMainContext          *owner_context;   /* NULL: the default main context */
    /* Replay: every request is answered from this archive (see replay.h), and
     * nothing is fetched or recorded -- the network isn't touched at all.
     * It must last as long as the proxy. */
    ArchiveReplay         *replay;
} ArchiveProxyOptions;

ArchiveProxy *archive_proxy_start(const ArchiveProxyOptions *options, GError **error);

guint16 archive_proxy_get_port(ArchiveProxy *proxy);
const char *archive_proxy_get_ca_file(ArchiveProxy *proxy);

/* While paused, traffic still passes, unrecorded. */
void archive_proxy_set_paused(ArchiveProxy *proxy, gboolean paused);

/* How many exchanges have been written so far. */
guint archive_proxy_get_recorded(ArchiveProxy *proxy);

/* Stops recording for good -- finishing the WARC file, so that it can be
 * packaged -- while traffic still passes. Exchanges in flight are not waited
 * for, since some never end (an event stream); they go unrecorded. */
gboolean archive_proxy_finish_recording(ArchiveProxy *proxy, GError **error);

/* Stops taking connections, ends those open, finishes the WARC file (if
 * archive_proxy_finish_recording() hasn't), and frees the proxy. Call it
 * from the thread of the owner's main context. */
gboolean archive_proxy_stop(ArchiveProxy *proxy, GError **error);

#endif /* end of include guard: _ARCHIVE_PROXY_H */
