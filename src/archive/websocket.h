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

#ifndef _ARCHIVE_WEBSOCKET_H
#define _ARCHIVE_WEBSOCKET_H

#include <glib.h>

/* Websockets (RFC 6455), as far as archiving them goes: reading the frames
 * that pass one way, a message at a time; writing frames; the handshake's
 * accept; and the messages a websocket carried, as a record keeps them.
 *
 * Compressed frames (permessage-deflate) aren't read: the archiving proxy
 * doesn't offer the extension, so a site never sends them. */

#define WS_TEXT   0x1
#define WS_BINARY 0x2
#define WS_CLOSE  0x8
#define WS_PING   0x9
#define WS_PONG   0xa

/* The longest message read whole; a longer one is passed on, not kept. */
#define WS_MESSAGE_MAX (16u << 20)

typedef struct WsReader WsReader;

/* Called with each message as it completes -- text, binary, or a control
 * frame's -- unmasked. */
typedef void (*WsMessageFunc)(guint opcode, const guint8 *data, gsize len, gpointer user_data);

WsReader *ws_reader_new(WsMessageFunc func, gpointer user_data);
void ws_reader_free(WsReader *reader);

/* Reads what it can of data; FALSE once the stream is not websocket frames,
 * after which it reads nothing more. */
gboolean ws_reader_feed(WsReader *reader, const guint8 *data, gsize len);

/* A frame, whole and final, unmasked as a server sends it, or masked with
 * mask (4 bytes) as a client must. */
GBytes *ws_frame(guint opcode, const guint8 *data, gsize len, const guint8 *mask);

/* The Sec-WebSocket-Accept for a Sec-WebSocket-Key. */
char *ws_accept(const char *key);

/* One message of those a websocket carried. */
typedef struct {
    gboolean from_server;
    gint64   ms;                /* since the websocket opened */
    guint    opcode;
    GBytes   *data;
} WsMessage;

void ws_message_free(WsMessage *message);

/* The messages as a record keeps them: JSON, a line each, after a line
 * naming the format and the URL. */
GBytes *ws_messages_write(const char *url, GPtrArray *messages);

/* And read back: WsMessages, or NULL if it isn't that. */
GPtrArray *ws_messages_read(const guint8 *data, gsize len);

#endif /* end of include guard: _ARCHIVE_WEBSOCKET_H */
