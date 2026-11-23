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

#ifndef _ARCHIVE_HTTP1_H
#define _ARCHIVE_HTTP1_H

#include <glib.h>

/* An HTTP/1.x message parser that is fed bytes as they arrive.
 *
 * It finds where a message ends -- by Content-Length, chunked coding or the
 * end of the stream -- and hands on the payload with any chunked coding taken
 * off, for digests; the bytes themselves are the caller's to keep or pass on
 * as they came, since an archive keeps what the server sent. Feeding stops at
 * the end of a message, so whatever follows belongs to the next one. */

#define HTTP1_ERROR (http1_error_quark())
GQuark http1_error_quark(void);

typedef enum {
    HTTP1_ERROR_MALFORMED,
    HTTP1_ERROR_TOO_LARGE,
    HTTP1_ERROR_TRUNCATED,
} Http1Error;

typedef enum {
    HTTP1_REQUEST,
    HTTP1_RESPONSE,
} Http1Kind;

typedef enum {
    HTTP1_BODY_NONE,
    HTTP1_BODY_LENGTH,
    HTTP1_BODY_CHUNKED,
    HTTP1_BODY_UNTIL_CLOSE,     /* a response that ends where its stream does */
} Http1BodyKind;

typedef struct {
    char *name;
    char *value;
} Http1Header;

typedef struct {
    char   *method;             /* requests */
    char   *target;             /* requests */
    guint  status;              /* responses */
    char   *reason;             /* responses */
    int    minor_version;       /* the x of HTTP/1.x */
    GArray *headers;            /* of Http1Header, in the order they came */
} Http1Head;

typedef struct Http1Parser Http1Parser;
typedef void (*Http1PayloadFunc)(const guint8 *data, gsize len, gpointer user_data);

/* A parser for requests or responses whose head may be at most max_head
 * bytes long. */
Http1Parser *http1_parser_new(Http1Kind kind, gsize max_head);
void http1_parser_free(Http1Parser *parser);

/* Makes ready for the next message, keeping the request method and payload
 * function. */
void http1_parser_reset(Http1Parser *parser);

/* For a response: the method of the request it answers, since the answer to
 * a HEAD has no body whatever its headers say. */
void http1_parser_set_request_method(Http1Parser *parser, const char *method);

/* Called with the payload as it is parsed, any chunked coding removed. */
void http1_parser_set_payload_func(Http1Parser *parser, Http1PayloadFunc func,
        gpointer user_data);

/* Called with the body's bytes exactly as they came, chunked coding and all:
 * everything the parser consumes after the head. */
void http1_parser_set_raw_body_func(Http1Parser *parser, Http1PayloadFunc func,
        gpointer user_data);

/* Parses what it can of data, returning how much of it belongs to the current
 * message (all of it, unless the message ended within it), or -1 when the
 * message is not HTTP. */
gssize http1_parser_feed(Http1Parser *parser, const guint8 *data, gsize len,
        GError **error);

/* Tells the parser that the stream ended. That completes a response whose
 * body runs until then; any other message it cuts short. */
gboolean http1_parser_eof(Http1Parser *parser, GError **error);

gboolean http1_parser_has_head(const Http1Parser *parser);
gboolean http1_parser_is_done(const Http1Parser *parser);
const Http1Head *http1_parser_head(const Http1Parser *parser);

/* The head exactly as it came, up to and including the empty line. */
const GByteArray *http1_parser_raw_head(const Http1Parser *parser);

Http1BodyKind http1_parser_body_kind(const Http1Parser *parser);

/* Whether the connection may carry another message after this one, as far as
 * this message says. */
gboolean http1_parser_keep_alive(const Http1Parser *parser);

/* The first value of a header, by case-insensitive name; NULL if absent. */
const char *http1_head_get(const Http1Head *head, const char *name);

/* Whether any header called name lists token among its comma-separated
 * values, ignoring case: Connection: close, Transfer-Encoding: chunked. */
gboolean http1_head_has_token(const Http1Head *head, const char *name,
        const char *token);

#endif /* end of include guard: _ARCHIVE_HTTP1_H */
