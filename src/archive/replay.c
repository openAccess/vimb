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
#include "surt.h"
#include "websocket.h"
#include "zip.h"

#define INDEX_MAX   ((gsize)2 << 30)        /* an index, uncompressed */
#define SMALL_MAX   ((gsize)64 << 20)       /* a page list, a datapackage */
#define RECORD_MAX  ((gsize)2 << 30)        /* a record */
#define HEAD_MAX    (1 << 20)               /* a recorded response's head */

typedef struct {
    char    *line;          /* owned; key, ts and json point into it */
    char    *key;
    char    *ts;
    char    *json;
    /* what lookups need of the JSON, read once */
    char    *url;
    char    *mime;
    char    *digest;
    char    *filename;
    guint64 offset;
    guint64 length;
    guint   status;
    gint64  at;             /* ts, in seconds */
    char    *request_body;      /* the body query an index records, cut */
    char    *request_body_full; /* and whole, where that was cut */
    char    **fields;           /* a body's: its fields, and its URL's query's; sorted */
} Entry;

struct ArchiveReplay {
    ArchiveZipReader *zip;
    char             *title;
    GPtrArray        *entries;          /* of Entry *, by key, then time */
    GPtrArray        *pages;            /* of ArchiveReplayPage * */
    GMutex           lock;              /* for what follows */
    GHashTable       *moments;          /* URL -> when what answered it was captured */
    GHashTable       *origin_moments;   /* origin -> when its last page was */
    GHashTable       *streams;          /* a stream's identity -> StreamPosition */
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
    g_free(e->url);
    g_free(e->mime);
    g_free(e->digest);
    g_free(e->filename);
    g_free(e->request_body);
    g_free(e->request_body_full);
    g_strfreev(e->fields);
    g_free(e);
}

/* A JSON string, from just after its opening quote; NULL if it isn't one. */
static char *json_string(const char **p)
{
    GString *s = g_string_new(NULL);
    const char *c = *p;
    while (*c && *c != '"') {
        if (*c != '\\') {
            g_string_append_c(s, *c++);
            continue;
        }
        c++;
        switch (*c) {
            case 'n': g_string_append_c(s, '\n'); c++; break;
            case 't': g_string_append_c(s, '\t'); c++; break;
            case 'r': g_string_append_c(s, '\r'); c++; break;
            case 'b': g_string_append_c(s, '\b'); c++; break;
            case 'f': g_string_append_c(s, '\f'); c++; break;
            case 'u': {
                gunichar u = 0;
                for (int i = 1; i <= 4; i++) {
                    if (!g_ascii_isxdigit(c[i])) {
                        g_string_free(s, TRUE);
                        return NULL;
                    }
                    u = u * 16 + g_ascii_xdigit_value(c[i]);
                }
                c += 5;
                if (u >= 0xd800 && u < 0xdc00 && c[0] == '\\' && c[1] == 'u') {
                    gunichar low = 0;
                    gboolean hex = TRUE;
                    for (int i = 2; i <= 5 && hex; i++) {
                        hex = g_ascii_isxdigit(c[i]);
                        low = hex ? low * 16 + g_ascii_xdigit_value(c[i]) : 0;
                    }
                    if (hex && low >= 0xdc00 && low < 0xe000) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                        c += 6;
                    }
                }
                g_string_append_unichar(s, u);
                break;
            }
            case '\0':
                g_string_free(s, TRUE);
                return NULL;
            default:                /* \" \\ \/ */
                g_string_append_c(s, *c++);
        }
    }
    if (*c != '"') {
        g_string_free(s, TRUE);
        return NULL;
    }
    *p = c + 1;
    return g_string_free(s, FALSE);
}

static void take_field(Entry *e, const char *name, char *value)
{
    char **field = !strcmp(name, "url") ? &e->url : !strcmp(name, "mime") ? &e->mime
        : !strcmp(name, "digest") ? &e->digest : !strcmp(name, "filename") ? &e->filename
        : !strcmp(name, "requestBody") ? &e->request_body
        : !strcmp(name, "requestBodyFull") ? &e->request_body_full : NULL;
    if (field) {
        g_free(*field);
        *field = value;
        return;
    }
    if (!strcmp(name, "status")) {
        e->status = (guint)g_ascii_strtoull(value, NULL, 10);
    } else if (!strcmp(name, "offset")) {
        e->offset = g_ascii_strtoull(value, NULL, 10);
    } else if (!strcmp(name, "length")) {
        e->length = g_ascii_strtoull(value, NULL, 10);
    }
    g_free(value);
}

/* Reads the flat object of strings and numbers an index line ends with, as
 * indexers write it -- faster than a JSON parser, read for every line. */
static gboolean read_flat_json(Entry *e)
{
    const char *p = e->json;
    if (*p++ != '{') {
        return FALSE;
    }
    for (;;) {
        while (g_ascii_isspace(*p)) {
            p++;
        }
        if (*p == '}') {
            return TRUE;
        }
        if (*p++ != '"') {
            return FALSE;
        }
        char *name = json_string(&p);
        while (name && g_ascii_isspace(*p)) {
            p++;
        }
        if (!name || *p++ != ':') {
            g_free(name);
            return FALSE;
        }
        while (g_ascii_isspace(*p)) {
            p++;
        }
        char *value = NULL;
        if (*p == '"') {
            p++;
            value = json_string(&p);
        } else if (*p != '{' && *p != '[') {
            const char *v = p;
            while (*p && *p != ',' && *p != '}' && !g_ascii_isspace(*p)) {
                p++;
            }
            value = g_strndup(v, p - v);
        }
        if (!value) {
            g_free(name);
            return FALSE;           /* nested: the slow way */
        }
        take_field(e, name, value);
        g_free(name);
        while (g_ascii_isspace(*p)) {
            p++;
        }
        if (*p == ',') {
            p++;
        } else if (*p != '}') {
            return FALSE;
        }
    }
}

static int compare_strings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char * const *)a, *(char * const *)b);
}

/* A body's fields, and its URL's query's as "?name=value": sorted, once each. */
static char **fields_of(const char *body_query, const char *url)
{
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
    char **body = archive_request_body_fields(body_query);
    for (char **f = body; *f; f++) {
        g_ptr_array_add(all, *f);
    }
    g_free(body);
    const char *q = strchr(url, '?');
    if (q) {
        char *query = g_strndup(q + 1, strcspn(q + 1, "#"));
        char **terms = g_strsplit(query, "&", -1);
        for (char **t = terms; *t; t++) {
            if (**t) {
                g_ptr_array_add(all, g_strconcat("?", *t, NULL));
            }
        }
        g_strfreev(terms);
        g_free(query);
    }
    g_ptr_array_sort(all, compare_strings);
    GPtrArray *unique = g_ptr_array_new();
    for (guint i = 0; i < all->len; i++) {
        char *f = g_ptr_array_index(all, i);
        if (!unique->len || strcmp(g_ptr_array_index(unique, unique->len - 1), f)) {
            g_ptr_array_add(unique, g_strdup(f));
        }
    }
    g_ptr_array_unref(all);
    g_ptr_array_add(unique, NULL);
    return (char **)g_ptr_array_free(unique, FALSE);
}

static gboolean has_field(char **sorted, const char *field)
{
    guint n = g_strv_length(sorted);
    return bsearch(&field, sorted, n, sizeof(char *), compare_strings) != NULL;
}

/* What a lookup needs of an entry, however its JSON is written. */
static void read_entry_json(Entry *e)
{
    if (!read_flat_json(e)) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, e->json, -1, NULL)
                && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            JsonObjectIter iter;
            const char *name;
            JsonNode *value;
            json_object_iter_init(&iter, json_node_get_object(json_parser_get_root(parser)));
            while (json_object_iter_next(&iter, &name, &value)) {
                if (JSON_NODE_HOLDS_VALUE(value)) {
                    take_field(e, name, json_node_get_value_type(value) == G_TYPE_STRING
                            ? g_strdup(json_node_get_string(value))
                            : g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(value)));
                }
            }
        }
        g_object_unref(parser);
    }
    e->url      = e->url ? e->url : g_strdup("");
    e->mime     = e->mime ? e->mime : g_strdup("");
    e->filename = e->filename ? e->filename : g_strdup("");
    e->at = seconds_of(e->ts);
    if (e->request_body_full || e->request_body) {
        e->fields = fields_of(e->request_body_full ? e->request_body_full : e->request_body, e->url);
    }
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
        read_entry_json(e);
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
    r->streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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
    g_hash_table_unref(r->streams);
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

static char *without_fragment(const char *url);
static gint64 moment_for(ArchiveReplay *r, const char *url, const char *referer, gboolean page);

gint64 archive_replay_page_moment(ArchiveReplay *r, const char *url, const char *referer)
{
    char *bare = without_fragment(url);
    char *from = referer ? without_fragment(referer) : NULL;
    g_mutex_lock(&r->lock);
    gint64 moment = moment_for(r, bare, from, TRUE);
    g_mutex_unlock(&r->lock);
    g_free(from);
    g_free(bare);
    /* nothing to go by yet: the first page's moment, or the first capture's */
    if (!moment && r->pages->len) {
        moment = seconds_of(((ArchiveReplayPage *)g_ptr_array_index(r->pages, 0))->ts);
    }
    if (!moment && r->entries->len) {
        moment = ((Entry *)g_ptr_array_index(r->entries, 0))->at;
    }
    return moment;
}

void archive_replay_counts(ArchiveReplay *r, guint *served, guint *missed)
{
    g_mutex_lock(&r->lock);
    *served = r->served;
    *missed = r->missed;
    g_mutex_unlock(&r->lock);
}

/* ---- finding ------------------------------------------------------------ */

/* The first entry whose key is not less than key. */
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

typedef gboolean (*Accept)(const Entry *e, gconstpointer data);

/* What a lookup is for: what was asked, and whether as a page. */
typedef struct {
    const char *url;
    gboolean   page;
} Asked;

static gboolean whole(const Entry *e, gconstpointer data)
{
    return e->status != 206;
}

static gboolean whole_of_this_url(const Entry *e, gconstpointer data)
{
    return whole(e, data) && !strcmp(e->url, ((const Asked *)data)->url);
}

static gboolean any_of_this_url(const Entry *e, gconstpointer data)
{
    return !strcmp(e->url, ((const Asked *)data)->url);
}

static gboolean any(const Entry *e, gconstpointer data)
{
    return TRUE;
}

static gboolean a_page(const Entry *e, gconstpointer data)
{
    return e->status >= 200 && e->status < 300;
}

/* Whether the site turned the visitor away for the moment -- a sign-in
 * wall, a bot check, too many asks -- rather than answering for the page. */
static gboolean turned_away(const Entry *e)
{
    return e->status == 401 || e->status == 403 || e->status == 429;
}

/* Whether an entry may answer: not a websocket's messages, which answer
 * only its handshake; accepted; and, asked, not a turning away. */
static gboolean acceptable(const Entry *e, Accept accept, gconstpointer data, gboolean not_away)
{
    return strcmp(e->mime, ARCHIVE_WEBSOCKET_MIME) && accept(e, data)
        && !(not_away && turned_away(e));
}

static gint64 distance(const Entry *e, gint64 moment)
{
    return moment ? llabs(e->at - moment) : e->at;      /* no moment: the earliest */
}

/* Of the captures filed under key exactly, the accepted one nearest moment. */
static const Entry *find_nearest(ArchiveReplay *r, const char *key, gint64 moment, Accept accept,
        gconstpointer data, gboolean not_away)
{
    const Entry *best = NULL;
    gint64 best_far   = 0;
    for (guint i = lower_bound(r, key); i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strcmp(e->key, key)) {
            break;
        }
        if (acceptable(e, accept, data, not_away) && (!best || distance(e, moment) < best_far)) {
            best     = e;
            best_far = distance(e, moment);
        }
    }
    return best;
}

/* Calls visit with each capture of key's path -- the key before its "?" --
 * by any method and query, which sort together after it, though not just
 * after: "/a-b" sorts between "/a" and "/a?". */
static void each_capture_of_path(ArchiveReplay *r, const char *key,
        void (*visit)(const Entry *e, const char *query, gpointer data), gpointer data)
{
    const char *q = strchr(key, '?');
    char *path    = q ? g_strndup(key, q - key) : g_strdup(key);
    gsize n       = strlen(path);
    char *with_query = g_strconcat(path, "?", NULL);
    for (guint i = lower_bound(r, path); i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strcmp(e->key, path)) {
            break;
        }
        visit(e, "", data);
    }
    for (guint i = lower_bound(r, with_query); i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strncmp(e->key, with_query, n + 1)) {
            break;
        }
        visit(e, e->key + n + 1, data);
    }
    g_free(with_query);
    g_free(path);
}

/* The method a key files its capture under, upper-cased: the one it names in
 * its query (__wb_method), or GET. */
static char *key_method(const char *query)
{
    char **terms = g_strsplit(query ? query : "", "&", -1);
    char *method = NULL;
    for (char **t = terms; *t && !method; t++) {
        if (g_str_has_prefix(*t, "__wb_method=")) {
            method = g_ascii_strup(*t + strlen("__wb_method="), -1);
        }
    }
    g_strfreev(terms);
    return method ? method : g_strdup("GET");
}

static const char *query_of(const char *key)
{
    const char *q = strchr(key, '?');
    return q ? q + 1 : "";
}

/* A query as parameters: name -> values (a GPtrArray of char *, sorted); a
 * name alone has the empty value. The method is left out. */
static GHashTable *parameters_of(const char *query)
{
    GHashTable *params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)g_ptr_array_unref);
    char **terms = g_strsplit(query, "&", -1);
    for (char **t = terms; *t; t++) {
        if (!**t) {
            continue;
        }
        char *eq   = strchr(*t, '=');
        char *name = eq ? g_strndup(*t, eq - *t) : g_strdup(*t);
        if (!strcmp(name, "__wb_method")) {
            g_free(name);
            continue;
        }
        GPtrArray *values = g_hash_table_lookup(params, name);
        if (!values) {
            values = g_ptr_array_new_with_free_func(g_free);
            g_hash_table_insert(params, g_strdup(name), values);
        }
        g_ptr_array_add(values, g_strdup(eq ? eq + 1 : ""));
        g_free(name);
    }
    g_strfreev(terms);
    GHashTableIter iter;
    gpointer values;
    g_hash_table_iter_init(&iter, params);
    while (g_hash_table_iter_next(&iter, NULL, &values)) {
        g_ptr_array_sort(values, (GCompareFunc)g_strcmp0);
    }
    return params;
}

/* How well a capture's query stands in for the one asked: a term both have
 * counts 2, a name both have 1; one only either has, -1. */
static int agreement(GHashTable *wanted, GHashTable *candidate)
{
    int score = 0;
    GHashTableIter iter;
    gpointer name, values;
    g_hash_table_iter_init(&iter, wanted);
    while (g_hash_table_iter_next(&iter, &name, &values)) {
        GPtrArray *want = values, *have = g_hash_table_lookup(candidate, name);
        for (guint i = 0; i < want->len; i++) {
            gboolean same = FALSE;
            for (guint j = 0; have && j < have->len && !same; j++) {
                same = !strcmp(g_ptr_array_index(want, i), g_ptr_array_index(have, j));
            }
            score += same ? 2 : have ? 1 : -1;
        }
    }
    g_hash_table_iter_init(&iter, candidate);
    while (g_hash_table_iter_next(&iter, &name, NULL)) {
        if (!g_hash_table_contains(wanted, name)) {
            score -= 1;
        }
    }
    return score;
}

/* "scheme://host[:port]/", the start every URL from an origin has. */
static char *origin_prefix(const char *url)
{
    const char *scheme_end = url ? strstr(url, "://") : NULL;
    const char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    return scheme_end ? (path ? g_strndup(url, path - url + 1) : g_strconcat(url, "/", NULL))
        : NULL;
}

typedef struct {
    Accept       accept;
    gconstpointer data;
    gboolean     not_away;
    gint64       moment;
    char         *method;
    GHashTable   *wanted;
    char         *origin;       /* the asked URL's */
    const Entry  *best;
    int          best_score;
    gboolean     best_same_origin;
    gint64       best_far;
} Search;

static void search_init(Search *s, const char *key, gint64 moment, Accept accept,
        gconstpointer data, gboolean not_away)
{
    memset(s, 0, sizeof(*s));
    s->accept   = accept;
    s->data     = data;
    s->not_away = not_away;
    s->moment   = moment;
    s->method   = key_method(query_of(key));
    s->wanted   = parameters_of(query_of(key));
    s->origin   = origin_prefix(((const Asked *)data)->url);
}

static void search_clear(Search *s)
{
    g_free(s->method);
    g_free(s->origin);
    g_hash_table_unref(s->wanted);
}

static gboolean search_considers(Search *s, const Entry *e, const char *query)
{
    if (!acceptable(e, s->accept, s->data, s->not_away)) {
        return FALSE;
    }
    char *method = key_method(query);
    gboolean same = !strcmp(method, s->method);
    g_free(method);
    return same;
}

/* Of two stand-ins, the one that agrees more; then one from the asked URL's
 * own origin -- another, filed under the same key for SURT drops "www.", is
 * as a rule a redirect to it, and would send a page round for ever; then the
 * nearer. */
static void search_offer(Search *s, const Entry *e, int score)
{
    gint64 far = distance(e, s->moment);
    gboolean same = s->origin && g_str_has_prefix(e->url, s->origin);
    if (!s->best || score > s->best_score
            || (score == s->best_score && same && !s->best_same_origin)
            || (score == s->best_score && same == s->best_same_origin && far < s->best_far)) {
        s->best             = e;
        s->best_score       = score;
        s->best_same_origin = same;
        s->best_far         = far;
    }
}

static void visit_closest(const Entry *e, const char *query, gpointer data)
{
    Search *s = data;
    if (search_considers(s, e, query)) {
        GHashTable *candidate = parameters_of(query);
        search_offer(s, e, agreement(s->wanted, candidate));
        g_hash_table_unref(candidate);
    }
}

/* For what a page loads: of the captures of key's path by its method, the
 * one whose query agrees most with key's, and of those, the nearest. What a
 * page loads asks anew each time with something new in it. */
static const Entry *find_closest(ArchiveReplay *r, const char *key, gint64 moment, Accept accept,
        gconstpointer data, gboolean not_away)
{
    Search s;
    search_init(&s, key, moment, accept, data, not_away);
    each_capture_of_path(r, key, visit_closest, &s);
    search_clear(&s);
    return s.best;
}

/* ---- values a load makes for itself ---- */

typedef enum {
    KIND_CHOSEN,                /* by whoever wrote the URL, for all that can be told */
    KIND_EMPTY,                 /* nothing yet */
    KIND_CLOCK,
    KIND_RANDOM,
    KIND_ID,
} ValueKind;

#define CLOCK_REACH (24 * 3600)     /* how far a reading of the clock may stray */

static gboolean is_clock_reading(const char *v, gint64 moment)
{
    gsize n = strlen(v);
    if (!moment || (n != 10 && n != 13)) {
        return FALSE;
    }
    for (const char *c = v; *c; c++) {
        if (!g_ascii_isdigit(*c)) {
            return FALSE;
        }
    }
    gint64 reading = (gint64)g_ascii_strtoull(v, NULL, 10);
    if (n == 13) {
        reading /= 1000;
    }
    return llabs(reading - moment) <= CLOCK_REACH;
}

static gboolean is_random_fraction(const char *v)
{
    if (strlen(v) < 8 || !g_str_has_prefix(v, "0.")) {
        return FALSE;
    }
    for (const char *c = v + 2; *c; c++) {
        if (!g_ascii_isdigit(*c)) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean is_uuid(const char *t, gsize n)
{
    if (n != 36) {
        return FALSE;
    }
    for (gsize i = 0; i < n; i++) {
        gboolean dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? t[i] != '-' : !g_ascii_isxdigit(t[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 16 hex digits or more, letters and digits both: all digits is a number a
 * site counts out, all letters a word. */
static gboolean is_hex_id(const char *t, gsize n)
{
    gboolean digit = FALSE, alpha = FALSE;
    if (n < 16) {
        return FALSE;
    }
    for (gsize i = 0; i < n; i++) {
        if (!g_ascii_isxdigit(t[i])) {
            return FALSE;
        }
        digit |= g_ascii_isdigit(t[i]);
        alpha |= g_ascii_isalpha(t[i]);
    }
    return digit && alpha;
}

/* An id alone, or ids among punctuation, as a page strings them together. */
static gboolean is_made_id(const char *v)
{
    gboolean found = FALSE;
    const char *c = v;
    while (*c) {
        if (!g_ascii_isalnum(*c) && *c != '-') {
            c++;
            continue;
        }
        const char *start = c;
        while (*c && (g_ascii_isalnum(*c) || *c == '-')) {
            c++;
        }
        if (!is_uuid(start, c - start) && !is_hex_id(start, c - start)) {
            return FALSE;
        }
        found = TRUE;
    }
    return found;
}

static ValueKind kind_of(const char *v, gint64 moment)
{
    static const char *empties[] = {"", "null", "undefined", "nan", "false", "0", NULL};
    for (int i = 0; empties[i]; i++) {
        if (!g_ascii_strcasecmp(v, empties[i])) {
            return KIND_EMPTY;
        }
    }
    return is_clock_reading(v, moment) ? KIND_CLOCK : is_random_fraction(v) ? KIND_RANDOM
        : is_made_id(v) ? KIND_ID : KIND_CHOSEN;
}

/* Whether the values a parameter was given in two queries (NULL: not given)
 * differ only as those a load makes for itself do: a value made for the visit
 * where the other had none yet, or another reading of the clock, or another
 * roll of the die -- but never another id, which may name another thing. */
static gboolean differ_only_as_visit_values(GPtrArray *want, gint64 want_at, GPtrArray *have,
        gint64 have_at)
{
    guint wn = want ? want->len : 0, hn = have ? have->len : 0;
    if (wn == hn) {
        gboolean same = TRUE;
        for (guint i = 0; i < wn && same; i++) {
            same = !strcmp(g_ptr_array_index(want, i), g_ptr_array_index(have, i));
        }
        if (same) {
            return TRUE;
        }
    }
    if (wn > 1 || hn > 1) {
        return FALSE;               /* a list is chosen */
    }
    ValueKind a = wn ? kind_of(g_ptr_array_index(want, 0), want_at) : KIND_EMPTY;
    ValueKind b = hn ? kind_of(g_ptr_array_index(have, 0), have_at) : KIND_EMPTY;
    if (a == KIND_CHOSEN || b == KIND_CHOSEN) {
        return FALSE;
    }
    if (a == KIND_EMPTY || b == KIND_EMPTY) {
        return a != b;
    }
    return a == b && a != KIND_ID;
}

static void visit_ignoring_visit_values(const Entry *e, const char *query, gpointer data)
{
    Search *s = data;
    if (!search_considers(s, e, query)) {
        return;
    }
    GHashTable *have = parameters_of(query);
    gboolean ok = TRUE;
    GHashTableIter iter;
    gpointer name, values;
    g_hash_table_iter_init(&iter, s->wanted);
    while (ok && g_hash_table_iter_next(&iter, &name, &values)) {
        ok = differ_only_as_visit_values(values, s->moment, g_hash_table_lookup(have, name), e->at);
    }
    g_hash_table_iter_init(&iter, have);
    while (ok && g_hash_table_iter_next(&iter, &name, &values)) {
        ok = g_hash_table_contains(s->wanted, name)
            || differ_only_as_visit_values(NULL, s->moment, values, e->at);
    }
    g_hash_table_unref(have);
    if (ok) {
        search_offer(s, e, 0);
    }
}

/* For a page: the capture of key's path, by its method, whose query is key's
 * once what each load makes anew is set aside -- the consent frame a news
 * site opens with an id made for the visit -- and of those, the nearest. */
static const Entry *find_ignoring_visit_values(ArchiveReplay *r, const char *key, gint64 moment,
        Accept accept, gconstpointer data, gboolean not_away)
{
    Search s;
    search_init(&s, key, moment, accept, data, not_away);
    each_capture_of_path(r, key, visit_ignoring_visit_values, &s);
    search_clear(&s);
    return s.best;
}

/* A query value as it reads: "+" a space, escapes undone. */
static char *unescaped(const char *v)
{
    char *spaced = g_strdelimit(g_strdup(v), "+", ' ');
    char *plain  = g_uri_unescape_string(spaced, NULL);
    g_free(spaced);
    return plain ? plain : g_strdup(v);
}

static void visit_fewer_parameters(const Entry *e, const char *query, gpointer data)
{
    Search *s = data;
    if (!search_considers(s, e, query)) {
        return;
    }
    GHashTable *have = parameters_of(query);
    int shared = 0;
    gboolean ok = TRUE;
    GHashTableIter iter;
    gpointer name, values;
    g_hash_table_iter_init(&iter, have);
    while (ok && g_hash_table_iter_next(&iter, &name, &values)) {
        GPtrArray *want = g_hash_table_lookup(s->wanted, name);
        GPtrArray *vs   = values;
        ok = want != NULL;
        for (guint i = 0; ok && i < vs->len; i++) {
            char *reads = unescaped(g_ptr_array_index(vs, i));
            gboolean asked = FALSE;
            for (guint j = 0; j < want->len && !asked; j++) {
                char *a = unescaped(g_ptr_array_index(want, j));
                asked = !strcmp(a, reads);
                g_free(a);
            }
            g_free(reads);
            ok = asked;
            shared++;
        }
    }
    g_hash_table_unref(have);
    if (ok) {
        search_offer(s, e, shared);
    }
}

/* For a page that added to its own address and reloads: the capture of the
 * address it added to -- every parameter the capture's is the request's too,
 * with its value -- sharing the most with it, and the nearest of those. */
static const Entry *find_with_fewer_parameters(ArchiveReplay *r, const char *key, gint64 moment,
        Accept accept, gconstpointer data)
{
    Search s;
    search_init(&s, key, moment, accept, data, FALSE);
    each_capture_of_path(r, key, visit_fewer_parameters, &s);
    search_clear(&s);
    return s.best;
}

/* ---- streams and bodies ---- */

typedef struct {
    guint  next;
    gint64 last_count;
} StreamPosition;

typedef struct {
    char *name;
    char *value;
} Param;

static void param_free(Param *p)
{
    g_free(p->name);
    g_free(p->value);
    g_free(p);
}

static int compare_params(gconstpointer a, gconstpointer b)
{
    const Param *x = *(Param * const *)a, *y = *(Param * const *)b;
    int c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->value, y->value);
}

/* A URL's query, as names and values, sorted. */
static GPtrArray *params_of(const char *url)
{
    GPtrArray *params = g_ptr_array_new_with_free_func((GDestroyNotify)param_free);
    const char *q = strchr(url, '?');
    if (q) {
        char *query = g_strndup(q + 1, strcspn(q + 1, "#"));
        char **terms = g_strsplit(query, "&", -1);
        for (char **t = terms; *t; t++) {
            if (!**t) {
                continue;
            }
            char *eq = strchr(*t, '=');
            Param *p = g_new0(Param, 1);
            p->name  = eq ? g_strndup(*t, eq - *t) : g_strdup(*t);
            p->value = g_strdup(eq ? eq + 1 : "");
            g_ptr_array_add(params, p);
        }
        g_strfreev(terms);
        g_free(query);
    }
    g_ptr_array_sort(params, compare_params);
    return params;
}

static const char *param_value(GPtrArray *params, const char *name, gboolean *present)
{
    for (guint i = 0; i < params->len; i++) {
        Param *p = g_ptr_array_index(params, i);
        if (!strcmp(p->name, name)) {
            *present = TRUE;
            return p->value;
        }
    }
    *present = FALSE;
    return NULL;
}

static gboolean is_count(const char *v)
{
    gsize n = strlen(v);
    if (!n || n > 12) {
        return FALSE;
    }
    for (const char *c = v; *c; c++) {
        if (!g_ascii_isdigit(*c)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Whether two queries, setting aside what each made up, are the same but
 * for at most one parameter both give a count -- a request counted; *by is
 * that parameter, NULL if they don't differ at all. */
static gboolean counted_by(GPtrArray *asked, GPtrArray *captured, GHashTable *made_up,
        const char **by)
{
    GPtrArray *a = g_ptr_array_new(), *b = g_ptr_array_new();
    for (guint i = 0; i < asked->len; i++) {
        Param *p = g_ptr_array_index(asked, i);
        if (!g_hash_table_contains(made_up, p->name)) {
            g_ptr_array_add(a, p);
        }
    }
    for (guint i = 0; i < captured->len; i++) {
        Param *p = g_ptr_array_index(captured, i);
        if (!g_hash_table_contains(made_up, p->name)) {
            g_ptr_array_add(b, p);
        }
    }
    const char *counted = NULL;
    gboolean ok = a->len == b->len;
    for (guint i = 0; ok && i < a->len; i++) {
        Param *x = g_ptr_array_index(a, i), *y = g_ptr_array_index(b, i);
        if (!strcmp(x->name, y->name) && !strcmp(x->value, y->value)) {
            continue;
        }
        ok = !strcmp(x->name, y->name) && !counted && is_count(x->value) && is_count(y->value);
        counted = x->name;
    }
    g_ptr_array_unref(a);
    g_ptr_array_unref(b);
    *by = counted;
    return ok;
}

static int compare_capture_order(gconstpointer a, gconstpointer b)
{
    const Entry *x = *(Entry * const *)a, *y = *(Entry * const *)b;
    int c = strcmp(x->ts, y->ts);
    if (!c) {
        c = strcmp(x->filename, y->filename);
    }
    return c ? c : (x->offset < y->offset ? -1 : x->offset > y->offset);
}

/* Of the candidates, those of the stream the request is part of -- the same
 * address but for a count, one request after another, as YouTube counts
 * each ("rn") -- the next in the order the capture made them; NULL when it
 * has had them all. *is_stream says whether there is such a stream. A
 * stream asks for what follows what came before, in more fields than can be
 * matched one by one; but a player given the same answers asks in the same
 * order. */
static const Entry *next_in_stream(ArchiveReplay *r, const char *url, const char *key,
        GPtrArray *candidates, gboolean *is_stream)
{
    *is_stream = FALSE;
    GPtrArray *asked = params_of(url);
    GPtrArray *captured = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < candidates->len; i++) {
        g_ptr_array_add(captured, params_of(((Entry *)g_ptr_array_index(candidates, i))->url));
    }
    /* what each player made up for itself: a parameter every capture has,
     * none with the request's value -- YouTube's "cpn" */
    GHashTable *made_up = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < asked->len; i++) {
        Param *p = g_ptr_array_index(asked, i);
        gboolean everywhere = captured->len > 0, seen = FALSE;
        for (guint j = 0; j < captured->len && everywhere; j++) {
            gboolean present;
            const char *v = param_value(g_ptr_array_index(captured, j), p->name, &present);
            everywhere = present;
            seen |= present && !strcmp(v, p->value);
        }
        if (everywhere && !seen && !is_count(p->value)) {
            g_hash_table_add(made_up, p->name);
        }
    }
    const char *counted = NULL;
    GPtrArray *stream = g_ptr_array_new();
    for (guint i = 0; i < candidates->len; i++) {
        const char *by;
        if (counted_by(asked, g_ptr_array_index(captured, i), made_up, &by)
                && (!by || !counted || !strcmp(by, counted))) {
            counted = by ? by : counted;
            g_ptr_array_add(stream, g_ptr_array_index(candidates, i));
        }
    }
    const Entry *next = NULL;
    /* a count with one capture is no stream, nor are captures not counted */
    if (stream->len >= 2 && counted) {
        *is_stream = TRUE;
        g_ptr_array_sort(stream, compare_capture_order);
        /* named for the path and the address but its count */
        GString *identity = g_string_new(NULL);
        g_string_append_len(identity, key, strcspn(key, "?"));
        gint64 count = -1;
        for (guint i = 0; i < asked->len; i++) {
            Param *p = g_ptr_array_index(asked, i);
            if (!strcmp(p->name, counted)) {
                count = (gint64)g_ascii_strtoll(p->value, NULL, 10);
            } else if (!g_hash_table_contains(made_up, p->name)) {
                g_string_append_printf(identity, "&%s=%s", p->name, p->value);
            }
        }
        g_mutex_lock(&r->lock);
        StreamPosition *pos = g_hash_table_lookup(r->streams, identity->str);
        if (!pos) {
            pos = g_new0(StreamPosition, 1);
            pos->last_count = -1;
            g_hash_table_insert(r->streams, g_strdup(identity->str), pos);
        }
        if (count <= pos->last_count) {
            pos->next = 0;              /* counted from lower: a player begun again */
        }
        pos->last_count = count;
        if (pos->next < stream->len) {
            next = g_ptr_array_index(stream, pos->next++);
        }
        g_mutex_unlock(&r->lock);
        g_string_free(identity, TRUE);
    }
    g_ptr_array_unref(stream);
    g_hash_table_unref(made_up);
    g_ptr_array_unref(captured);
    g_ptr_array_unref(asked);
    return next;
}

typedef struct {
    char      *method;
    GPtrArray *candidates;
} BodyCandidates;

static void visit_body_candidate(const Entry *e, const char *query, gpointer data)
{
    BodyCandidates *b = data;
    char *method = key_method(query);
    if (e->fields && e->status != 206 && strcmp(e->mime, ARCHIVE_WEBSOCKET_MIME)
            && !strcmp(method, b->method)) {
        g_ptr_array_add(b->candidates, (gpointer)e);
    }
    g_free(method);
}

/* For a POST or PUT: of the captures of key's path by its method, the one
 * whose body -- and address -- agree most with the request's, field by field,
 * a field counting the more the fewer captures share it; the nearest moment
 * of those that agree as well; none, if none agrees in anything. What a page
 * sends differs in more than what it asks for, so a request is seldom
 * answered by its key alone: YouTube asks for its video by POSTing where its
 * player is and what it has buffered. A stream (see next_in_stream()) is
 * answered in its order. *settled is set when the answer, or that there is
 * none, stands -- a stream has had all the capture had, or (but for
 * only_if_cut) the bodies were weighed and none agreed -- so that nothing
 * that matches by less is looked for. With only_if_cut, only for a body its
 * key holds the start of (4096 characters): YouTube names the video asked
 * for further on than that. */
static const Entry *find_by_body(ArchiveReplay *r, const Asked *asked, const char *key,
        const char *method, const char *content_type, const guint8 *body, gsize body_len,
        gint64 moment, gboolean only_if_cut, gboolean *settled)
{
    *settled = FALSE;
    if (!body || !body_len || (g_ascii_strcasecmp(method, "POST") && g_ascii_strcasecmp(method, "PUT"))) {
        return NULL;
    }
    char *whole = archive_request_body_query(content_type, body, body_len, TRUE);
    char *cut   = only_if_cut ? archive_request_body_query(content_type, body, body_len, FALSE)
        : NULL;
    gboolean go = whole && *whole && (!only_if_cut || strlen(whole) > strlen(cut ? cut : ""));
    g_free(cut);
    BodyCandidates b = {key_method(query_of(key)), g_ptr_array_new()};
    if (go) {
        each_capture_of_path(r, key, visit_body_candidate, &b);
    }
    const Entry *best = NULL;
    if (b.candidates->len) {
        gboolean is_stream;
        best = next_in_stream(r, asked->url, key, b.candidates, &is_stream);
        if (is_stream) {
            *settled = best == NULL;
        } else {
            char **wanted = fields_of(whole, asked->url);
            GHashTable *sharing = g_hash_table_new(g_str_hash, g_str_equal);
            for (guint i = 0; i < b.candidates->len; i++) {
                for (char **f = ((Entry *)g_ptr_array_index(b.candidates, i))->fields; *f; f++) {
                    g_hash_table_insert(sharing, *f, GUINT_TO_POINTER(
                                GPOINTER_TO_UINT(g_hash_table_lookup(sharing, *f)) + 1));
                }
            }
            double best_score = 0;
            gint64 best_far = 0;
            for (guint i = 0; i < b.candidates->len; i++) {
                const Entry *e = g_ptr_array_index(b.candidates, i);
                double score = 0;
                for (char **f = wanted; *f; f++) {
                    if (has_field(e->fields, *f)) {
                        score += 1.0 / GPOINTER_TO_UINT(g_hash_table_lookup(sharing, *f));
                    }
                }
                gint64 far = distance(e, moment);
                /* agreeing in nothing is no answer */
                if (score > 0 && (!best || score > best_score
                            || (score == best_score && far < best_far))) {
                    best       = e;
                    best_score = score;
                    best_far   = far;
                }
            }
            g_hash_table_unref(sharing);
            g_strfreev(wanted);
            *settled = !best && !only_if_cut;
        }
    }
    g_ptr_array_unref(b.candidates);
    g_free(b.method);
    g_free(whole);
    return best;
}

typedef const Entry *(*Finder)(ArchiveReplay *r, const char *key, gint64 moment, Accept accept,
        gconstpointer data, gboolean not_away);

/* For a page, a capture the site let the visitor into before a nearer one in
 * which it turned them away: replayed, a bot check that reloads once satisfied
 * would be refused again at the same moment, and reload for ever. */
static const Entry *let_in_first(Finder find, ArchiveReplay *r, const char *key, gint64 moment,
        Accept accept, const Asked *asked)
{
    const Entry *e = asked->page ? find(r, key, moment, accept, asked, TRUE) : NULL;
    return e ? e : find(r, key, moment, accept, asked, FALSE);
}

/* The capture to answer a request with -- in the order the Chromium fork
 * looks, from the capture of the very thing asked, to what stands in for it:
 *
 *   - filed under the request's key (a HEAD the capture never made, under a
 *     GET's), nearest the moment: a whole capture of the very URL; a whole
 *     one; a part of the very URL; any. Another URL may share a key -- SURT
 *     drops the scheme and "www." -- and is as a rule a redirect to the one
 *     asked, which would send the page round for ever;
 *   - then what comes nearest: for what a page loads, the capture of its path
 *     whose query agrees most; for a page, one differing only in what the
 *     load made for itself;
 *   - last, for a page, the address it added to -- answered with a page,
 *     never a redirect, which could send it on round again. */
static const Entry *lookup(ArchiveReplay *r, const Asked *asked, const char *method,
        const char *content_type, const guint8 *body, gsize body_len, gint64 moment)
{
    char *keys[3] = {archive_request_key(asked->url, method, content_type, body,
            (gssize)body_len), NULL, NULL};
    if (!g_ascii_strcasecmp(method, "HEAD")) {
        keys[1] = archive_surt(asked->url);
    }
    static const Accept nearest_tiers[] = {whole_of_this_url, whole, any_of_this_url, any};
    static const Accept closest_tiers[] = {whole, any};
    gboolean settled = FALSE;
    /* a body the key holds only the start of: what agrees with all of it */
    const Entry *e = keys[0] ? find_by_body(r, asked, keys[0], method, content_type, body,
            body_len, moment, TRUE, &settled) : NULL;
    for (int k = 0; keys[k] && !e && !settled; k++) {
        for (gsize t = 0; t < G_N_ELEMENTS(nearest_tiers) && !e; t++) {
            e = let_in_first(find_nearest, r, keys[k], moment, nearest_tiers[t], asked);
        }
    }
    /* a POST or PUT whose body nothing was filed under: the one agreeing most */
    if (!e && !settled && keys[0]) {
        e = find_by_body(r, asked, keys[0], method, content_type, body, body_len, moment, FALSE,
                &settled);
    }
    /* a stream that has had all the capture had answers nothing more -- not
     * the last again, which a player would ask for again at once -- nor does
     * a body agreeing with none, which would only be given another's answer */
    for (int k = 0; keys[k] && !e && !settled; k++) {
        for (gsize t = 0; t < G_N_ELEMENTS(closest_tiers) && !e; t++) {
            e = let_in_first(asked->page ? find_ignoring_visit_values : find_closest, r, keys[k],
                    moment, closest_tiers[t], asked);
        }
    }
    for (int k = 0; keys[k] && !e && !settled && asked->page; k++) {
        e = find_with_fewer_parameters(r, keys[k], moment, a_page, asked);
    }
    g_free(keys[0]);
    g_free(keys[1]);
    return e;
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
static gint64 moment_for(ArchiveReplay *r, const char *url, const char *referer, gboolean page)
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

/* A record, read out of the archive. */
typedef struct {
    GByteArray   *data;
    char         *type;
    char         *content_type;
    char         *refers_to_uri;
    char         *refers_to_date;
    char         *payload_digest;
    const guint8 *block;
    gsize        block_len;
} Record;

static void record_clear(Record *rec)
{
    if (rec->data) {
        g_byte_array_unref(rec->data);
    }
    g_free(rec->type);
    g_free(rec->content_type);
    g_free(rec->refers_to_uri);
    g_free(rec->refers_to_date);
    g_free(rec->payload_digest);
    memset(rec, 0, sizeof(*rec));
}

static gboolean read_record(ArchiveReplay *r, const Entry *e, Record *rec, GError **error)
{
    memset(rec, 0, sizeof(*rec));
    char *name = g_strconcat("archive/", e->filename, NULL);
    const ArchiveZipEntry *ze = archive_zip_find(r->zip, name);
    g_free(name);
    if (!ze || !e->length || e->length > RECORD_MAX) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no record for %s", e->url);
        return FALSE;
    }
    guint8 *raw = g_malloc(e->length);
    if (!archive_zip_read_at(r->zip, ze, e->offset, raw, e->length, error)) {
        g_free(raw);
        return FALSE;
    }
    if (g_str_has_suffix(e->filename, ".gz")) {
        rec->data = gunzip(raw, e->length, RECORD_MAX, error);
        g_free(raw);
        if (!rec->data) {
            return FALSE;
        }
    } else {
        rec->data = g_byte_array_new_take(raw, e->length);
    }

    /* its WARC headers, then its block */
    const char *start = (const char *)rec->data->data;
    const char *sep   = g_strstr_len(start, MIN(rec->data->len, HEAD_MAX), "\r\n\r\n");
    if (!sep) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a WARC record");
        record_clear(rec);
        return FALSE;
    }
    char *head = g_strndup(start, sep - start);
    guint64 block_len = 0;
    char **lines = g_strsplit(head, "\r\n", -1);
    for (char **l = lines; *l; l++) {
        char *colon = strchr(*l, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        const char *v = g_strstrip(colon + 1);
        char **field = !g_ascii_strcasecmp(*l, "WARC-Type") ? &rec->type
            : !g_ascii_strcasecmp(*l, "Content-Type") ? &rec->content_type
            : !g_ascii_strcasecmp(*l, "WARC-Refers-To-Target-URI") ? &rec->refers_to_uri
            : !g_ascii_strcasecmp(*l, "WARC-Refers-To-Date") ? &rec->refers_to_date
            : !g_ascii_strcasecmp(*l, "WARC-Payload-Digest") ? &rec->payload_digest : NULL;
        if (field) {
            g_free(*field);
            *field = g_strdup(v);
        } else if (!g_ascii_strcasecmp(*l, "Content-Length")) {
            block_len = g_ascii_strtoull(v, NULL, 10);
        }
    }
    g_strfreev(lines);
    g_free(head);
    rec->block     = (const guint8 *)sep + 4;
    gsize avail    = rec->data->len - (rec->block - rec->data->data);
    rec->block_len = MIN(block_len ? block_len : avail, avail);
    return TRUE;
}

/* An HTTP response, ready to send on. */
typedef struct {
    guint      status;
    char       *reason;
    GPtrArray  *headers;        /* "Name: value", hop by hop gone */
    GByteArray *body;           /* de-chunked, still in its content coding */
} Response;

static void response_init(Response *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status  = 200;
    resp->reason  = g_strdup("OK");
    resp->headers = g_ptr_array_new_with_free_func(g_free);
    resp->body    = g_byte_array_new();
}

static void response_clear(Response *resp)
{
    g_free(resp->reason);
    if (resp->headers) {
        g_ptr_array_unref(resp->headers);
    }
    if (resp->body) {
        g_byte_array_unref(resp->body);
    }
    memset(resp, 0, sizeof(*resp));
}

/* The HTTP response a block holds; with head_only, its head alone -- a
 * revisit's, which a writer may end without the blank line. */
static gboolean parse_response(const guint8 *block, gsize len, gboolean head_only, Response *resp,
        GError **error)
{
    GByteArray *head = NULL;
    if (head_only) {
        head = g_byte_array_sized_new(len + 4);
        g_byte_array_append(head, block, len);
        if (!g_strstr_len((const char *)head->data, head->len, "\r\n\r\n")) {
            gboolean line_ended = len >= 2 && block[len - 2] == '\r' && block[len - 1] == '\n';
            g_byte_array_append(head, (const guint8 *)"\r\n\r\n", line_ended ? 2 : 4);
        }
        block = head->data;
        len   = head->len;
    }
    Http1Parser *p = http1_parser_new(HTTP1_RESPONSE, HEAD_MAX);
    /* as for a HEAD, no body follows the head of a revisit */
    http1_parser_set_request_method(p, head_only ? "HEAD" : "GET");
    http1_parser_set_payload_func(p, append_payload, resp->body);
    GError *perr = NULL;
    gssize used = http1_parser_feed(p, block, len, &perr);
    if (used >= 0 && !http1_parser_is_done(p)) {
        http1_parser_eof(p, NULL);          /* a body cut short is served as held */
    }
    gboolean ok = http1_parser_has_head(p);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "a response without a head%s%s",
                perr ? ": " : "", perr ? perr->message : "");
    } else {
        const Http1Head *h = http1_parser_head(p);
        resp->status = h->status;
        g_free(resp->reason);
        resp->reason = g_strdup(h->reason ? h->reason : "");
        for (guint i = 0; i < h->headers->len; i++) {
            Http1Header *hd = &g_array_index(h->headers, Http1Header, i);
            if (!hop_by_hop(hd->name)) {
                g_ptr_array_add(resp->headers, g_strdup_printf("%s: %s", hd->name, hd->value));
            }
        }
    }
    g_clear_error(&perr);
    http1_parser_free(p);
    if (head) {
        g_byte_array_unref(head);
    }
    return ok;
}

static const char *digest_value(const char *digest)
{
    const char *colon = digest ? strchr(digest, ':') : NULL;
    return colon ? colon + 1 : digest;      /* "sha1:AB..." or "AB..." */
}

typedef struct {
    const Entry *revisit;
    const char  *digest;
    const char  *target;
} Revisited;

/* A capture holding the payload: a response, not the revisit itself, whose
 * digest agrees where both are known. */
static gboolean holds_payload(const Entry *c, gconstpointer data)
{
    const Revisited *v = data;
    if ((!strcmp(c->filename, v->revisit->filename) && c->offset == v->revisit->offset)
            || !strcmp(c->mime, "warc/revisit")) {
        return FALSE;
    }
    return !v->digest || !c->digest || !g_strcmp0(digest_value(c->digest), digest_value(v->digest));
}

static gboolean holds_payload_at_target(const Entry *c, gconstpointer data)
{
    return !strcmp(c->url, ((const Revisited *)data)->target) && holds_payload(c, data);
}

/* The capture holding what a revisit refers to: of the URL it names (its own,
 * if none), nearest the date it names, with its digest; failing that, any
 * capture in the archive with that digest. */
static const Entry *find_revisited(ArchiveReplay *r, const Entry *e, const Record *rec)
{
    Revisited v = {
        .revisit = e,
        .digest  = e->digest ? e->digest : rec->payload_digest,
        .target  = rec->refers_to_uri ? rec->refers_to_uri : e->url,
    };
    gint64 moment = e->at;
    if (rec->refers_to_date) {
        char *ts = archive_replay_index_time(rec->refers_to_date);
        moment   = seconds_of(ts);
        g_free(ts);
    }
    char *key = !strcmp(v.target, e->url) ? g_strdup(e->key) : archive_surt(v.target);
    const Entry *found = NULL;
    if (key) {
        found = find_nearest(r, key, moment, holds_payload_at_target, &v, FALSE);
        found = found ? found : find_nearest(r, key, moment, holds_payload, &v, FALSE);
    }
    g_free(key);
    if (!found && v.digest) {
        for (guint i = 0; i < r->entries->len; i++) {
            const Entry *c = g_ptr_array_index(r->entries, i);
            if (c->digest && holds_payload(c, &v)
                    && (!found || distance(c, moment) < distance(found, moment))) {
                found = c;
            }
        }
    }
    return found;
}

/* The response an entry holds: a response record's, a resource as a 200, or
 * for a revisit, its own headers with the payload of what it refers to. */
static gboolean read_response(ArchiveReplay *r, const Entry *e, Response *resp, int depth,
        GError **error)
{
    Record rec;
    if (!read_record(r, e, &rec, error)) {
        return FALSE;
    }
    gboolean ok = FALSE;
    response_init(resp);
    if (!g_strcmp0(rec.type, "response")) {
        ok = parse_response(rec.block, rec.block_len, FALSE, resp, error);
    } else if (!g_strcmp0(rec.type, "resource")) {
        g_ptr_array_add(resp->headers, g_strdup_printf("Content-Type: %s",
                    rec.content_type ? rec.content_type : "application/octet-stream"));
        g_byte_array_append(resp->body, rec.block, rec.block_len);
        ok = TRUE;
    } else if (!g_strcmp0(rec.type, "revisit") && depth == 0) {
        const Entry *original = find_revisited(r, e, &rec);
        Response orig, own;
        if (!original) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "the archive doesn't hold what %s revisits", e->url);
        } else if (read_response(r, original, &orig, depth + 1, error)) {
            ok = TRUE;
            response_clear(resp);
            response_init(&own);
            /* its own headers, unless it has none, or the site said "not
             * modified": then the original, as the capturing browser had it */
            if (parse_response(rec.block, rec.block_len, TRUE, &own, NULL) && own.status != 304) {
                g_byte_array_unref(own.body);
                own.body  = orig.body;
                orig.body = NULL;
                *resp     = own;
                response_clear(&orig);
            } else {
                response_clear(&own);
                *resp = orig;
            }
        }
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "a %s record can't answer",
                rec.type ? rec.type : "untyped");
    }
    record_clear(&rec);
    if (!ok) {
        response_clear(resp);
    }
    return ok;
}

/* A response made into one for the browser: framed by its length, a range of
 * it if one was asked, and when it was captured. */
static GBytes *answer_with(const Response *resp, gint64 captured,
        const ArchiveReplayRequest *request)
{
    gboolean head_only = !g_ascii_strcasecmp(request->method, "HEAD");
    gsize from = 0, to = resp->body->len ? resp->body->len - 1 : 0;
    gboolean ranged = resp->status == 200 && asked_range(request->head, resp->body->len, &from, &to);
    GString *out = g_string_new(NULL);
    if (ranged) {
        g_string_append(out, "HTTP/1.1 206 Partial Content\r\n");
    } else {
        g_string_append_printf(out, "HTTP/1.1 %u %s\r\n", resp->status, resp->reason);
    }
    for (guint i = 0; i < resp->headers->len; i++) {
        g_string_append_printf(out, "%s\r\n", (char *)g_ptr_array_index(resp->headers, i));
    }
    if (ranged) {
        g_string_append_printf(out, "Content-Range: bytes %" G_GSIZE_FORMAT "-%" G_GSIZE_FORMAT
                "/%u\r\n", from, to, resp->body->len);
    }
    gsize length = ranged ? to - from + 1 : resp->body->len;
    char *when = http_date(captured);
    g_string_append_printf(out, "Content-Length: %" G_GSIZE_FORMAT "\r\nMemento-Datetime: %s\r\n\r\n",
            length, when);
    g_free(when);
    if (!head_only && resp->status != 204 && resp->status != 304 && resp->status >= 200) {
        g_string_append_len(out, (const char *)resp->body->data + (ranged ? from : 0), length);
    }
    gsize size = out->len;
    return g_bytes_new_take(g_string_free(out, FALSE), size);
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
    gint64 moment = moment_for(r, url, http1_head_get(request->head, "Referer"), page);
    g_mutex_unlock(&r->lock);

    Asked asked = {.url = url, .page = page};
    const Entry *e = lookup(r, &asked, request->method, type, request->body, request->body_len,
            moment);
    GBytes *answer = NULL;
    GError *error  = NULL;
    if (e) {
        Response resp;
        if (read_response(r, e, &resp, 0, &error)) {
            answer = answer_with(&resp, e->at, request);
            response_clear(&resp);
        }
    }
    g_mutex_lock(&r->lock);
    if (answer) {
        gint64 at = e->at;
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

    char *key = answer ? NULL : archive_request_key(url, request->method, type, request->body,
            (gssize)request->body_len);
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

/* ---- websockets ---------------------------------------------------------- */

static gboolean switched(const Entry *e, gconstpointer data)
{
    return e->status == 101;
}

ArchiveReplayWebsocket *archive_replay_websocket(ArchiveReplay *r,
        const ArchiveReplayRequest *request)
{
    char *url = without_fragment(request->url);
    char *key = archive_surt(url);
    g_mutex_lock(&r->lock);
    gint64 moment = moment_for(r, url, http1_head_get(request->head, "Referer"), FALSE);
    g_mutex_unlock(&r->lock);

    /* its messages, nearest the moment */
    const Entry *messages = NULL;
    for (guint i = key ? lower_bound(r, key) : r->entries->len; i < r->entries->len; i++) {
        const Entry *e = g_ptr_array_index(r->entries, i);
        if (strcmp(e->key, key)) {
            break;
        }
        if (!strcmp(e->mime, ARCHIVE_WEBSOCKET_MIME)
                && (!messages || distance(e, moment) < distance(messages, moment))) {
            messages = e;
        }
    }
    ArchiveReplayWebsocket *ws = NULL;
    Record rec;
    if (messages && read_record(r, messages, &rec, NULL)) {
        GPtrArray *read = ws_messages_read(rec.block, rec.block_len);
        record_clear(&rec);
        if (read) {
            ws = g_new0(ArchiveReplayWebsocket, 1);
            ws->messages = read;
            /* the subprotocol its handshake agreed, if it did */
            const Entry *handshake = find_nearest(r, key, messages->at, switched, NULL, FALSE);
            Response resp;
            if (handshake && read_response(r, handshake, &resp, 0, NULL)) {
                for (guint i = 0; i < resp.headers->len && !ws->protocol; i++) {
                    const char *h = g_ptr_array_index(resp.headers, i);
                    if (!g_ascii_strncasecmp(h, "Sec-WebSocket-Protocol:", 23)) {
                        ws->protocol = g_strstrip(g_strdup(h + 23));
                    }
                }
                response_clear(&resp);
            }
        }
    }
    g_mutex_lock(&r->lock);
    if (ws) {
        r->served++;
    } else {
        r->missed++;
    }
    g_mutex_unlock(&r->lock);
    g_free(key);
    g_free(url);
    return ws;
}

void archive_replay_websocket_free(ArchiveReplayWebsocket *ws)
{
    if (ws) {
        g_ptr_array_unref(ws->messages);
        g_free(ws->protocol);
        g_free(ws);
    }
}
