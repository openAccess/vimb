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
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "indexer.h"
#include "json.h"
#include "wacz.h"
#include "zip.h"

#define WACZ_VERSION     "1.1.1"
#define INDEX_DATA_NAME  "index.cdx.gz"
#define LINES_PER_BLOCK  300
#define COPY_SIZE        (1024 * 1024)
#define MAX_PAGES_FILE   (256 * 1024 * 1024)

typedef struct {
    char    *path;
    char    *name;
    guint64 length;     /* of whole records: what gets packaged */
    guint64 file_length;
    char    *hash;      /* as written into the WACZ */
} Archive;

static void archive_free(gpointer data)
{
    Archive *a = data;
    g_free(a->path);
    g_free(a->name);
    g_free(a->hash);
    g_free(a);
}

char *archive_page_line(const char *url, const char *ts, const char *title)
{
    char *uuid    = g_uuid_string_random();
    GString *line = g_string_new("{");
    archive_json_append_member(line, "id", uuid);
    archive_json_append_member(line, "url", url);
    archive_json_append_member(line, "ts", ts);
    if (title && *title && g_utf8_validate(title, -1, NULL)) {
        archive_json_append_member(line, "title", title);
    }
    g_string_append_c(line, '}');
    g_free(uuid);
    return g_string_free(line, FALSE);
}

static gboolean gzip_compress(const char *data, gsize len, GByteArray *out)
{
    z_stream zs = {0};
    guint8 buf[16384];
    int ret;
    if (deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return FALSE;
    }
    zs.next_in  = (Bytef *)data;
    zs.avail_in = (uInt)len;
    do {
        zs.next_out  = buf;
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        g_byte_array_append(out, buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END;
}

static char *sha256_label(const void *data, gsize len)
{
    char *hex   = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
    char *label = g_strconcat("sha256:", hex, NULL);
    g_free(hex);
    return label;
}

/* The index in blocks of LINES_PER_BLOCK lines, each gzipped on its own,
 * and the index of the blocks: each block's first key and timestamp, and
 * where it is. */
static gboolean compress_index(GPtrArray *lines, GByteArray *data, GString *blocks)
{
    g_string_append(blocks, "!meta 0 {\"format\": \"cdxj-gzip-1.0\", \"filename\": \""
            INDEX_DATA_NAME "\"}\n");
    for (guint first = 0; first < lines->len; first += LINES_PER_BLOCK) {
        guint last   = MIN(lines->len, first + LINES_PER_BLOCK);
        GString *text = g_string_new(NULL);
        for (guint i = first; i < last; i++) {
            g_string_append(text, g_ptr_array_index(lines, i));
            g_string_append_c(text, '\n');
        }
        GByteArray *block = g_byte_array_new();
        gboolean ok       = gzip_compress(text->str, text->len, block);
        g_string_free(text, TRUE);
        if (!ok) {
            g_byte_array_free(block, TRUE);
            return FALSE;
        }
        const char *line  = g_ptr_array_index(lines, first);
        const char *space = strchr(line, ' ');
        const char *end   = space ? strchr(space + 1, ' ') : NULL;
        char *digest      = sha256_label(block->data, block->len);
        g_string_append_len(blocks, line, end ? end - line : (gssize)strlen(line));
        g_string_append_printf(blocks, " {\"offset\": %u, \"length\": %u, \"digest\": \"%s\"}\n",
                data->len, block->len, digest);
        g_free(digest);
        g_byte_array_append(data, block->data, block->len);
        g_byte_array_free(block, TRUE);
    }
    return TRUE;
}

static int compare_strings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int compare_archives(gconstpointer a, gconstpointer b)
{
    return strcmp((*(Archive *const *)a)->name, (*(Archive *const *)b)->name);
}

/* An RFC 3339 time to the second: what WACZ "created" holds. */
static char *rfc3339_now(void)
{
    GDateTime *now = g_date_time_new_now_utc();
    char *text     = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(now);
    return text;
}

/* The first page in a page list: its url and ts. */
static gboolean first_page(const char *pages, char **url, char **ts)
{
    char **lines = g_strsplit(pages, "\n", -1);
    gboolean found = FALSE;
    for (char **l = lines; *l && !found; l++) {
        JsonParser *parser = json_parser_new_immutable();
        if (**l && json_parser_load_from_data(parser, *l, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *o = json_node_get_object(root);
                const char *u = json_object_get_string_member_with_default(o, "url", NULL);
                if (u && *u) {
                    *url  = g_strdup(u);
                    *ts   = g_strdup(json_object_get_string_member_with_default(o, "ts", ""));
                    found = TRUE;
                }
            }
        }
        g_object_unref(parser);
    }
    g_strfreev(lines);
    return found;
}

typedef struct {
    char    *path;
    char    *hash;
    guint64 bytes;
} Resource;

static void resource_free(gpointer data)
{
    Resource *r = data;
    g_free(r->path);
    g_free(r->hash);
    g_free(r);
}

static void add_resource(GPtrArray *resources, const char *path, char *hash, guint64 bytes)
{
    Resource *r = g_new0(Resource, 1);
    r->path  = g_strdup(path);
    r->hash  = hash;
    r->bytes = bytes;
    g_ptr_array_add(resources, r);
}

static char *data_package(GPtrArray *resources, const char *software, const char *title,
        const char *created, const char *main_url, const char *main_ts)
{
    GString *p = g_string_new("{\n  \"profile\": \"data-package\",\n  \"resources\": [\n");
    for (guint i = 0; i < resources->len; i++) {
        Resource *r      = g_ptr_array_index(resources, i);
        const char *base = strrchr(r->path, '/');
        char *name       = g_ascii_strdown(base ? base + 1 : r->path, -1);
        g_string_append(p, "    {\n      \"name\": ");
        archive_json_append_string(p, name);
        g_string_append(p, ",\n      \"path\": ");
        archive_json_append_string(p, r->path);
        g_string_append(p, ",\n      \"hash\": ");
        archive_json_append_string(p, r->hash);
        g_string_append_printf(p, ",\n      \"bytes\": %" G_GUINT64_FORMAT "\n    }%s\n",
                r->bytes, i + 1 < resources->len ? "," : "");
        g_free(name);
    }
    g_string_append(p, "  ],\n  \"wacz_version\": \"" WACZ_VERSION "\",\n  \"software\": ");
    archive_json_append_string(p, software);
    g_string_append(p, ",\n  \"created\": ");
    archive_json_append_string(p, created);
    if (title && *title && g_utf8_validate(title, -1, NULL)) {
        g_string_append(p, ",\n  \"title\": ");
        archive_json_append_string(p, title);
    }
    if (main_url) {
        g_string_append(p, ",\n  \"mainPageURL\": ");
        archive_json_append_string(p, main_url);
        if (main_ts && *main_ts) {
            g_string_append(p, ",\n  \"mainPageDate\": ");
            archive_json_append_string(p, main_ts);
        }
    }
    g_string_append(p, "\n}");
    return g_string_free(p, FALSE);
}

static gboolean write_wacz(const char *path, GPtrArray *archives, GPtrArray *lines,
        const char *pages, const char *software, const char *title, GError **error)
{
    GByteArray *index_data = g_byte_array_new();
    GString *blocks        = g_string_new(NULL);
    GPtrArray *resources   = g_ptr_array_new_with_free_func(resource_free);
    ArchiveZipWriter *zip  = NULL;
    gboolean ok            = FALSE;
    char *hash;

    if (!compress_index(lines, index_data, blocks)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not compress the index");
        goto out;
    }
    if (!(zip = archive_zip_writer_new(path, error))) {
        goto out;
    }
    if (!archive_zip_add(zip, "indexes/" INDEX_DATA_NAME, index_data->data, index_data->len,
                &hash, error)) {
        goto out;
    }
    add_resource(resources, "indexes/" INDEX_DATA_NAME, hash, index_data->len);
    if (!archive_zip_add(zip, "indexes/index.idx", blocks->str, blocks->len, &hash, error)) {
        goto out;
    }
    add_resource(resources, "indexes/index.idx", hash, blocks->len);

    for (guint i = 0; i < archives->len; i++) {
        Archive *a  = g_ptr_array_index(archives, i);
        char *entry = g_strconcat("archive/", a->name, NULL);
        gboolean added = archive_zip_add_file(zip, entry, a->path, a->length, &a->hash, error);
        if (added) {
            add_resource(resources, entry, g_strdup(a->hash), a->length);
        }
        g_free(entry);
        if (!added) {
            goto out;
        }
    }
    if (!archive_zip_add(zip, "pages/pages.jsonl", pages, strlen(pages), &hash, error)) {
        goto out;
    }
    add_resource(resources, "pages/pages.jsonl", hash, strlen(pages));

    char *main_url = NULL, *main_ts = NULL;
    first_page(pages, &main_url, &main_ts);
    char *created = rfc3339_now();
    char *package = data_package(resources, software, title, created, main_url, main_ts);
    char *package_hash = sha256_label(package, strlen(package));
    char *digest = g_strdup_printf("{\n  \"path\": \"datapackage.json\",\n  \"hash\": \"%s\"\n}",
            package_hash);
    ok = archive_zip_add(zip, "datapackage.json", package, strlen(package), NULL, error)
        && archive_zip_add(zip, "datapackage-digest.json", digest, strlen(digest), NULL, error);
    g_free(digest);
    g_free(package_hash);
    g_free(package);
    g_free(created);
    g_free(main_url);
    g_free(main_ts);
    if (ok) {
        ok  = archive_zip_finish(zip, error);
        zip = NULL;
    }
out:
    if (zip) {
        archive_zip_abandon(zip);
    }
    g_ptr_array_unref(resources);
    g_string_free(blocks, TRUE);
    g_byte_array_free(index_data, TRUE);
    return ok;
}

/* Counts the lines in the index blocks of a WACZ, to check it reads back. */
static gboolean count_index_lines(ArchiveZipReader *zip, guint *count, GError **error)
{
    const ArchiveZipEntry *entry = archive_zip_find(zip, "indexes/" INDEX_DATA_NAME);
    if (!entry || !entry->stored) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The index is missing");
        return FALSE;
    }
    GBytes *data = archive_zip_read_entry(zip, entry, 1024 * 1024 * 1024, error);
    if (!data) {
        return FALSE;
    }
    gsize len;
    const guint8 *p = g_bytes_get_data(data, &len);
    z_stream zs     = {0};
    guint8 buf[16384];
    gboolean ok     = TRUE;
    inflateInit2(&zs, 15 + 16);
    zs.next_in  = (Bytef *)p;
    zs.avail_in = (uInt)len;
    *count = 0;
    while (zs.avail_in && ok) {
        zs.next_out  = buf;
        zs.avail_out = sizeof(buf);
        int ret = inflate(&zs, Z_NO_FLUSH);
        for (guint8 *c = buf; c < buf + (sizeof(buf) - zs.avail_out); c++) {
            *count += *c == '\n';
        }
        if (ret == Z_STREAM_END) {
            inflateReset(&zs);
        } else if (ret != Z_OK) {
            ok = FALSE;
        }
    }
    inflateEnd(&zs);
    g_bytes_unref(data);
    if (!ok) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The index doesn't inflate");
    }
    return ok;
}

/* Reads the WACZ back: every archive byte for byte, and the index. */
static gboolean verify(const char *path, GPtrArray *archives, guint index_lines, GError **error)
{
    ArchiveZipReader *zip = archive_zip_reader_open(path, error);
    if (!zip) {
        return FALSE;
    }
    gboolean ok = TRUE;
    guint8 *buf = g_malloc(COPY_SIZE);
    for (guint i = 0; i < archives->len && ok; i++) {
        Archive *a  = g_ptr_array_index(archives, i);
        char *name  = g_strconcat("archive/", a->name, NULL);
        const ArchiveZipEntry *entry = archive_zip_find(zip, name);
        g_free(name);
        if (!entry || !entry->stored || entry->uncompressed_size != a->length) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is missing from %s",
                    a->name, path);
            ok = FALSE;
            break;
        }
        GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
        for (guint64 read = 0; read < a->length && ok;) {
            gsize want = (gsize)MIN((guint64)COPY_SIZE, a->length - read);
            ok = archive_zip_read_at(zip, entry, read, buf, want, error);
            g_checksum_update(sum, buf, (gssize)want);
            read += want;
        }
        char *hash = g_strconcat("sha256:", g_checksum_get_string(sum), NULL);
        if (ok && strcmp(hash, a->hash)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s reads back changed from %s",
                    a->name, path);
            ok = FALSE;
        }
        g_free(hash);
        g_checksum_free(sum);
    }
    g_free(buf);
    guint count = 0;
    if (ok && (!count_index_lines(zip, &count, error) || count != index_lines)) {
        if (error && !*error) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "The index of %s reads back with %u lines, not %u", path, count, index_lines);
        }
        ok = FALSE;
    }
    archive_zip_reader_free(zip);
    return ok;
}

/* The page list: the capture's own, or one made from the page loads the
 * archive records. */
static char *read_pages(const char *dir, ArchiveWarcIndex *index)
{
    char *file    = g_build_filename(dir, ARCHIVE_PAGES_FILE, NULL);
    char *content = NULL;
    gsize len     = 0;
    GStatBuf st;
    if (g_stat(file, &st) == 0 && st.st_size > 0 && st.st_size < MAX_PAGES_FILE
            && g_file_get_contents(file, &content, &len, NULL) && len
            && g_utf8_validate(content, (gssize)len, NULL)) {
        g_free(file);
        if (content[len - 1] != '\n') {
            char *ended = g_strconcat(content, "\n", NULL);
            g_free(content);
            content = ended;
        }
        return content;
    }
    g_free(content);
    g_free(file);

    GString *pages = g_string_new(ARCHIVE_PAGES_HEADER "\n");
    for (guint i = 0; i < index->navigations->len; i++) {
        ArchiveNavigation *nav = g_ptr_array_index(index->navigations, i);
        char *line = archive_page_line(nav->url, nav->date, NULL);
        g_string_append_printf(pages, "%s\n", line);
        g_free(line);
    }
    return g_string_free(pages, FALSE);
}

gboolean archive_wacz_package(const char *dir, const char *wacz_path, const char *software,
        const char *title, gboolean remove_sources, GError **error)
{
    GDir *listing = g_dir_open(dir, 0, error);
    if (!listing) {
        return FALSE;
    }
    GPtrArray *archives = g_ptr_array_new_with_free_func(archive_free);
    const char *name;
    while ((name = g_dir_read_name(listing))) {
        if (g_str_has_suffix(name, ".warc.gz")) {
            Archive *a = g_new0(Archive, 1);
            a->name    = g_strdup(name);
            a->path    = g_build_filename(dir, name, NULL);
            g_ptr_array_add(archives, a);
        }
    }
    g_dir_close(listing);
    if (!archives->len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s holds no .warc.gz files", dir);
        g_ptr_array_unref(archives);
        return FALSE;
    }
    g_ptr_array_sort(archives, compare_archives);

    ArchiveWarcIndex *index = archive_warc_index_new();
    gboolean ok = TRUE;
    for (guint i = 0; i < archives->len && ok; i++) {
        Archive *a = g_ptr_array_index(archives, i);
        GStatBuf st;
        ok = archive_index_warc(a->path, a->name, index, error) && g_stat(a->path, &st) == 0;
        if (ok) {
            a->file_length = (guint64)st.st_size;
            a->length      = index->complete_length;
            if (a->length < a->file_length) {
                g_message("archive: %s ends partway through a record; packaging its first %"
                        G_GUINT64_FORMAT " bytes of %" G_GUINT64_FORMAT, a->path, a->length,
                        a->file_length);
            }
            if (!a->length) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "%s holds no whole WARC record", a->path);
                ok = FALSE;
            }
        }
    }
    if (ok) {
        g_ptr_array_sort(index->lines, compare_strings);
        /* Once each: the same record indexed twice is one line. */
        for (guint i = 1; i < index->lines->len;) {
            if (!strcmp(g_ptr_array_index(index->lines, i - 1), g_ptr_array_index(index->lines, i))) {
                g_ptr_array_remove_index(index->lines, i);
            } else {
                i++;
            }
        }
        char *pages   = read_pages(dir, index);
        char *partial = g_strconcat(wacz_path, ".partial", NULL);
        ok = write_wacz(partial, archives, index->lines, pages, software ? software : "vimb",
                title, error)
            && verify(partial, archives, index->lines->len, error);
        if (ok && g_rename(partial, wacz_path) < 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not rename %s: %s", partial, g_strerror(errno));
            ok = FALSE;
        }
        if (!ok) {
            g_unlink(partial);
        } else if (remove_sources) {
            for (guint i = 0; i < archives->len; i++) {
                g_unlink(((Archive *)g_ptr_array_index(archives, i))->path);
            }
            char *pages_file = g_build_filename(dir, ARCHIVE_PAGES_FILE, NULL);
            g_unlink(pages_file);
            g_free(pages_file);
        }
        g_free(partial);
        g_free(pages);
    }
    archive_warc_index_free(index);
    g_ptr_array_unref(archives);
    return ok;
}
