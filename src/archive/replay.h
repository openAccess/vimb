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

#ifndef _ARCHIVE_REPLAY_H
#define _ARCHIVE_REPLAY_H

#include <gio/gio.h>

#include "http1.h"

/* Replay: a WACZ opened to answer a browser's requests from what it holds,
 * as the archiving proxy answers them in replay mode (see proxy.h). Nothing
 * is fetched from anywhere else: what the archive doesn't hold, the browser
 * is told it doesn't.
 *
 * A request is answered by the capture of it -- by its URL, or for a POST
 * by what it sent as well (see request_key.h) -- nearest the moment being
 * replayed: for a page, when the archive's page list says it was captured;
 * for what a page loads, when the page that asked for it was. A whole
 * capture is preferred to a nearer part of one (a 206), and for a page, one
 * in which the site let the visitor in to one in which it turned them away
 * (401, 403, 429), as a bot check that reloads once satisfied would
 * otherwise reload for ever. A POST or PUT whose body nothing was filed
 * under is answered by the capture whose body agrees with it most, and a
 * stream of counted requests (YouTube's video, "rn") in the capture's order.
 *
 * The index is read into memory; the records are read from the archive as
 * they're asked for. Any thread. */

/* The content type of a record holding a websocket's messages (see
 * recorder.h): it answers the websocket's handshake, and nothing else. */
#define ARCHIVE_WEBSOCKET_MIME "application/x-vimb-websocket-messages"

/* Where replay's own pages are: the list of what the archive holds. */
#define ARCHIVE_REPLAY_HOME "http://replay.vimb/"

typedef struct ArchiveReplay ArchiveReplay;

typedef struct {
    char *url;
    char *ts;                   /* 14 digits, as an index writes a time */
    char *title;
} ArchiveReplayPage;

ArchiveReplay *archive_replay_open(const char *wacz_path, GError **error);
void archive_replay_free(ArchiveReplay *replay);

/* The pages the archive says it captured, in its order: ArchiveReplayPages. */
const GPtrArray *archive_replay_pages(ArchiveReplay *replay);

/* What the archive calls itself, or its file's name. */
const char *archive_replay_title(ArchiveReplay *replay);

/* How many requests have been answered from the archive, and how many it
 * couldn't answer. */
void archive_replay_counts(ArchiveReplay *replay, guint *served, guint *missed);

/* The moment a page at url is replayed at -- as its request will be
 * answered, referer being the page it was reached from -- in seconds since
 * 1970: for setting a replayed page's clock. */
gint64 archive_replay_page_moment(ArchiveReplay *replay, const char *url, const char *referer);

typedef struct {
    const char      *method;
    const char      *url;       /* absolute */
    const Http1Head *head;      /* the request's headers */
    const guint8    *body;
    gsize           body_len;
} ArchiveReplayRequest;

/* The answer to request, as a whole HTTP/1.1 response for the browser: from
 * the archive, or saying what it doesn't hold and why. found says which. */
GBytes *archive_replay_answer(ArchiveReplay *replay, const ArchiveReplayRequest *request,
        gboolean *found);

/* For a websocket's handshake: the messages the archive holds of it, and
 * the subprotocol its handshake agreed; NULL if it holds none. */
typedef struct {
    GPtrArray *messages;        /* of WsMessage (see websocket.h) */
    char      *protocol;        /* Sec-WebSocket-Protocol, or NULL */
} ArchiveReplayWebsocket;

ArchiveReplayWebsocket *archive_replay_websocket(ArchiveReplay *replay,
        const ArchiveReplayRequest *request);
void archive_replay_websocket_free(ArchiveReplayWebsocket *ws);

/* The 14 digits an index writes a time in, from those or from RFC 3339 as a
 * page list writes it ("2026-11-23T21:57:53Z"). */
char *archive_replay_index_time(const char *time);

#endif /* end of include guard: _ARCHIVE_REPLAY_H */
