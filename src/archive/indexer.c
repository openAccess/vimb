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
#include <zlib.h>

#include "indexer.h"
#include "json.h"
#include "request_key.h"
#include "surt.h"
#include "warc.h"

#define READ_SIZE        (64 * 1024)
/* How much of each record is kept to read: its WARC head, its HTTP head, and
 * a request's body for the key of the response beside it. */
#define MAX_RECORD_START (1024 * 1024)

typedef struct {
    char *name;
    char *value;
} Field;

typedef struct {
    char       *type;
    char       *url;
    char       *date;
    char       *id;
    char       *concurrent_to;
    char       *payload_digest;
    char       *content_type;

    gboolean   has_http;
    char       *status;
    char       *method;
    char       *http_content_type;
    char       *referer;
    char       *cookie;
    gboolean   navigation_request;  /* Sec-Fetch-Dest: document, Mode: navigate */
    GByteArray *body;

    guint64    offset;
    guint64    length;
    char       *record_digest;

    gboolean   joined;
    gboolean   navigation;
    char       *request_referer;
    char       *request_cookie;
    char       *request_method;
    char       *request_content_type;
    GByteArray *request_body;
} Record;

static void record_free(Record *r)
{
    if (!r) {
        return;
    }
    g_free(r->type);
    g_free(r->url);
    g_free(r->date);
    g_free(r->id);
    g_free(r->concurrent_to);
    g_free(r->payload_digest);
    g_free(r->content_type);
    g_free(r->status);
    g_free(r->method);
    g_free(r->http_content_type);
    g_free(r->referer);
    g_free(r->cookie);
    if (r->body) {
        g_byte_array_free(r->body, TRUE);
    }
    g_free(r->record_digest);
    g_free(r->request_referer);
    g_free(r->request_cookie);
    g_free(r->request_method);
    g_free(r->request_content_type);
    if (r->request_body) {
        g_byte_array_free(r->request_body, TRUE);
    }
    g_free(r);
}

static void field_clear(gpointer data)
{
    Field *f = data;
    g_free(f->name);
    g_free(f->value);
}

static const char *field_value(GArray *fields, const char *name)
{
    for (guint i = 0; i < fields->len; i++) {
        Field *f = &g_array_index(fields, Field, i);
        if (!g_ascii_strcasecmp(f->name, name)) {
            return f->value;
        }
    }
    return "";
}

static const char *find_blank_line(const char *data, gsize len)
{
    for (gsize i = 0; i + 4 <= len; i++) {
        if (!memcmp(data + i, "\r\n\r\n", 4)) {
            return data + i;
        }
    }
    return NULL;
}

/* Reads a header block -- a first line, then fields up to a blank line --
 * returning how long it is, through the blank line, or 0 if it doesn't end
 * within len. */
static gsize parse_header_block(const char *data, gsize len, char **first_line,
        GArray *fields)
{
    const char *end = find_blank_line(data, len);
    if (!end) {
        return 0;
    }
    char *text   = g_strndup(data, end - data);
    char **lines = g_strsplit(text, "\r\n", -1);
    g_free(text);
    *first_line = g_strdup(lines[0]);
    for (char **l = lines + 1; *l; l++) {
        if (**l == ' ' || **l == '\t') {
            if (fields->len) {
                Field *last = &g_array_index(fields, Field, fields->len - 1);
                char *joined = g_strconcat(last->value, " ", g_strstrip(*l), NULL);
                g_free(last->value);
                last->value = joined;
            }
            continue;
        }
        char *colon = strchr(*l, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        Field f = {g_strdup(g_strstrip(*l)), g_strdup(g_strstrip(colon + 1))};
        g_array_append_val(fields, f);
    }
    g_strfreev(lines);
    return (gsize)(end - data) + 4;
}

static GArray *new_fields(void)
{
    GArray *fields = g_array_new(FALSE, TRUE, sizeof(Field));
    g_array_set_clear_func(fields, field_clear);
    return fields;
}

static Record *parse_record(const char *start, gsize start_len, guint64 offset,
        guint64 length, char *record_digest)
{
    char *version  = NULL;
    GArray *fields = new_fields();
    gsize head_len = parse_header_block(start, start_len, &version, fields);
    if (!head_len || !g_str_has_prefix(version, "WARC/")) {
        g_free(version);
        g_array_free(fields, TRUE);
        g_free(record_digest);
        return NULL;
    }
    g_free(version);

    Record *r = g_new0(Record, 1);
    r->type   = g_ascii_strdown(field_value(fields, "WARC-Type"), -1);
    /* WARC 1.0 wrote the URI in angle brackets; 1.1 doesn't. */
    const char *url = field_value(fields, "WARC-Target-URI");
    gsize url_len   = strlen(url);
    r->url = (url_len >= 2 && url[0] == '<' && url[url_len - 1] == '>')
        ? g_strndup(url + 1, url_len - 2) : g_strdup(url);
    r->date           = g_strdup(field_value(fields, "WARC-Date"));
    r->id             = g_strdup(field_value(fields, "WARC-Record-ID"));
    r->concurrent_to  = g_strdup(field_value(fields, "WARC-Concurrent-To"));
    r->payload_digest = g_strdup(field_value(fields, "WARC-Payload-Digest"));
    r->content_type   = g_strdup(field_value(fields, "Content-Type"));
    r->offset         = offset;
    r->length         = length;
    r->record_digest  = record_digest;

    guint64 content_length = g_ascii_strtoull(field_value(fields, "Content-Length"), NULL, 10);
    g_array_free(fields, TRUE);
    const char *block = start + head_len;
    gsize block_len   = start_len - head_len;
    gboolean whole    = block_len >= content_length;
    if (block_len > content_length) {
        block_len = (gsize)content_length;
    }
    const char *payload = block;
    gsize payload_len   = block_len;

    /* The HTTP message a response, request or revisit of an http(s) URI
     * holds, decided by the record's type as warcio decides it. */
    gboolean http_type = !strcmp(r->type, "response") || !strcmp(r->type, "request")
        || !strcmp(r->type, "revisit");
    if (http_type && block_len && (g_str_has_prefix(r->url, "http:")
                || g_str_has_prefix(r->url, "https:"))) {
        char *start_line    = NULL;
        GArray *http_fields = new_fields();
        gsize http_len      = parse_header_block(block, block_len, &start_line, http_fields);
        if (http_len) {
            r->has_http = TRUE;
            payload     = block + http_len;
            payload_len = block_len - http_len;
            char **words = g_strsplit_set(g_strstrip(start_line), " ", -1);
            GPtrArray *nonempty = g_ptr_array_new();
            for (char **w = words; *w; w++) {
                if (**w) {
                    g_ptr_array_add(nonempty, *w);
                }
            }
            if (!strcmp(r->type, "request")) {
                if (nonempty->len) {
                    r->method = g_ascii_strup(g_ptr_array_index(nonempty, 0), -1);
                }
                r->body = g_byte_array_new();
                g_byte_array_append(r->body, (const guint8 *)payload, (guint)payload_len);
                r->navigation_request =
                    !g_ascii_strcasecmp(field_value(http_fields, "Sec-Fetch-Dest"), "document")
                    && !g_ascii_strcasecmp(field_value(http_fields, "Sec-Fetch-Mode"), "navigate");
            } else if (nonempty->len >= 2) {
                r->status = g_strdup(g_ptr_array_index(nonempty, 1));
            }
            g_ptr_array_free(nonempty, TRUE);
            g_strfreev(words);
            r->http_content_type = g_strdup(field_value(http_fields, "Content-Type"));
            r->referer           = g_strdup(field_value(http_fields, "Referer"));
            r->cookie            = g_strdup(field_value(http_fields, "Cookie"));
        }
        g_free(start_line);
        g_array_free(http_fields, TRUE);
    }

    /* A record without a payload digest is given one, as cdxj-indexer gives
     * it, when it is held whole. */
    if (!*r->payload_digest && whole && strcmp(r->type, "request")
            && strcmp(r->type, "warcinfo")) {
        guint8 digest[20];
        gsize digest_len = sizeof(digest);
        GChecksum *sum   = g_checksum_new(G_CHECKSUM_SHA1);
        g_checksum_update(sum, (const guchar *)payload, (gssize)payload_len);
        g_checksum_get_digest(sum, digest, &digest_len);
        g_checksum_free(sum);
        g_free(r->payload_digest);
        r->payload_digest = warc_label_sha1(digest);
    }
    return r;
}

static gboolean is_indexed(const Record *r)
{
    if (!strcmp(r->type, "response") || !strcmp(r->type, "revisit")) {
        return TRUE;
    }
    if (!strcmp(r->type, "resource") || !strcmp(r->type, "metadata")) {
        return strcmp(r->content_type, "application/warc-fields") != 0;
    }
    return FALSE;
}

/* Joins a request and the response written beside it, in either order:
 * the same URI, and b naming a as the record it was written with. */
static gboolean join(Record *a, Record *b)
{
    if (strcmp(a->url, b->url) || strcmp(b->concurrent_to, a->id)) {
        return FALSE;
    }
    Record *request, *response;
    if (!strcmp(a->type, "request") && !strcmp(b->type, "response")) {
        request  = a;
        response = b;
    } else if (!strcmp(a->type, "response") && !strcmp(b->type, "request")) {
        request  = b;
        response = a;
    } else {
        return FALSE;
    }
    if (!request->has_http) {
        return TRUE;
    }
    response->joined          = TRUE;
    response->request_referer = g_strdup(request->referer);
    response->request_cookie  = g_strdup(request->cookie);
    response->navigation      = request->navigation_request && response->status
        && response->status[0] == '2';
    if (!g_strcmp0(request->method, "POST") || !g_strcmp0(request->method, "PUT")) {
        response->request_method       = g_strdup(request->method);
        response->request_content_type = g_strdup(request->http_content_type);
        response->request_body         = request->body;
        request->body                  = NULL;
    }
    return TRUE;
}

static gboolean is_text(const char *value)
{
    return value && *value && g_utf8_validate(value, -1, NULL);
}

static void set_if_text(GString *json, const char *name, const char *value)
{
    if (is_text(value)) {
        archive_json_append_member(json, name, value);
    }
}

/* "text/html; charset=utf-8" -> "text/html" */
static char *bare_media_type(const char *content_type)
{
    gsize end = strcspn(content_type, "; \t");
    char *bare = g_strndup(content_type, end);
    return g_strstrip(bare);
}

char *archive_index_timestamp(const char *date)
{
    GString *digits = g_string_new(NULL);
    for (const char *c = date; *c && digits->len < 14; c++) {
        if (g_ascii_isdigit(*c)) {
            g_string_append_c(digits, *c);
        }
    }
    return g_string_free(digits, FALSE);
}

typedef struct {
    const char       *filename;
    ArchiveWarcIndex *index;
    Record           *held;     /* one behind, to join a response to its request */
} Lines;

static void write_line(Lines *lines, Record *r)
{
    if (!is_indexed(r) || !g_utf8_validate(r->url, -1, NULL)) {
        return;
    }
    char *key = r->request_method
        ? archive_request_key(r->url, r->request_method, r->request_content_type,
                r->request_body ? r->request_body->data : NULL,
                r->request_body ? (gssize)r->request_body->len : -1)
        : archive_surt(r->url);
    if (!key) {
        key = g_strdup(r->url);
    }

    GString *json = g_string_new("{");
    archive_json_append_member(json, "url", r->url);
    if (!strcmp(r->type, "revisit")) {
        archive_json_append_member(json, "mime", "warc/revisit");
    } else {
        char *mime = bare_media_type(!strcmp(r->type, "response") && r->http_content_type
                ? r->http_content_type : r->content_type);
        set_if_text(json, "mime", mime);
        g_free(mime);
    }
    if (r->has_http) {
        set_if_text(json, "status", r->status);
    }
    set_if_text(json, "digest", r->payload_digest);
    char number[32];
    g_snprintf(number, sizeof(number), "%" G_GUINT64_FORMAT, r->length);
    archive_json_append_member(json, "length", number);
    g_snprintf(number, sizeof(number), "%" G_GUINT64_FORMAT, r->offset);
    archive_json_append_member(json, "offset", number);
    archive_json_append_member(json, "filename", lines->filename);
    if (r->joined) {
        set_if_text(json, "referrer", r->request_referer);
        set_if_text(json, "req.http:cookie", r->request_cookie);
    }
    archive_json_append_member(json, "recordDigest", r->record_digest);
    if (r->request_method) {
        const guint8 *body = r->request_body ? r->request_body->data : (const guint8 *)"";
        gsize body_len     = r->request_body ? r->request_body->len : 0;
        char *query = archive_request_body_query(r->request_content_type, body, body_len, FALSE);
        char *full  = archive_request_body_query(r->request_content_type, body, body_len, TRUE);
        archive_json_append_member(json, "method", r->request_method);
        /* Present even when empty, as cdxj-indexer writes it. */
        archive_json_append_member(json, "requestBody", g_utf8_validate(query, -1, NULL) ? query : "");
        /* And whole, where the key had to cut it: captures alike up to the
         * cut share a key, and replay tells them apart by the rest. */
        if (strlen(full) > strlen(query) && g_utf8_validate(full, -1, NULL)) {
            archive_json_append_member(json, "requestBodyFull", full);
        }
        g_free(query);
        g_free(full);
    }
    g_string_append_c(json, '}');

    char *timestamp = archive_index_timestamp(r->date);
    g_ptr_array_add(lines->index->lines, g_strdup_printf("%s %s %s", key, timestamp, json->str));
    g_free(timestamp);
    g_string_free(json, TRUE);
    g_free(key);

    if (r->navigation) {
        ArchiveNavigation *nav = g_new0(ArchiveNavigation, 1);
        nav->url  = g_strdup(r->url);
        nav->date = g_strdup(r->date);
        g_ptr_array_add(lines->index->navigations, nav);
    }
}

static void lines_add(Lines *lines, Record *r)
{
    if (lines->held && join(lines->held, r)) {
        write_line(lines, lines->held);
        write_line(lines, r);
        record_free(lines->held);
        record_free(r);
        lines->held = NULL;
        return;
    }
    if (lines->held) {
        write_line(lines, lines->held);
        record_free(lines->held);
    }
    lines->held = r;
}

static void lines_finish(Lines *lines)
{
    if (lines->held) {
        write_line(lines, lines->held);
        record_free(lines->held);
        lines->held = NULL;
    }
}

static void navigation_free(gpointer data)
{
    ArchiveNavigation *nav = data;
    g_free(nav->url);
    g_free(nav->date);
    g_free(nav);
}

ArchiveWarcIndex *archive_warc_index_new(void)
{
    ArchiveWarcIndex *index = g_new0(ArchiveWarcIndex, 1);
    index->lines       = g_ptr_array_new_with_free_func(g_free);
    index->navigations = g_ptr_array_new_with_free_func(navigation_free);
    return index;
}

void archive_warc_index_free(ArchiveWarcIndex *index)
{
    if (!index) {
        return;
    }
    g_ptr_array_unref(index->lines);
    g_ptr_array_unref(index->navigations);
    g_free(index);
}

static char *sha256_label(GChecksum *sum)
{
    return g_strconcat("sha256:", g_checksum_get_string(sum), NULL);
}

gboolean archive_index_warc(const char *path, const char *filename,
        ArchiveWarcIndex *index, GError **error)
{
    int fd = g_open(path, O_RDONLY, 0);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not open %s: %s", path, g_strerror(errno));
        return FALSE;
    }

    Lines lines     = {filename, index, NULL};
    z_stream zs     = {0};
    guint8 *input   = g_malloc(READ_SIZE);
    guint8 *output  = g_malloc(READ_SIZE);
    GString *start  = g_string_new(NULL);
    GChecksum *sum  = g_checksum_new(G_CHECKSUM_SHA256);
    guint64 member  = 0;            /* where the current member began */
    gsize input_len = 0;
    gboolean eof    = FALSE;
    gboolean ok     = TRUE;

    index->complete_length = 0;
    inflateInit2(&zs, 15 + 16);
    for (;;) {
        if (zs.avail_in == 0 && !eof) {
            ssize_t n;
            do {
                n = read(fd, input, READ_SIZE);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Could not read %s: %s", path, g_strerror(errno));
                ok = FALSE;
                break;
            }
            eof         = n == 0;
            input_len   = (gsize)n;
            zs.next_in  = input;
            zs.avail_in = (uInt)n;
        }
        gsize used_before = input_len - zs.avail_in;
        zs.next_out  = output;
        zs.avail_out = READ_SIZE;
        int ret = inflate(&zs, Z_NO_FLUSH);

        /* One call never runs past the end of a member. */
        g_checksum_update(sum, input + used_before, (gssize)(input_len - zs.avail_in - used_before));
        gsize produced = READ_SIZE - zs.avail_out;
        if (start->len < MAX_RECORD_START) {
            g_string_append_len(start, (const char *)output,
                    (gssize)MIN(produced, MAX_RECORD_START - start->len));
        }

        if (ret == Z_STREAM_END) {
            guint64 length = zs.total_in;
            Record *r = parse_record(start->str, start->len, member, length, sha256_label(sum));
            if (r) {
                lines_add(&lines, r);
            }
            member                += length;
            index->complete_length = member;
            g_string_truncate(start, 0);
            g_checksum_reset(sum);
            if (inflateReset(&zs) != Z_OK) {
                break;
            }
            continue;
        }
        /* Out of input with a member unfinished, or bytes that aren't gzip:
         * nothing past the last whole member is a record. */
        if ((ret == Z_BUF_ERROR && (eof || zs.avail_in)) || (ret != Z_OK && ret != Z_BUF_ERROR)) {
            break;
        }
    }
    lines_finish(&lines);
    inflateEnd(&zs);
    close(fd);
    g_checksum_free(sum);
    g_string_free(start, TRUE);
    g_free(input);
    g_free(output);
    return ok;
}
