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
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "replay.h"
#include "request_key.h"
#include "zip.h"

#define INDEX_MAX   ((gsize)2 << 30)        /* an index, uncompressed */
#define SMALL_MAX   ((gsize)64 << 20)       /* a page list, a datapackage */
#define RECORD_MAX  ((gsize)2 << 30)        /* a record */
#define HEAD_MAX    (1 << 20)               /* a recorded response's head */

typedef struct {
    char   *line;           /* owned; key, ts and json point into it */
    char   *key;
    char   *ts;
    char   *json;
} Entry;

/* What a lookup needs of an entry's JSON. */
typedef struct {
    guint   status;
    char    *mime;
    char    *url;
    char    *filename;
    guint64 offset;
    guint64 length;
} EntryInfo;

struct ArchiveReplay {
    ArchiveZipReader *zip;
    char             *title;
    GPtrArray        *entries;          /* of Entry *, by key, then time */
    GPtrArray        *pages;            /* of ArchiveReplayPage * */
    GMutex           lock;              /* for what follows */
    GHashTable       *moments;          /* URL -> when what answered it was captured */
    GHashTable       *origin_moments;   /* origin -> when its last page was */
    gint64           last_page;         /* when the last page answered was; 0: none */
    guint            served;
    guint            missed;
};

/* ---- times -------------------------------------------------------------- */

char *archive_replay_index_time(const char *time)
{
    GString *digits = g_string_new(NULL);
    for (const char *p = time ? time : ""; *p && digits->len < 14; p++) {
        if (g_ascii_isdigit(*p)) {
            g_string_append_c(digits, *p);
        } else if (*p == '.' || *p == 'Z' || *p == '+') {
            break;                  /* fractions and zones aren't the index's */
        }
    }
    return g_string_free(digits, FALSE);
}

/* An index time in seconds since 1970; 0 if it isn't one. */
static gint64 seconds_of(const char *ts)
{
    int f[6] = {0, 1, 1, 0, 0, 0};
    static const int widths[6] = {4, 2, 2, 2, 2, 2};
    const char *p = ts;
    for (int i = 0; i < 6 && p && *p; i++) {
        int v = 0;
        for (int w = 0; w < widths[i]; w++, p++) {
            if (!g_ascii_isdigit(*p)) {
                return 0;
            }
            v = v * 10 + (*p - '0');
        }
        f[i] = v;
    }
    GDateTime *t = g_date_time_new_utc(f[0], f[1], f[2], f[3], f[4], f[5]);
    gint64 s = t ? g_date_time_to_unix(t) : 0;
    if (t) {
        g_date_time_unref(t);
    }
    return s;
}

/* ---- reading ------------------------------------------------------------ */

/* Inflates gzip data -- every member of it, one after another. */
static GByteArray *gunzip(const guint8 *data, gsize len, gsize max, GError **error)
{
    z_stream z = {0};
    guint8 buf[65536];
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "zlib failed");
        return NULL;
    }
    GByteArray *out = g_byte_array_new();
    z.next_in  = (Bytef *)data;
    z.avail_in = (uInt)len;
    for (;;) {
        z.next_out  = buf;
        z.avail_out = sizeof(buf);
        int rc = inflate(&z, Z_NO_FLUSH);
        g_byte_array_append(out, buf, sizeof(buf) - z.avail_out);
        if (out->len > max) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "too large");
            break;
        }
        if (rc == Z_STREAM_END) {
            if (!z.avail_in) {
                inflateEnd(&z);
                return out;
            }
            inflateReset(&z);       /* the next member */
        } else if (rc != Z_OK && !(rc == Z_BUF_ERROR && z.avail_out == 0)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s",
                    z.avail_in || rc != Z_BUF_ERROR ? "not gzip" : "cut short");
            break;
        }
    }
    inflateEnd(&z);
    g_byte_array_unref(out);
    return NULL;
}

static GBytes *read_entry(ArchiveReplay *r, const char *name, gsize max, GError **error)
{
    const ArchiveZipEntry *e = archive_zip_find(r->zip, name);
    if (!e) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "the archive holds no %s", name);
        return NULL;
    }
    return archive_zip_read_entry(r->zip, e, max, error);
}

static int compare_entries(gconstpointer a, gconstpointer b)
{
    const Entry *x = *(Entry * const *)a, *y = *(Entry * const *)b;
    int c = strcmp(x->key, y->key);
    return c ? c : strcmp(x->ts, y->ts);
}

static void entry_free(Entry *e)
{
    g_free(e->line);
    g_free(e);
}

static gboolean load_index(ArchiveReplay *r, GError **error)
{
    static const char *names[] = {"indexes/index.cdx.gz", "indexes/index.cdxj.gz",
        "indexes/index.cdx", "indexes/index.cdxj", NULL};
    GBytes *raw = NULL;
    const char *name = NULL;
    for (int i = 0; names[i] && !raw; i++) {
        if (archive_zip_find(r->zip, names[i])) {
            name = names[i];
            raw  = read_entry(r, name, INDEX_MAX, error);
            if (!raw) {
                return FALSE;
            }
        }
    }
    if (!raw) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "the archive has no index (indexes/index.cdx.gz)");
        return FALSE;
    }
    gsize size;
    const guint8 *data = g_bytes_get_data(raw, &size);
    GByteArray *text = NULL;
    if (g_str_has_suffix(name, ".gz")) {
        text = gunzip(data, size, INDEX_MAX, error);
        if (!text) {
            g_bytes_unref(raw);
            g_prefix_error(error, "%s: ", name);
            return FALSE;
        }
    } else {
        text = g_byte_array_sized_new(size);
        g_byte_array_append(text, data, size);
    }
    g_bytes_unref(raw);
    g_byte_array_append(text, (const guint8 *)"", 1);

    gboolean sorted = TRUE;
    char *save = NULL;
    for (char *line = strtok_r((char *)text->data, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *sp1 = strchr(line, ' ');
        char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp2 || sp2[1] != '{') {
            continue;               /* not CDXJ: a CDX header, say */
        }
        Entry *e = g_new0(Entry, 1);
        e->line  = g_strdup(line);
        e->key   = e->line;
        e->ts    = e->line + (sp1 - line) + 1;
        e->json  = e->line + (sp2 - line) + 1;
        e->ts[-1]   = '\0';
        e->json[-1] = '\0';
        if (r->entries->len && compare_entries(&g_ptr_array_index(r->entries,
                        r->entries->len - 1), &e) > 0) {
            sorted = FALSE;
        }
        g_ptr_array_add(r->entries, e);
    }
    g_byte_array_unref(text);
    if (!sorted) {
        g_ptr_array_sort(r->entries, compare_entries);
    }
    return TRUE;
}

static void page_free(ArchiveReplayPage *p)
{
    g_free(p->url);
    g_free(p->ts);
    g_free(p->title);
    g_free(p);
}

static void load_pages(ArchiveReplay *r)
{
    GBytes *raw = read_entry(r, "pages/pages.jsonl", SMALL_MAX, NULL);
    if (!raw) {
        return;                     /* a list is a help, not a need */
    }
    gsize size;
    const char *data = g_bytes_get_data(raw, &size);
    char *text = g_strndup(data, size);
    char **lines = g_strsplit(text, "\n", -1);
    JsonParser *parser = json_parser_new();
    for (char **l = lines; *l; l++) {
        if (!**l || !json_parser_load_from_data(parser, *l, -1, NULL)
                || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            continue;
        }
        JsonObject *o = json_node_get_object(json_parser_get_root(parser));
        const char *url = json_object_get_string_member_with_default(o, "url", NULL);
        if (!url) {
            continue;               /* the header line */
        }
        ArchiveReplayPage *p = g_new0(ArchiveReplayPage, 1);
        p->url   = g_strdup(url);
        p->ts    = archive_replay_index_time(json_object_get_string_member_with_default(o, "ts", ""));
        p->title = g_strdup(json_object_get_string_member_with_default(o, "title", ""));
        g_ptr_array_add(r->pages, p);
    }
    g_object_unref(parser);
    g_strfreev(lines);
    g_free(text);
    g_bytes_unref(raw);
}

static void load_title(ArchiveReplay *r, const char *path)
{
    GBytes *raw = read_entry(r, "datapackage.json", SMALL_MAX, NULL);
    if (raw) {
        gsize size;
        const char *data = g_bytes_get_data(raw, &size);
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, data, size, NULL)
                && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            const char *t = json_object_get_string_member_with_default(
                    json_node_get_object(json_parser_get_root(parser)), "title", NULL);
            r->title = t && *t ? g_strdup(t) : NULL;
        }
        g_object_unref(parser);
        g_bytes_unref(raw);
    }
    if (!r->title) {
        char *base = g_path_get_basename(path);
        if (g_str_has_suffix(base, ".wacz")) {
            base[strlen(base) - 5] = '\0';
        }
        r->title = base;
    }
}

ArchiveReplay *archive_replay_open(const char *wacz_path, GError **error)
{
    ArchiveReplay *r = g_new0(ArchiveReplay, 1);
    g_mutex_init(&r->lock);
    r->entries = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
    r->pages   = g_ptr_array_new_with_free_func((GDestroyNotify)page_free);
    r->moments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    r->origin_moments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    r->zip     = archive_zip_reader_open(wacz_path, error);
    if (!r->zip || !load_index(r, error)) {
        archive_replay_free(r);
        return NULL;
    }
    load_pages(r);
    load_title(r, wacz_path);
    return r;
}

void archive_replay_free(ArchiveReplay *r)
{
    if (!r) {
        return;
    }
    if (r->zip) {
        archive_zip_reader_free(r->zip);
    }
    g_ptr_array_unref(r->entries);
    g_ptr_array_unref(r->pages);
    g_hash_table_unref(r->moments);
    g_hash_table_unref(r->origin_moments);
    g_free(r->title);
    g_mutex_clear(&r->lock);
    g_free(r);
}

const GPtrArray *archive_replay_pages(ArchiveReplay *r)
{
    return r->pages;
}

const char *archive_replay_title(ArchiveReplay *r)
{
    return r->title;
}

void archive_replay_counts(ArchiveReplay *r, guint *served, guint *missed)
{
    g_mutex_lock(&r->lock);
    *served = r->served;
    *missed = r->missed;
    g_mutex_unlock(&r->lock);
}

/* ---- finding ------------------------------------------------------------ */

/* The first entry whose key is not less than key (or, with prefix, that
 * doesn't sort before keys starting with it). */
static guint lower_bound(ArchiveReplay *r, const char *key)
{
    guint lo = 0, hi = r->entries->len;
    while (lo < hi) {
        guint mid = lo + (hi - lo) / 2;
        Entry *e  = g_ptr_array_index(r->entries, mid);
        if (strcmp(e->key, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void info_clear(EntryInfo *info)
{
    g_free(info->mime);
    g_free(info->url);
    g_free(info->filename);
}

static gboolean info_of(const Entry *e, EntryInfo *info)
{
    memset(info, 0, sizeof(*info));
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, e->json, -1, NULL)
        && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser));
    if (ok) {
        JsonObject *o = json_node_get_object(json_parser_get_root(parser));
        const char *status = json_object_get_string_member_with_default(o, "status", "0");
        info->status   = (guint)g_ascii_strtoull(status, NULL, 10);
        info->mime     = g_strdup(json_object_get_string_member_with_default(o, "mime", ""));
        info->url      = g_strdup(json_object_get_string_member_with_default(o, "url", ""));
        info->filename = g_strdup(json_object_get_string_member_with_default(o, "filename", ""));
        info->offset   = g_ascii_strtoull(json_object_get_string_member_with_default(o, "offset",
                    "0"), NULL, 10);
        info->length   = g_ascii_strtoull(json_object_get_string_member_with_default(o, "length",
                    "0"), NULL, 10);
    }
    g_object_unref(parser);
    return ok;
}

static gboolean refused(guint status)
{
    return status == 401 || status == 403 || status == 429;
}

/* Of the captures of key, the one to answer with: a whole one before part
 * of one, for a page one the site let in before one it refused, and of
 * those, the nearest moment (the earliest, with no moment). Revisits wait
 * for a reader that can follow them. */
static const Entry *best_capture(ArchiveReplay *r, const char *key, gint64 moment, gboolean page)
{
    const Entry *best = NULL;
    int best_rank     = G_MAXINT;
    gint64 best_far   = G_MAXINT64;
    for (guint i = lower_bound(r, key); i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strcmp(e->key, key)) {
            break;
        }
        EntryInfo info;
        if (!info_of(e, &info)) {
            continue;
        }
        if (strcmp(info.mime, "warc/revisit")) {
            int rank   = (info.status == 206 ? 2 : 0) + (page && refused(info.status) ? 1 : 0);
            gint64 at  = seconds_of(e->ts);
            gint64 far = moment ? llabs(at - moment) : at;
            if (rank < best_rank || (rank == best_rank && far < best_far)) {
                best      = e;
                best_rank = rank;
                best_far  = far;
            }
        }
        info_clear(&info);
    }
    return best;
}

/* Whether any key starts with prefix, followed by what follow allows (any
 * character, if NULL). */
static gboolean any_key_with(ArchiveReplay *r, const char *prefix, const char *follow)
{
    gsize n = strlen(prefix);
    for (guint i = lower_bound(r, prefix); i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strncmp(e->key, prefix, n)) {
            return FALSE;
        }
        if (!follow || strchr(follow, e->key[n])) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Why nothing answers key, from what the index holds of its host and path. */
static const char *why_missing(ArchiveReplay *r, const char *key)
{
    const char *close = strchr(key, ')');
    char *host = close ? g_strndup(key, close - key + 1) : g_strdup(key);
    const char *query = strchr(key, '?');
    char *path = query ? g_strndup(key, query - key) : g_strdup(key);
    const char *why;
    if (!any_key_with(r, host, NULL)) {
        why = "Nothing was archived from this site.";
    } else if (!any_key_with(r, path, "?")) {
        why = any_key_with(r, path, "")
            ? "This address was archived, but not with this query, or not by this method."
            : "Nothing was archived at this address.";
    } else {
        why = "This address was archived, but not with this query, or not by this method.";
    }
    g_free(host);
    g_free(path);
    return why;
}

/* ---- moments ------------------------------------------------------------ */

static char *without_fragment(const char *url)
{
    const char *hash = strchr(url, '#');
    return hash ? g_strndup(url, hash - url) : g_strdup(url);
}

static char *origin_of(const char *url)
{
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
    char *origin = NULL;
    if (uri && g_uri_get_host(uri)) {
        int port = g_uri_get_port(uri);
        origin = port > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri),
                g_uri_get_host(uri), port)
            : g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
    }
    if (uri) {
        g_uri_unref(uri);
    }
    return origin;
}

/* Whether a request is for a page or a frame -- something navigated to --
 * rather than what one loads. */
static gboolean is_page(const Http1Head *head)
{
    const char *dest = http1_head_get(head, "Sec-Fetch-Dest");
    if (dest) {
        return !strcmp(dest, "document") || !strcmp(dest, "iframe") || !strcmp(dest, "frame");
    }
    const char *mode = http1_head_get(head, "Sec-Fetch-Mode");
    if (mode) {
        return !strcmp(mode, "navigate") || !strcmp(mode, "nested-navigate");
    }
    const char *accept = http1_head_get(head, "Accept");
    return accept && g_str_has_prefix(accept, "text/html");
}

/* The moment to answer a request at: a page's own, from the page list;
 * else that of what asked for it (the Referer), or of its site's last page;
 * else the last page's. Called locked. */
static gint64 moment_for(ArchiveReplay *r, const char *url, const Http1Head *head, gboolean page)
{
    if (page) {
        for (guint i = 0; i < r->pages->len; i++) {
            ArchiveReplayPage *p = g_ptr_array_index(r->pages, i);
            char *listed = without_fragment(p->url);
            gboolean same = !strcmp(listed, url);
            g_free(listed);
            if (same) {
                return seconds_of(p->ts);
            }
        }
    }
    const char *referer = http1_head_get(head, "Referer");
    if (referer) {
        char *from = without_fragment(referer);
        gpointer at = g_hash_table_lookup(r->moments, from);
        g_free(from);
        if (at) {
            return GPOINTER_TO_SIZE(at);
        }
        char *origin = origin_of(referer);
        at = origin ? g_hash_table_lookup(r->origin_moments, origin) : NULL;
        g_free(origin);
        if (at) {
            return GPOINTER_TO_SIZE(at);
        }
    }
    return r->last_page;
}

/* ---- answering ---------------------------------------------------------- */

static gboolean hop_by_hop(const char *name)
{
    static const char *names[] = {"Connection", "Keep-Alive", "Proxy-Connection",
        "Proxy-Authenticate", "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding",
        "Upgrade", "Content-Length",
        /* nor what would send the browser elsewhere than the archive */
        "Alt-Svc", "Strict-Transport-Security", NULL};
    for (int i = 0; names[i]; i++) {
        if (!g_ascii_strcasecmp(name, names[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static void append_payload(const guint8 *data, gsize len, gpointer user)
{
    g_byte_array_append(user, data, len);
}

/* The range a request asks for, of a body total bytes long: a single range
 * only; FALSE for any other, or none. */
static gboolean asked_range(const Http1Head *head, gsize total, gsize *from, gsize *to)
{
    const char *range = http1_head_get(head, "Range");
    if (!range || !g_str_has_prefix(range, "bytes=") || strchr(range, ',') || !total) {
        return FALSE;
    }
    const char *spec = range + 6;
    char *end;
    if (*spec == '-') {                 /* the last n bytes */
        guint64 n = g_ascii_strtoull(spec + 1, &end, 10);
        if (!n) {
            return FALSE;
        }
        *from = n >= total ? 0 : total - n;
        *to   = total - 1;
        return TRUE;
    }
    guint64 a = g_ascii_strtoull(spec, &end, 10);
    if (end == spec || *end != '-' || a >= total) {
        return FALSE;
    }
    guint64 b = end[1] ? g_ascii_strtoull(end + 1, NULL, 10) : total - 1;
    *from = a;
    *to   = MIN(b, total - 1);
    return *to >= *from;
}

static char *http_date(gint64 seconds)
{
    GDateTime *t = g_date_time_new_from_unix_utc(seconds);
    char *s = g_date_time_format(t, "%a, %d %b %Y %H:%M:%S GMT");
    g_date_time_unref(t);
    return s;
}

/* A recorded response, made into one for the browser. */
static GBytes *answer_from(ArchiveReplay *r, const Entry *e, const EntryInfo *info,
        const ArchiveReplayRequest *request, GError **error)
{
    char *name = g_strconcat("archive/", info->filename, NULL);
    const ArchiveZipEntry *ze = archive_zip_find(r->zip, name);
    g_free(name);
    if (!ze || !info->length || info->length > RECORD_MAX) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no record for %s", info->url);
        return NULL;
    }
    guint8 *raw = g_malloc(info->length);
    if (!archive_zip_read_at(r->zip, ze, info->offset, raw, info->length, error)) {
        g_free(raw);
        return NULL;
    }
    GByteArray *record;
    if (g_str_has_suffix(info->filename, ".gz")) {
        record = gunzip(raw, info->length, RECORD_MAX, error);
        g_free(raw);
        if (!record) {
            return NULL;
        }
    } else {
        record = g_byte_array_new_take(raw, info->length);
    }

    /* the record: its WARC headers, then its block */
    const char *start = (const char *)record->data;
    const char *sep   = g_strstr_len(start, MIN(record->len, HEAD_MAX), "\r\n\r\n");
    if (!sep) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a WARC record");
        g_byte_array_unref(record);
        return NULL;
    }
    char *warc_head = g_strndup(start, sep - start);
    char *type = NULL, *content_type = NULL;
    guint64 block_len = 0;
    char **lines = g_strsplit(warc_head, "\r\n", -1);
    for (char **l = lines; *l; l++) {
        char *colon = strchr(*l, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        const char *value = g_strstrip(colon + 1);
        if (!g_ascii_strcasecmp(*l, "WARC-Type")) {
            type = g_strdup(value);
        } else if (!g_ascii_strcasecmp(*l, "Content-Type")) {
            content_type = g_strdup(value);
        } else if (!g_ascii_strcasecmp(*l, "Content-Length")) {
            block_len = g_ascii_strtoull(value, NULL, 10);
        }
    }
    g_strfreev(lines);
    g_free(warc_head);
    const guint8 *block = (const guint8 *)sep + 4;
    gsize avail = record->len - (block - record->data);
    gsize len   = MIN(block_len ? block_len : avail, avail);

    guint status = 200;
    char *reason = g_strdup("OK");
    GPtrArray *headers = g_ptr_array_new_with_free_func(g_free);
    GByteArray *body = g_byte_array_new();
    gboolean ok = TRUE;
    if (!g_strcmp0(type, "response")) {
        Http1Parser *p = http1_parser_new(HTTP1_RESPONSE, HEAD_MAX);
        http1_parser_set_request_method(p, "GET");
        http1_parser_set_payload_func(p, append_payload, body);
        GError *perr = NULL;
        gssize used = http1_parser_feed(p, block, len, &perr);
        if (used >= 0 && !http1_parser_is_done(p)) {
            http1_parser_eof(p, NULL);      /* a body cut short is served as held */
        }
        if (!http1_parser_has_head(p)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "a response without a head: %s",
                    perr ? perr->message : "");
            ok = FALSE;
        } else {
            const Http1Head *h = http1_parser_head(p);
            status = h->status;
            g_free(reason);
            reason = g_strdup(h->reason ? h->reason : "");
            for (guint i = 0; i < h->headers->len; i++) {
                Http1Header *hd = &g_array_index(h->headers, Http1Header, i);
                if (!hop_by_hop(hd->name)) {
                    g_ptr_array_add(headers, g_strdup_printf("%s: %s", hd->name, hd->value));
                }
            }
        }
        g_clear_error(&perr);
        http1_parser_free(p);
    } else if (!g_strcmp0(type, "resource")) {
        g_ptr_array_add(headers, g_strdup_printf("Content-Type: %s",
                    content_type ? content_type : "application/octet-stream"));
        g_byte_array_append(body, block, len);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "a %s record can't answer",
                type ? type : "untyped");
        ok = FALSE;
    }
    g_free(type);
    g_free(content_type);
    g_byte_array_unref(record);

    GBytes *answer = NULL;
    if (ok) {
        gboolean head_only = !g_ascii_strcasecmp(request->method, "HEAD");
        gsize from = 0, to = body->len ? body->len - 1 : 0;
        gboolean ranged = status == 200 && asked_range(request->head, body->len, &from, &to);
        GString *out = g_string_new(NULL);
        if (ranged) {
            g_string_append(out, "HTTP/1.1 206 Partial Content\r\n");
        } else {
            g_string_append_printf(out, "HTTP/1.1 %u %s\r\n", status, reason);
        }
        for (guint i = 0; i < headers->len; i++) {
            g_string_append_printf(out, "%s\r\n", (char *)g_ptr_array_index(headers, i));
        }
        if (ranged) {
            g_string_append_printf(out, "Content-Range: bytes %" G_GSIZE_FORMAT "-%"
                    G_GSIZE_FORMAT "/%u\r\n", from, to, body->len);
        }
        gsize length = ranged ? to - from + 1 : body->len;
        char *when = http_date(seconds_of(e->ts));
        g_string_append_printf(out, "Content-Length: %" G_GSIZE_FORMAT "\r\n"
                "Memento-Datetime: %s\r\n\r\n", length, when);
        g_free(when);
        if (!head_only && status != 204 && status != 304 && status >= 200) {
            g_string_append_len(out, (const char *)body->data + (ranged ? from : 0), length);
        }
        gsize size = out->len;
        answer = g_bytes_new_take(g_string_free(out, FALSE), size);
    }
    g_free(reason);
    g_ptr_array_unref(headers);
    g_byte_array_unref(body);
    return answer;
}

static GBytes *simple_answer(guint status, const char *reason, const char *type,
        const char *body, const char *extra_header)
{
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
            "%s%sContent-Length: %" G_GSIZE_FORMAT "\r\n\r\n%s", status, reason, type,
            extra_header ? extra_header : "", extra_header ? "\r\n" : "", strlen(body), body);
    gsize size = out->len;
    return g_bytes_new_take(g_string_free(out, FALSE), size);
}

static GBytes *missing_answer(const char *url, const char *why, gboolean page)
{
    char *header = g_strdup_printf("X-Archive-Missing: %s", why);
    GBytes *answer;
    if (page) {
        char *u = g_markup_escape_text(url, -1);
        char *w = g_markup_escape_text(why, -1);
        char *html = g_strdup_printf("<!doctype html><meta charset=utf-8>"
                "<title>Not in the archive</title>"
                "<style>body{font:16px/1.5 sans-serif;margin:3em auto;max-width:40em;padding:0 1em}"
                "code{word-break:break-all}</style>"
                "<h1>Not in the archive</h1><p><code>%s</code></p><p>%s</p>"
                "<p><a href=\"" ARCHIVE_REPLAY_HOME "\">What the archive holds</a></p>", u, w);
        answer = simple_answer(404, "Not Found", "text/html; charset=utf-8", html, header);
        g_free(html);
        g_free(w);
        g_free(u);
    } else {
        answer = simple_answer(404, "Not Found", "text/plain; charset=utf-8", why, header);
    }
    g_free(header);
    return answer;
}

/* The list of what the archive holds: replay's home. */
static GBytes *home_answer(ArchiveReplay *r)
{
    GString *html = g_string_new("<!doctype html><meta charset=utf-8>");
    char *title = g_markup_escape_text(r->title, -1);
    g_string_append_printf(html, "<title>%s</title><style>body{font:16px/1.5 sans-serif;"
            "margin:2em auto;max-width:50em;padding:0 1em}li{margin:.5em 0}"
            "small{color:#666;word-break:break-all}</style><h1>%s</h1>", title, title);
    g_free(title);
    if (!r->pages->len) {
        g_string_append(html, "<p>The archive lists no pages.</p>");
    } else {
        g_string_append(html, "<ol>");
        for (guint i = 0; i < r->pages->len; i++) {
            ArchiveReplayPage *p = g_ptr_array_index(r->pages, i);
            char *url   = g_markup_escape_text(p->url, -1);
            char *label = g_markup_escape_text(*p->title ? p->title : p->url, -1);
            GDateTime *t = g_date_time_new_from_unix_utc(seconds_of(p->ts));
            char *when  = g_date_time_format(t, "%Y-%m-%d %H:%M UTC");
            g_date_time_unref(t);
            g_string_append_printf(html, "<li><a href=\"%s\">%s</a><br><small>%s &middot; %s"
                    "</small>", url, label, url, when);
            g_free(when);
            g_free(label);
            g_free(url);
        }
        g_string_append(html, "</ol>");
    }
    GBytes *answer = simple_answer(200, "OK", "text/html; charset=utf-8", html->str, NULL);
    g_string_free(html, TRUE);
    return answer;
}

GBytes *archive_replay_answer(ArchiveReplay *r, const ArchiveReplayRequest *request,
        gboolean *found)
{
    char *url = without_fragment(request->url);
    *found = TRUE;
    if (g_str_has_prefix(url, ARCHIVE_REPLAY_HOME)) {
        g_free(url);
        return home_answer(r);
    }
    gboolean page = is_page(request->head);
    const char *type = http1_head_get(request->head, "Content-Type");

    g_mutex_lock(&r->lock);
    gint64 moment = moment_for(r, url, request->head, page);
    g_mutex_unlock(&r->lock);

    char *key = archive_request_key(url, request->method, type, request->body,
            (gssize)request->body_len);
    const Entry *e = key ? best_capture(r, key, moment, page) : NULL;
    if (!e && key && !g_ascii_strcasecmp(request->method, "HEAD")) {
        /* a HEAD the capture never made: a GET's headers are what it'd say */
        g_free(key);
        key = archive_request_key(url, "GET", NULL, NULL, 0);
        e   = key ? best_capture(r, key, moment, page) : NULL;
    }

    GBytes *answer = NULL;
    GError *error  = NULL;
    if (e) {
        EntryInfo info;
        if (info_of(e, &info)) {
            answer = answer_from(r, e, &info, request, &error);
            info_clear(&info);
        }
    }
    g_mutex_lock(&r->lock);
    if (answer) {
        gint64 at = seconds_of(e->ts);
        r->served++;
        g_hash_table_replace(r->moments, g_strdup(url), GSIZE_TO_POINTER(at));
        if (page) {
            char *origin = origin_of(url);
            if (origin) {
                g_hash_table_replace(r->origin_moments, origin, GSIZE_TO_POINTER(at));
            }
            r->last_page = at;
        }
    } else {
        r->missed++;
    }
    g_mutex_unlock(&r->lock);

    if (!answer) {
        *found = FALSE;
        const char *why = error ? "The archive lists this, but its record can't be read."
            : key ? why_missing(r, key) : "This isn't an address the archive could hold.";
        answer = missing_answer(url, why, page);
        if (error) {
            g_debug("replay: %s: %s", url, error->message);
        }
    }
    g_clear_error(&error);
    g_free(key);
    g_free(url);
    return answer;
}
