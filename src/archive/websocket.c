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

#include <json-glib/json-glib.h>
#include <string.h>

#include "json.h"
#include "websocket.h"

#define WS_FORMAT "vimb-websocket-messages-1"

struct WsReader {
    WsMessageFunc func;
    gpointer      user_data;
    GByteArray    *buf;         /* what hasn't made a frame yet */
    GByteArray    *message;     /* a fragmented message so far */
    guint         message_opcode;
    gboolean      skipping;     /* a message too long to keep */
    gboolean      broken;
};

WsReader *ws_reader_new(WsMessageFunc func, gpointer user_data)
{
    WsReader *r  = g_new0(WsReader, 1);
    r->func      = func;
    r->user_data = user_data;
    r->buf       = g_byte_array_new();
    r->message   = g_byte_array_new();
    return r;
}

void ws_reader_free(WsReader *r)
{
    if (r) {
        g_byte_array_unref(r->buf);
        g_byte_array_unref(r->message);
        g_free(r);
    }
}

/* Reads one frame from the front of buf, if it is all there. */
static gboolean read_frame(WsReader *r, gboolean *got)
{
    const guint8 *b = r->buf->data;
    gsize n = r->buf->len;
    *got = FALSE;
    if (n < 2) {
        return TRUE;
    }
    gboolean fin    = b[0] & 0x80;
    guint opcode    = b[0] & 0x0f;
    gboolean masked = b[1] & 0x80;
    guint64 len     = b[1] & 0x7f;
    gsize at        = 2;
    if (b[0] & 0x70) {
        return FALSE;               /* an extension's bits: not offered, not read */
    }
    if (len == 126) {
        if (n < 4) {
            return TRUE;
        }
        len = ((guint64)b[2] << 8) | b[3];
        at  = 4;
    } else if (len == 127) {
        if (n < 10) {
            return TRUE;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | b[2 + i];
        }
        at = 10;
    }
    if (len > G_MAXSIZE / 2) {
        return FALSE;
    }
    const guint8 *mask = NULL;
    if (masked) {
        if (n < at + 4) {
            return TRUE;
        }
        mask = b + at;
        at  += 4;
    }
    if (n - at < len) {
        return TRUE;
    }
    guint8 *payload = g_malloc(len ? len : 1);
    for (guint64 i = 0; i < len; i++) {
        payload[i] = b[at + i] ^ (mask ? mask[i % 4] : 0);
    }

    if (opcode >= 0x8) {            /* control frames come whole, between fragments */
        r->func(opcode, payload, len, r->user_data);
    } else {
        if (opcode != 0) {          /* a message begins */
            g_byte_array_set_size(r->message, 0);
            r->message_opcode = opcode;
            r->skipping       = FALSE;
        }
        if (!r->skipping && r->message->len + len > WS_MESSAGE_MAX) {
            r->skipping = TRUE;
            g_byte_array_set_size(r->message, 0);
        }
        if (!r->skipping) {
            g_byte_array_append(r->message, payload, len);
        }
        if (fin) {
            if (!r->skipping && r->message_opcode) {
                r->func(r->message_opcode, r->message->data, r->message->len, r->user_data);
            }
            g_byte_array_set_size(r->message, 0);
            r->message_opcode = 0;
        }
    }
    g_free(payload);
    g_byte_array_remove_range(r->buf, 0, at + len);
    *got = TRUE;
    return TRUE;
}

gboolean ws_reader_feed(WsReader *r, const guint8 *data, gsize len)
{
    if (r->broken) {
        return FALSE;
    }
    g_byte_array_append(r->buf, data, len);
    gboolean got = TRUE;
    while (got) {
        if (!read_frame(r, &got)) {
            r->broken = TRUE;
            g_byte_array_set_size(r->buf, 0);
            return FALSE;
        }
    }
    return TRUE;
}

GBytes *ws_frame(guint opcode, const guint8 *data, gsize len, const guint8 *mask)
{
    GByteArray *f = g_byte_array_sized_new(len + 14);
    guint8 head[10];
    gsize n = 2;
    head[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        head[1] = (guint8)len;
    } else if (len <= 0xffff) {
        head[1] = 126;
        head[2] = (guint8)(len >> 8);
        head[3] = (guint8)len;
        n = 4;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; i++) {
            head[2 + i] = (guint8)((guint64)len >> (56 - 8 * i));
        }
        n = 10;
    }
    if (mask) {
        head[1] |= 0x80;
    }
    g_byte_array_append(f, head, n);
    if (mask) {
        g_byte_array_append(f, mask, 4);
    }
    gsize start = f->len;
    g_byte_array_append(f, data, len);
    for (gsize i = 0; mask && i < len; i++) {
        f->data[start + i] ^= mask[i % 4];
    }
    return g_byte_array_free_to_bytes(f);
}

char *ws_accept(const char *key)
{
    char *joined = g_strconcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", NULL);
    guint8 digest[20];
    gsize len = sizeof(digest);
    GChecksum *sha1 = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sha1, (const guchar *)joined, -1);
    g_checksum_get_digest(sha1, digest, &len);
    g_checksum_free(sha1);
    g_free(joined);
    return g_base64_encode(digest, len);
}

void ws_message_free(WsMessage *m)
{
    if (m) {
        g_bytes_unref(m->data);
        g_free(m);
    }
}

GBytes *ws_messages_write(const char *url, GPtrArray *messages)
{
    GString *out = g_string_new("{");
    archive_json_append_member(out, "format", WS_FORMAT);
    archive_json_append_member(out, "url", url);
    g_string_append(out, "}\n");
    for (guint i = 0; i < messages->len; i++) {
        WsMessage *m = g_ptr_array_index(messages, i);
        gsize len;
        const guint8 *data = g_bytes_get_data(m->data, &len);
        gboolean text = m->opcode == WS_TEXT && g_utf8_validate((const char *)data, len, NULL);
        char *value = text ? g_strndup((const char *)data, len) : g_base64_encode(data, len);
        g_string_append_c(out, '{');
        archive_json_append_member(out, "from", m->from_server ? "server" : "client");
        archive_json_append_number(out, "ms", (guint64)MAX(m->ms, 0));
        archive_json_append_member(out, "type", m->opcode == WS_TEXT ? (text ? "text" : "text-base64")
                : m->opcode == WS_CLOSE ? "close" : "binary");
        archive_json_append_member(out, "data", value);
        g_string_append(out, "}\n");
        g_free(value);
    }
    gsize size = out->len;
    return g_bytes_new_take(g_string_free(out, FALSE), size);
}

GPtrArray *ws_messages_read(const guint8 *data, gsize len)
{
    char *text = g_strndup((const char *)data, len);
    char **lines = g_strsplit(text, "\n", -1);
    GPtrArray *messages = NULL;
    JsonParser *parser = json_parser_new();
    for (char **l = lines; *l; l++) {
        if (!**l || !json_parser_load_from_data(parser, *l, -1, NULL)
                || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            continue;
        }
        JsonObject *o = json_node_get_object(json_parser_get_root(parser));
        if (!messages) {
            if (g_strcmp0(json_object_get_string_member_with_default(o, "format", ""), WS_FORMAT)) {
                break;
            }
            messages = g_ptr_array_new_with_free_func((GDestroyNotify)ws_message_free);
            continue;
        }
        const char *type  = json_object_get_string_member_with_default(o, "type", "");
        const char *value = json_object_get_string_member_with_default(o, "data", "");
        WsMessage *m      = g_new0(WsMessage, 1);
        m->from_server    = !g_strcmp0(json_object_get_string_member_with_default(o, "from", ""),
                "server");
        m->ms     = json_object_get_int_member_with_default(o, "ms", 0);
        m->opcode = !strcmp(type, "text") || !strcmp(type, "text-base64") ? WS_TEXT
            : !strcmp(type, "close") ? WS_CLOSE : WS_BINARY;
        if (!strcmp(type, "text")) {
            m->data = g_bytes_new(value, strlen(value));
        } else {
            gsize n;
            guint8 *raw = g_base64_decode(value, &n);
            m->data = g_bytes_new_take(raw, n);
        }
        g_ptr_array_add(messages, m);
    }
    g_object_unref(parser);
    g_strfreev(lines);
    g_free(text);
    return messages;
}
