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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "request_key.h"
#include "surt.h"

/* cdxj-indexer cuts what a body adds to the key at this many characters. */
#define MAX_BODY_QUERY     4096
/* The most a gzipped body is inflated to for its key. */
#define MAX_INFLATED_BODY  (8 * 1024 * 1024)

/* Cuts query to MAX_BODY_QUERY characters, counted as Python counts them. */
static void truncate_query(GString *query)
{
    gsize characters = 0;
    for (gsize i = 0; i < query->len; i++) {
        if (((guchar)query->str[i] & 0xc0) == 0x80) {
            continue;               /* continues a UTF-8 sequence */
        }
        if (characters++ == MAX_BODY_QUERY) {
            g_string_truncate(query, i);
            return;
        }
    }
}

/* As Python's quote_plus escapes. */
static void append_quote_plus(GString *out, const char *text)
{
    for (const guchar *c = (const guchar *)text; *c; c++) {
        if (g_ascii_isalnum(*c) || *c == '_' || *c == '.' || *c == '-' || *c == '~') {
            g_string_append_c(out, (char)*c);
        } else if (*c == ' ') {
            g_string_append_c(out, '+');
        } else {
            g_string_append_printf(out, "%%%02X", *c);
        }
    }
}

/* A float as Python's str() writes it: the shortest digits that read back
 * as the same number, positional from 1e-4 up to 1e16, scientific beyond. */
static char *python_float(double value)
{
    if (isnan(value)) {
        return g_strdup("nan");
    }
    if (isinf(value)) {
        return g_strdup(value < 0 ? "-inf" : "inf");
    }
    char buf[64], format[16];
    for (int precision = 1; precision <= 17; precision++) {
        g_snprintf(format, sizeof(format), "%%.%de", precision - 1);
        g_ascii_formatd(buf, sizeof(buf), format, value);
        if (g_ascii_strtod(buf, NULL) == value) {
            break;
        }
    }
    /* buf is "-d.ddde+XX": take its digits and exponent apart. */
    gboolean negative = buf[0] == '-';
    char *e           = strchr(buf, 'e');
    int exponent      = atoi(e + 1);
    GString *digits   = g_string_new(NULL);
    for (char *c = buf + negative; c < e; c++) {
        if (g_ascii_isdigit(*c)) {
            g_string_append_c(digits, *c);
        }
    }
    while (digits->len > 1 && digits->str[digits->len - 1] == '0') {
        g_string_truncate(digits, digits->len - 1);
    }

    GString *out = g_string_new(negative ? "-" : "");
    if (exponent >= -4 && exponent < 16) {
        if (exponent >= 0) {
            for (int i = 0; i <= exponent; i++) {
                g_string_append_c(out, i < (int)digits->len ? digits->str[i] : '0');
            }
            g_string_append_c(out, '.');
            if ((int)digits->len > exponent + 1) {
                g_string_append(out, digits->str + exponent + 1);
            } else {
                g_string_append_c(out, '0');
            }
        } else {
            g_string_append(out, "0.");
            for (int i = -1; i > exponent; i--) {
                g_string_append_c(out, '0');
            }
            g_string_append(out, digits->str);
        }
    } else {
        g_string_append_c(out, digits->str[0]);
        if (digits->len > 1) {
            g_string_append_printf(out, ".%s", digits->str + 1);
        }
        g_string_append_printf(out, "e%c%02d", exponent < 0 ? '-' : '+', ABS(exponent));
    }
    g_string_free(digits, TRUE);
    return g_string_free(out, FALSE);
}

/* The leaves of a JSON body, each named by the key that holds it, as
 * cdxj-indexer's json_parse() lists them; a name taken already gets ".2_",
 * ".3_" and so on. */
typedef struct {
    GString    *query;
    GHashTable *names;      /* taken */
    GHashTable *repeats;    /* name -> times it has been seen */
} Leaves;

static void add_leaf(Leaves *leaves, const char *name, const char *value)
{
    char *key = g_strdup(name);
    if (g_hash_table_contains(leaves->names, key)) {
        int count = GPOINTER_TO_INT(g_hash_table_lookup(leaves->repeats, name));
        count     = count ? count + 1 : 2;
        g_hash_table_insert(leaves->repeats, g_strdup(name), GINT_TO_POINTER(count));
        g_free(key);
        key = g_strdup_printf("%s.%d_", name, count);
    }
    g_hash_table_add(leaves->names, g_strdup(key));
    if (leaves->query->len) {
        g_string_append_c(leaves->query, '&');
    }
    append_quote_plus(leaves->query, key);
    g_string_append_c(leaves->query, '=');
    append_quote_plus(leaves->query, value);
    g_free(key);
}

static void add_leaves(Leaves *leaves, JsonNode *node, const char *name)
{
    switch (JSON_NODE_TYPE(node)) {
        case JSON_NODE_OBJECT: {
            JsonObjectIter iter;
            const char *key;
            JsonNode *child;
            json_object_iter_init_ordered(&iter, json_node_get_object(node));
            while (json_object_iter_next_ordered(&iter, &key, &child)) {
                add_leaves(leaves, child, key);
            }
            break;
        }
        case JSON_NODE_ARRAY: {
            JsonArray *array = json_node_get_array(node);
            for (guint i = 0; i < json_array_get_length(array); i++) {
                add_leaves(leaves, json_array_get_element(array, i), name);
            }
            break;
        }
        case JSON_NODE_NULL:
            if (*name) {
                add_leaf(leaves, name, "None");
            }
            break;
        case JSON_NODE_VALUE:
            if (!*name) {
                break;
            }
            switch (json_node_get_value_type(node)) {
                case G_TYPE_STRING:
                    add_leaf(leaves, name, json_node_get_string(node));
                    break;
                case G_TYPE_BOOLEAN:
                    add_leaf(leaves, name, json_node_get_boolean(node) ? "True" : "False");
                    break;
                case G_TYPE_INT64: {
                    char *text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
                    add_leaf(leaves, name, text);
                    g_free(text);
                    break;
                }
                case G_TYPE_DOUBLE: {
                    char *text = python_float(json_node_get_double(node));
                    add_leaf(leaves, name, text);
                    g_free(text);
                    break;
                }
                default:
                    break;
            }
            break;
    }
}

static JsonNode *parse_json(const char *text, gssize len)
{
    JsonParser *parser = json_parser_new_immutable();
    JsonNode *root     = NULL;
    json_parser_set_strict(parser, TRUE);
    if (json_parser_load_from_data(parser, text, len, NULL) && json_parser_get_root(parser)) {
        root = json_node_copy(json_parser_get_root(parser));
    }
    g_object_unref(parser);
    return root;
}

/* The leaves of body, or NULL if it isn't JSON. A body of one JSON value per
 * line is read line by line, as cdxj-indexer reads it. */
static GString *json_query(const char *body, gsize len)
{
    if (!g_utf8_validate(body, (gssize)len, NULL)) {
        return NULL;
    }
    Leaves leaves = {
        g_string_new(NULL),
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    };
    gboolean ok    = TRUE;
    JsonNode *root = parse_json(body, (gssize)len);
    if (root) {
        add_leaves(&leaves, root, "");
        json_node_unref(root);
    } else if (memchr(body, '\n', len)) {
        char *text   = g_strndup(body, len);
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l && ok; l++) {
            JsonNode *line = parse_json(*l, -1);
            if (line) {
                add_leaves(&leaves, line, "");
                json_node_unref(line);
            } else {
                ok = FALSE;
            }
        }
        g_strfreev(lines);
        g_free(text);
    } else {
        ok = FALSE;
    }
    g_hash_table_unref(leaves.names);
    g_hash_table_unref(leaves.repeats);
    if (!ok) {
        g_string_free(leaves.query, TRUE);
        return NULL;
    }
    return leaves.query;
}

static GString *base64_query(const guint8 *body, gsize len)
{
    char *encoded = g_base64_encode(body, len);
    GString *out  = g_string_new("__wb_post_data=");
    g_string_append(out, encoded);
    g_free(encoded);
    return out;
}

/* body inflated, when it is gzipped -- told by its bytes, since no form,
 * JSON or text body begins with gzip's magic -- or NULL. */
static GByteArray *inflate_if_gzipped(const guint8 *body, gsize len)
{
    if (len < 18 || body[0] != 0x1f || body[1] != 0x8b) {
        return NULL;
    }
    z_stream zs = {0};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        return NULL;
    }
    GByteArray *out = g_byte_array_new();
    guint8 buf[16384];
    int ret;
    zs.next_in  = (Bytef *)body;
    zs.avail_in = (uInt)len;
    do {
        zs.next_out  = buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        g_byte_array_append(out, buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK && out->len <= MAX_INFLATED_BODY);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END || out->len > MAX_INFLATED_BODY) {
        g_byte_array_free(out, TRUE);
        return NULL;
    }
    return out;
}

/* A form, decoded once as cdxj-indexer decodes it, then escaped again so
 * that the key, which decodes it in turn, reads the same fields -- all but
 * "#", which cdxj-indexer leaves in, cutting the key's query there. */
static GString *form_query(const guint8 *body, gsize len, gboolean whole)
{
    GString *decoded = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        if (body[i] == '+') {
            g_string_append_c(decoded, ' ');
        } else if (body[i] == '%' && i + 2 < len && g_ascii_isxdigit(body[i + 1])
                && g_ascii_isxdigit(body[i + 2])) {
            g_string_append_c(decoded, (char)(g_ascii_xdigit_value(body[i + 1]) * 16
                        + g_ascii_xdigit_value(body[i + 2])));
            i += 2;
        } else {
            g_string_append_c(decoded, (char)body[i]);
        }
    }
    if (!whole) {
        truncate_query(decoded);
    }
    GString *out = g_string_sized_new(decoded->len);
    for (gsize i = 0; i < decoded->len; i++) {
        guchar c = (guchar)decoded->str[i];
        if (g_ascii_isalnum(c) || c == '&' || c == '=' || c == '#') {
            g_string_append_c(out, (char)c);
        } else {
            g_string_append_printf(out, "%%%02X", c);
        }
    }
    g_string_free(decoded, TRUE);
    return out;
}

char *archive_request_body_query(const char *content_type, const guint8 *body,
        gsize body_len, gboolean whole)
{
    char *type          = g_ascii_strdown(content_type ? content_type : "", -1);
    GByteArray *inflated = inflate_if_gzipped(body, body_len);
    GString *query      = NULL;
    gboolean cut        = !whole;

    if (inflated) {
        body     = inflated->data;
        body_len = inflated->len;
    }
    if (g_str_has_prefix(type, "application/x-www-form-urlencoded")) {
        if (g_utf8_validate((const char *)body, (gssize)body_len, NULL)) {
            query = form_query(body, body_len, whole);
            cut   = FALSE;              /* cut while decoded */
        } else {
            query = base64_query(body, body_len);
            cut   = FALSE;              /* as the reference does: not cut */
        }
    } else if (g_str_has_prefix(type, "multipart/")) {
        query = strstr(type, "boundary=") ? g_string_new(NULL) : base64_query(body, body_len);
    } else if (g_str_has_prefix(type, "application/json")) {
        query = json_query((const char *)body, body_len);
        if (!query) {
            query = g_string_new(NULL);
        }
    } else if (g_str_has_prefix(type, "text/plain")) {
        query = json_query((const char *)body, body_len);
        if (!query) {
            query = base64_query(body, body_len);
        }
    } else {
        query = base64_query(body, body_len);
    }
    if (cut) {
        truncate_query(query);
    }
    if (inflated) {
        g_byte_array_free(inflated, TRUE);
    }
    g_free(type);
    return g_string_free(query, FALSE);
}

char *archive_request_key(const char *url, const char *method, const char *content_type,
        const guint8 *body, gssize body_len)
{
    if (!strcmp(method, "GET")) {
        return archive_surt(url);
    }
    const char *hash = strchr(url, '#');
    GString *spec    = g_string_new_len(url, hash ? hash - url : (gssize)strlen(url));
    char *upper      = g_ascii_strup(method, -1);
    g_string_append_printf(spec, "%s__wb_method=%s", strchr(spec->str, '?') ? "&" : "?", upper);
    g_free(upper);
    if ((!strcmp(method, "POST") || !strcmp(method, "PUT")) && body_len >= 0) {
        char *query = archive_request_body_query(content_type, body, (gsize)body_len, FALSE);
        if (*query) {
            g_string_append_printf(spec, "&%s", query);
        }
        g_free(query);
    }
    char *key = archive_surt(spec->str);
    g_string_free(spec, TRUE);
    return key;
}

/* ---- body fields ---------------------------------------------------------- */

#define PROTOBUF_DEPTH_MAX  8
#define PROTOBUF_LEAVES_MAX 4000

static gboolean read_varint(const guint8 *b, gsize len, gsize *at, guint64 *value)
{
    *value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*at >= len) {
            return FALSE;
        }
        guint8 byte = b[(*at)++];
        *value |= (guint64)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return TRUE;
        }
    }
    return FALSE;
}

static char *hex_of(const guint8 *b, gsize len)
{
    GString *s = g_string_sized_new(len * 2);
    for (gsize i = 0; i < len; i++) {
        g_string_append_printf(s, "%02X", b[i]);
    }
    return g_string_free(s, FALSE);
}

/* The leaves of bytes read as a protobuf message, or FALSE if they aren't
 * one: every field well formed, and the message ending where the bytes do.
 * A length-delimited field is read as a message within it where it is one,
 * and as its bytes where it isn't. */
static gboolean protobuf_leaves(const guint8 *b, gsize len, const char *path, int depth,
        GPtrArray *leaves)
{
    gsize at = 0;
    gboolean any = FALSE;
    while (at < len) {
        guint64 tag, value;
        if (!read_varint(b, len, &at, &tag) || !(tag >> 3) || leaves->len > PROTOBUF_LEAVES_MAX) {
            return FALSE;
        }
        char *name = *path ? g_strdup_printf("%s.%" G_GUINT64_FORMAT, path, tag >> 3)
            : g_strdup_printf("%" G_GUINT64_FORMAT, tag >> 3);
        gboolean ok = TRUE;
        switch (tag & 7) {
            case 0:
                ok = read_varint(b, len, &at, &value);
                if (ok) {
                    g_ptr_array_add(leaves, g_strdup_printf("%s=%" G_GUINT64_FORMAT, name, value));
                }
                break;
            case 1:
            case 5: {
                gsize width = (tag & 7) == 1 ? 8 : 4;
                ok = len - at >= width;
                if (ok) {
                    char *hex = hex_of(b + at, width);
                    g_ptr_array_add(leaves, g_strdup_printf("%s=%s", name, hex));
                    g_free(hex);
                    at += width;
                }
                break;
            }
            case 2: {
                ok = read_varint(b, len, &at, &value) && value <= len - at;
                if (ok) {
                    const guint8 *inner = b + at;
                    gsize n = (gsize)value;
                    at += n;
                    GPtrArray *nested = g_ptr_array_new_with_free_func(g_free);
                    if (depth < PROTOBUF_DEPTH_MAX && n
                            && protobuf_leaves(inner, n, name, depth + 1, nested)) {
                        for (guint i = 0; i < nested->len; i++) {
                            g_ptr_array_add(leaves, g_strdup(g_ptr_array_index(nested, i)));
                        }
                    } else {
                        char *hex = hex_of(inner, n);
                        g_ptr_array_add(leaves, g_strdup_printf("%s=%s", name, hex));
                        g_free(hex);
                    }
                    g_ptr_array_unref(nested);
                }
                break;
            }
            default:
                ok = FALSE;
        }
        g_free(name);
        if (!ok) {
            return FALSE;
        }
        any = TRUE;
    }
    return any;
}

char **archive_request_body_fields(const char *body_query)
{
    static const char bytes_prefix[] = "__wb_post_data=";
    GPtrArray *fields = g_ptr_array_new_with_free_func(g_free);
    if (g_str_has_prefix(body_query, bytes_prefix)) {
        gsize len;
        guint8 *bytes = g_base64_decode(body_query + strlen(bytes_prefix), &len);
        GPtrArray *leaves = g_ptr_array_new_with_free_func(g_free);
        if (len && protobuf_leaves(bytes, len, "", 0, leaves)) {
            g_ptr_array_unref(fields);
            fields = leaves;
        } else {
            g_ptr_array_unref(leaves);
            g_ptr_array_add(fields, g_strdup(body_query));
        }
        g_free(bytes);
    } else {
        char **terms = g_strsplit(body_query, "&", -1);
        for (char **t = terms; *t; t++) {
            if (**t) {
                g_ptr_array_add(fields, g_strdup(*t));
            }
        }
        g_strfreev(terms);
    }
    g_ptr_array_set_free_func(fields, NULL);
    g_ptr_array_add(fields, NULL);
    return (char **)g_ptr_array_free(fields, FALSE);
}
