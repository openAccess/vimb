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

#include <string.h>

#include "http1.h"

/* Longest chunk-size line, extensions included. */
#define MAX_CHUNK_LINE 4096

typedef enum {
    S_HEAD,
    S_BODY_LENGTH,
    S_BODY_CLOSE,
    S_CHUNK_SIZE,
    S_CHUNK_DATA,
    S_CHUNK_DATA_END,
    S_TRAILER,
    S_DONE,
    S_ERROR,
} State;

struct Http1Parser {
    Http1Kind        kind;
    gsize            max_head;
    State            state;
    GByteArray       *raw_head;
    gsize            line_start;    /* where the head line being read began */
    gboolean         has_head;
    Http1Head        head;
    char             *request_method;
    Http1BodyKind    body_kind;
    guint64          remaining;     /* of the body, or of the current chunk */
    GString          *line;         /* a chunk-size or trailer line so far */
    gsize            trailer_bytes;
    Http1PayloadFunc payload_func;
    gpointer         payload_data;
    Http1PayloadFunc raw_body_func;
    gpointer         raw_body_data;
};

G_DEFINE_QUARK(http1-error-quark, http1_error)

static void clear_header(gpointer data)
{
    Http1Header *header = data;
    g_free(header->name);
    g_free(header->value);
}

static void clear_head(Http1Head *head)
{
    g_clear_pointer(&head->method, g_free);
    g_clear_pointer(&head->target, g_free);
    g_clear_pointer(&head->reason, g_free);
    head->status        = 0;
    head->minor_version = 1;
    g_array_set_size(head->headers, 0);
}

Http1Parser *http1_parser_new(Http1Kind kind, gsize max_head)
{
    Http1Parser *parser  = g_new0(Http1Parser, 1);
    parser->kind         = kind;
    parser->max_head     = max_head;
    parser->raw_head     = g_byte_array_new();
    parser->line         = g_string_new(NULL);
    parser->head.headers = g_array_new(FALSE, TRUE, sizeof(Http1Header));
    g_array_set_clear_func(parser->head.headers, clear_header);
    http1_parser_reset(parser);
    return parser;
}

void http1_parser_free(Http1Parser *parser)
{
    if (!parser) {
        return;
    }
    clear_head(&parser->head);
    g_array_free(parser->head.headers, TRUE);
    g_byte_array_free(parser->raw_head, TRUE);
    g_string_free(parser->line, TRUE);
    g_free(parser->request_method);
    g_free(parser);
}

void http1_parser_reset(Http1Parser *parser)
{
    parser->state         = S_HEAD;
    parser->line_start    = 0;
    parser->has_head      = FALSE;
    parser->body_kind     = HTTP1_BODY_NONE;
    parser->remaining     = 0;
    parser->trailer_bytes = 0;
    g_byte_array_set_size(parser->raw_head, 0);
    g_string_truncate(parser->line, 0);
    clear_head(&parser->head);
}

void http1_parser_set_request_method(Http1Parser *parser, const char *method)
{
    g_free(parser->request_method);
    parser->request_method = g_strdup(method);
}

void http1_parser_set_payload_func(Http1Parser *parser, Http1PayloadFunc func,
        gpointer user_data)
{
    parser->payload_func = func;
    parser->payload_data = user_data;
}

void http1_parser_set_raw_body_func(Http1Parser *parser, Http1PayloadFunc func,
        gpointer user_data)
{
    parser->raw_body_func = func;
    parser->raw_body_data = user_data;
}

static void raw_body(Http1Parser *parser, const guint8 *data, gsize len)
{
    if (len && parser->raw_body_func) {
        parser->raw_body_func(data, len, parser->raw_body_data);
    }
}

static gboolean fail(Http1Parser *parser, GError **error, Http1Error code,
        const char *message)
{
    parser->state = S_ERROR;
    g_set_error_literal(error, HTTP1_ERROR, code, message);
    return FALSE;
}

static void payload(Http1Parser *parser, const guint8 *data, gsize len)
{
    if (len && parser->payload_func) {
        parser->payload_func(data, len, parser->payload_data);
    }
}

const char *http1_head_get(const Http1Head *head, const char *name)
{
    for (guint i = 0; i < head->headers->len; i++) {
        Http1Header *header = &g_array_index(head->headers, Http1Header, i);
        if (!g_ascii_strcasecmp(header->name, name)) {
            return header->value;
        }
    }
    return NULL;
}

gboolean http1_head_has_token(const Http1Head *head, const char *name,
        const char *token)
{
    for (guint i = 0; i < head->headers->len; i++) {
        Http1Header *header = &g_array_index(head->headers, Http1Header, i);
        if (g_ascii_strcasecmp(header->name, name)) {
            continue;
        }
        char **values = g_strsplit(header->value, ",", -1);
        gboolean found = FALSE;
        for (char **v = values; *v && !found; v++) {
            found = !g_ascii_strcasecmp(g_strstrip(*v), token);
        }
        g_strfreev(values);
        if (found) {
            return TRUE;
        }
    }
    return FALSE;
}

/* The last coding a message's Transfer-Encoding headers name, lowercased;
 * NULL when there are none. */
static char *last_transfer_coding(const Http1Head *head)
{
    char *last = NULL;
    for (guint i = 0; i < head->headers->len; i++) {
        Http1Header *header = &g_array_index(head->headers, Http1Header, i);
        if (g_ascii_strcasecmp(header->name, "Transfer-Encoding")) {
            continue;
        }
        char **codings = g_strsplit(header->value, ",", -1);
        for (char **c = codings; *c; c++) {
            char *coding = g_strstrip(*c);
            if (*coding) {
                g_free(last);
                last = g_ascii_strdown(coding, -1);
            }
        }
        g_strfreev(codings);
    }
    return last;
}

/* The Content-Length the message gives: -1 when it gives none, -2 when what it
 * gives is not a length, or two lengths that disagree. */
static gint64 content_length(const Http1Head *head)
{
    gint64 length = -1;
    for (guint i = 0; i < head->headers->len; i++) {
        Http1Header *header = &g_array_index(head->headers, Http1Header, i);
        if (g_ascii_strcasecmp(header->name, "Content-Length")) {
            continue;
        }
        /* "Content-Length: 42, 42" is a list some servers send. */
        char **values = g_strsplit(header->value, ",", -1);
        for (char **v = values; *v; v++) {
            char *value = g_strstrip(*v);
            guint64 n;
            if (!*value || !g_ascii_string_to_unsigned(value, 10, 0, G_MAXINT64, &n, NULL)
                    || (length >= 0 && (guint64)length != n)) {
                g_strfreev(values);
                return -2;
            }
            length = (gint64)n;
        }
        g_strfreev(values);
    }
    return length;
}

static gboolean is_token_char(char c)
{
    return g_ascii_isalnum(c) || strchr("!#$%&'*+-.^_`|~", c);
}

static gboolean parse_version(const char *text, int *minor)
{
    if (strncmp(text, "HTTP/1.", 7) || !g_ascii_isdigit(text[7]) || text[8]) {
        return FALSE;
    }
    *minor = text[7] - '0';
    return TRUE;
}

static gboolean parse_start_line(Http1Parser *parser, const char *line, GError **error)
{
    Http1Head *head = &parser->head;

    if (parser->kind == HTTP1_REQUEST) {
        const char *sp1 = strchr(line, ' ');
        const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1 || strchr(sp2 + 1, ' ')) {
            return fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed request line");
        }
        for (const char *c = line; c < sp1; c++) {
            if (!is_token_char(*c)) {
                return fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed request method");
            }
        }
        if (!parse_version(sp2 + 1, &head->minor_version)) {
            return fail(parser, error, HTTP1_ERROR_MALFORMED, "Not an HTTP/1.x request");
        }
        head->method = g_strndup(line, sp1 - line);
        head->target = g_strndup(sp1 + 1, sp2 - sp1 - 1);
        return TRUE;
    }

    /* HTTP/1.1 200 OK -- though the reason, and its space, may be missing. */
    char version[16];
    const char *sp = strchr(line, ' ');
    if (!sp || sp - line >= (long)sizeof(version)) {
        return fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed status line");
    }
    memcpy(version, line, sp - line);
    version[sp - line] = '\0';
    if (!parse_version(version, &head->minor_version)) {
        return fail(parser, error, HTTP1_ERROR_MALFORMED, "Not an HTTP/1.x response");
    }
    const char *code = sp + 1;
    if (!g_ascii_isdigit(code[0]) || !g_ascii_isdigit(code[1]) || !g_ascii_isdigit(code[2])
            || (code[3] && code[3] != ' ')) {
        return fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed status code");
    }
    head->status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    head->reason = g_strdup(code[3] ? code + 4 : "");
    return TRUE;
}

static gboolean parse_head(Http1Parser *parser, GError **error)
{
    /* The raw head without its final empty line, as a string. */
    char *text   = g_strndup((char *)parser->raw_head->data, parser->line_start);
    char **lines = g_strsplit(text, "\n", -1);
    gboolean ok  = TRUE;
    g_free(text);

    for (char **l = lines; *l; l++) {
        gsize len = strlen(*l);
        if (len && (*l)[len - 1] == '\r') {
            (*l)[--len] = '\0';
        }
    }

    if (!parse_start_line(parser, lines[0], error)) {
        ok = FALSE;
        goto out;
    }
    for (char **l = lines + 1; *l && **l; l++) {
        char *line = *l;
        if (*line == ' ' || *line == '\t') {
            /* A folded continuation of the line before. */
            GArray *headers = parser->head.headers;
            if (!headers->len) {
                ok = fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed header");
                goto out;
            }
            Http1Header *last = &g_array_index(headers, Http1Header, headers->len - 1);
            char *joined = g_strconcat(last->value, " ", g_strstrip(line), NULL);
            g_free(last->value);
            last->value = joined;
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon || colon == line) {
            ok = fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed header");
            goto out;
        }
        for (char *c = line; c < colon; c++) {
            if (!is_token_char(*c)) {
                ok = fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed header name");
                goto out;
            }
        }
        Http1Header header;
        header.name  = g_strndup(line, colon - line);
        header.value = g_strstrip(g_strdup(colon + 1));
        g_array_append_val(parser->head.headers, header);
    }
out:
    g_strfreev(lines);
    return ok;
}

/* Decides how the body is framed, once the head is in, and moves to the state
 * that reads it. */
static gboolean start_body(Http1Parser *parser, GError **error)
{
    Http1Head *head = &parser->head;

    parser->has_head = TRUE;
    if (parser->kind == HTTP1_RESPONSE) {
        gboolean head_request = parser->request_method
            && !g_ascii_strcasecmp(parser->request_method, "HEAD");
        if (head_request || head->status / 100 == 1 || head->status == 204
                || head->status == 304) {
            parser->body_kind = HTTP1_BODY_NONE;
            parser->state     = S_DONE;
            return TRUE;
        }
    }

    char *coding  = last_transfer_coding(head);
    gint64 length = content_length(head);
    if (coding) {
        gboolean chunked = !strcmp(coding, "chunked");
        g_free(coding);
        if (chunked) {
            /* A request that also gives a length is how requests are
             * smuggled past proxies; a response that does is answered by the
             * coding, as RFC 9112 says. */
            if (parser->kind == HTTP1_REQUEST && length != -1) {
                return fail(parser, error, HTTP1_ERROR_MALFORMED,
                        "Request with both Transfer-Encoding and Content-Length");
            }
            parser->body_kind = HTTP1_BODY_CHUNKED;
            parser->state     = S_CHUNK_SIZE;
            return TRUE;
        }
        if (parser->kind == HTTP1_REQUEST) {
            return fail(parser, error, HTTP1_ERROR_MALFORMED,
                    "Request with a transfer coding other than chunked");
        }
        parser->body_kind = HTTP1_BODY_UNTIL_CLOSE;
        parser->state     = S_BODY_CLOSE;
        return TRUE;
    }
    if (length == -2) {
        return fail(parser, error, HTTP1_ERROR_MALFORMED, "Invalid Content-Length");
    }
    if (length >= 0) {
        parser->body_kind = length ? HTTP1_BODY_LENGTH : HTTP1_BODY_NONE;
        parser->remaining = (guint64)length;
        parser->state     = length ? S_BODY_LENGTH : S_DONE;
        return TRUE;
    }
    if (parser->kind == HTTP1_REQUEST) {
        parser->body_kind = HTTP1_BODY_NONE;
        parser->state     = S_DONE;
    } else {
        parser->body_kind = HTTP1_BODY_UNTIL_CLOSE;
        parser->state     = S_BODY_CLOSE;
    }
    return TRUE;
}

/* Reads into parser->line up to the end of a line. Returns how many bytes it
 * took, and sets *complete once the line has ended; the line's ending is not
 * kept. */
static gssize read_line(Http1Parser *parser, const guint8 *data, gsize len,
        gsize limit, gboolean *complete, GError **error)
{
    const guint8 *nl = memchr(data, '\n', len);
    gsize take       = nl ? (gsize)(nl - data) + 1 : len;

    if (parser->line->len + take > limit) {
        fail(parser, error, HTTP1_ERROR_TOO_LARGE, "Line too long");
        return -1;
    }
    g_string_append_len(parser->line, (const char *)data, nl ? take - 1 : take);
    *complete = nl != NULL;
    if (*complete && parser->line->len && parser->line->str[parser->line->len - 1] == '\r') {
        g_string_truncate(parser->line, parser->line->len - 1);
    }
    return (gssize)take;
}

static gboolean parse_chunk_size(Http1Parser *parser, GError **error)
{
    const char *s = parser->line->str;
    guint64 size  = 0;
    int digits    = 0;

    for (; g_ascii_isxdigit(*s); s++, digits++) {
        if (size > (G_MAXUINT64 >> 4)) {
            return fail(parser, error, HTTP1_ERROR_MALFORMED, "Chunk size too large");
        }
        size = (size << 4) | (guint64)g_ascii_xdigit_value(*s);
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!digits || (*s && *s != ';')) {
        return fail(parser, error, HTTP1_ERROR_MALFORMED, "Malformed chunk size");
    }
    g_string_truncate(parser->line, 0);
    parser->remaining = size;
    parser->state     = size ? S_CHUNK_DATA : S_TRAILER;
    return TRUE;
}

gssize http1_parser_feed(Http1Parser *parser, const guint8 *data, gsize len,
        GError **error)
{
    gsize i = 0;

    while (i < len && parser->state != S_DONE) {
        switch (parser->state) {
            case S_HEAD: {
                const guint8 *nl = memchr(data + i, '\n', len - i);
                gsize take       = nl ? (gsize)(nl - (data + i)) + 1 : len - i;
                if (parser->raw_head->len + take > parser->max_head) {
                    fail(parser, error, HTTP1_ERROR_TOO_LARGE, "Message head too large");
                    return -1;
                }
                g_byte_array_append(parser->raw_head, data + i, take);
                i += take;
                if (!nl) {
                    break;
                }
                gsize line_len = parser->raw_head->len - parser->line_start - 1;
                const guint8 *line = parser->raw_head->data + parser->line_start;
                if (line_len && line[line_len - 1] == '\r') {
                    line_len--;
                }
                if (line_len) {
                    parser->line_start = parser->raw_head->len;
                } else if (parser->line_start == 0) {
                    /* Empty lines before a message are allowed, and dropped. */
                    g_byte_array_set_size(parser->raw_head, 0);
                } else if (!parse_head(parser, error) || !start_body(parser, error)) {
                    return -1;
                }
                break;
            }

            case S_BODY_LENGTH: {
                gsize n = (gsize)MIN(parser->remaining, (guint64)(len - i));
                raw_body(parser, data + i, n);
                payload(parser, data + i, n);
                i += n;
                parser->remaining -= n;
                if (!parser->remaining) {
                    parser->state = S_DONE;
                }
                break;
            }

            case S_BODY_CLOSE:
                raw_body(parser, data + i, len - i);
                payload(parser, data + i, len - i);
                i = len;
                break;

            case S_CHUNK_SIZE:
            case S_CHUNK_DATA_END:
            case S_TRAILER: {
                gboolean complete;
                gsize limit = parser->state == S_TRAILER
                    ? parser->max_head - MIN(parser->max_head, parser->trailer_bytes)
                    : MAX_CHUNK_LINE;
                gssize n = read_line(parser, data + i, len - i, limit, &complete, error);
                if (n < 0) {
                    return -1;
                }
                raw_body(parser, data + i, (gsize)n);
                i += (gsize)n;
                if (parser->state == S_TRAILER) {
                    parser->trailer_bytes += (gsize)n;
                }
                if (!complete) {
                    break;
                }
                if (parser->state == S_CHUNK_SIZE) {
                    if (!parse_chunk_size(parser, error)) {
                        return -1;
                    }
                } else if (parser->state == S_CHUNK_DATA_END) {
                    if (parser->line->len) {
                        fail(parser, error, HTTP1_ERROR_MALFORMED, "Chunk not followed by CRLF");
                        return -1;
                    }
                    parser->state = S_CHUNK_SIZE;
                } else if (!parser->line->len) {
                    parser->state = S_DONE;
                }
                g_string_truncate(parser->line, 0);
                break;
            }

            case S_CHUNK_DATA: {
                gsize n = (gsize)MIN(parser->remaining, (guint64)(len - i));
                raw_body(parser, data + i, n);
                payload(parser, data + i, n);
                i += n;
                parser->remaining -= n;
                if (!parser->remaining) {
                    parser->state = S_CHUNK_DATA_END;
                }
                break;
            }

            case S_DONE:
                break;

            case S_ERROR:
                g_set_error_literal(error, HTTP1_ERROR, HTTP1_ERROR_MALFORMED,
                        "Parser already failed");
                return -1;
        }
    }
    return (gssize)i;
}

gboolean http1_parser_eof(Http1Parser *parser, GError **error)
{
    if (parser->state == S_BODY_CLOSE) {
        parser->state = S_DONE;
        return TRUE;
    }
    if (parser->state == S_DONE) {
        return TRUE;
    }
    if (parser->state == S_HEAD && parser->raw_head->len == 0) {
        g_set_error_literal(error, HTTP1_ERROR, HTTP1_ERROR_TRUNCATED,
                "The stream ended before a message began");
        return FALSE;
    }
    return fail(parser, error, HTTP1_ERROR_TRUNCATED, "The stream ended within a message");
}

gboolean http1_parser_has_head(const Http1Parser *parser)
{
    return parser->has_head;
}

gboolean http1_parser_is_done(const Http1Parser *parser)
{
    return parser->state == S_DONE;
}

const Http1Head *http1_parser_head(const Http1Parser *parser)
{
    return &parser->head;
}

const GByteArray *http1_parser_raw_head(const Http1Parser *parser)
{
    return parser->raw_head;
}

Http1BodyKind http1_parser_body_kind(const Http1Parser *parser)
{
    return parser->body_kind;
}

gboolean http1_parser_keep_alive(const Http1Parser *parser)
{
    const Http1Head *head = &parser->head;

    if (parser->body_kind == HTTP1_BODY_UNTIL_CLOSE
            || http1_head_has_token(head, "Connection", "close")) {
        return FALSE;
    }
    if (head->minor_version == 0) {
        return http1_head_has_token(head, "Connection", "keep-alive");
    }
    return TRUE;
}
