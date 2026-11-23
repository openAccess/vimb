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

#include <stdlib.h>
#include <string.h>

#include "surt.h"

/* What a browser would have escaped before parsing: spaces, controls and
 * anything past ASCII. GUri, unlike a browser, won't take them raw. */
static char *escape_unparseable(const char *url)
{
    GString *out = g_string_sized_new(strlen(url));
    for (const guchar *c = (const guchar *)url; *c; c++) {
        if (*c <= 0x20 || *c >= 0x7f) {
            g_string_append_printf(out, "%%%02X", *c);
        } else {
            g_string_append_c(out, (char)*c);
        }
    }
    return g_string_free(out, FALSE);
}

/* Decodes every escape, and again until nothing changes: "%2561" is "%41"
 * is "a", and a URL escaped twice should not hide what it names. Bounded:
 * four passes is far past any real URL. */
static char *decode_escapes(const char *text)
{
    char *out = g_strdup(text);
    for (int pass = 0; pass < 4; pass++) {
        GString *next   = g_string_sized_new(strlen(out));
        gboolean change = FALSE;
        for (const char *c = out; *c; c++) {
            if (c[0] == '%' && g_ascii_isxdigit(c[1]) && g_ascii_isxdigit(c[2])) {
                g_string_append_c(next,
                        (char)(g_ascii_xdigit_value(c[1]) * 16 + g_ascii_xdigit_value(c[2])));
                c     += 2;
                change = TRUE;
            } else {
                g_string_append_c(next, *c);
            }
        }
        g_free(out);
        out = g_string_free(next, FALSE);
        if (!change) {
            break;
        }
    }
    return out;
}

/* Escapes only what can't be written plainly: controls, spaces, non-ASCII,
 * and the "%" and "#" that would read as something else. A site's "|" and
 * "," stay literal -- escaped, they made a replay of Wikipedia miss every
 * stylesheet (see the Chromium fork's surt.cc). */
static void append_escaped(GString *out, const char *text, gsize len)
{
    for (gsize i = 0; i < len; i++) {
        guchar c = (guchar)text[i];
        if (c <= 0x20 || c >= 0x7f || c == '%' || c == '#') {
            g_string_append_printf(out, "%%%02x", c);
        } else {
            g_string_append_c(out, (char)c);
        }
    }
}

/* "www.example.org" and "www2.example.org" are keyed with "example.org". */
static const char *strip_www(const char *host)
{
    if (strncmp(host, "www", 3)) {
        return host;
    }
    const char *c = host + 3;
    while (g_ascii_isdigit(*c)) {
        c++;
    }
    return *c == '.' ? c + 1 : host;
}

/* "example.org" -> "org,example". An IPv6 literal is left alone: its colons
 * are no hierarchy. */
static char *reverse_host(const char *host)
{
    if (strchr(host, ':')) {
        return g_strdup(host);
    }
    char **labels = g_strsplit(host, ".", -1);
    GString *out  = g_string_new(NULL);
    guint n       = g_strv_length(labels);
    for (guint i = n; i-- > 0;) {
        if (*labels[i]) {
            if (out->len) {
                g_string_append_c(out, ',');
            }
            g_string_append(out, labels[i]);
        }
    }
    g_strfreev(labels);
    return g_string_free(out, FALSE);
}

/* By name; a name alone before the same name with a value; then by value.
 * Not the order of the terms as strings: "page" sorts before "page2". */
static int compare_terms(const void *pa, const void *pb)
{
    const char *a  = *(const char *const *)pa;
    const char *b  = *(const char *const *)pb;
    const char *ae = strchr(a, '=');
    const char *be = strchr(b, '=');
    size_t an      = ae ? (size_t)(ae - a) : strlen(a);
    size_t bn      = be ? (size_t)(be - b) : strlen(b);
    int c          = memcmp(a, b, MIN(an, bn));

    if (c) {
        return c;
    }
    if (an != bn) {
        return an < bn ? -1 : 1;
    }
    if (!ae != !be) {
        return !ae ? -1 : 1;
    }
    return strcmp(a, b);
}

/* Decoded, lowercased, split on "&" -- after decoding, so an escaped "&"
 * divides where the decoded URL does -- and sorted. Every term is kept as it
 * is, empty ones and spaces included, as the reference keeps them. */
static char *normalize_query(const char *query)
{
    char *decoded = decode_escapes(query);
    char *lower   = g_ascii_strdown(decoded, -1);
    char **terms  = g_strsplit(lower, "&", -1);
    guint n       = g_strv_length(terms);
    g_free(decoded);
    g_free(lower);

    for (guint i = 0; i < n; i++) {
        GString *escaped = g_string_new(NULL);
        append_escaped(escaped, terms[i], strlen(terms[i]));
        g_free(terms[i]);
        terms[i] = g_string_free(escaped, FALSE);
    }
    qsort(terms, n, sizeof(char *), compare_terms);
    char *joined = g_strjoinv("&", terms);
    g_strfreev(terms);
    return joined;
}

/* RFC 3986's remove_dot_segments, which a browser applies before it asks
 * for a URL. */
static char *remove_dot_segments(const char *path)
{
    GString *out   = g_string_new(NULL);
    const char *in = path;

    while (*in) {
        if (g_str_has_prefix(in, "../")) {
            in += 3;
        } else if (g_str_has_prefix(in, "./")) {
            in += 2;
        } else if (g_str_has_prefix(in, "/./")) {
            in += 2;
        } else if (!strcmp(in, "/.")) {
            in += 1;
            g_string_append_c(out, '/');
            break;
        } else if (g_str_has_prefix(in, "/../") || !strcmp(in, "/..")) {
            gboolean last = !strcmp(in, "/..");
            in += 3;
            char *slash = strrchr(out->str, '/');
            g_string_truncate(out, slash ? (gsize)(slash - out->str) : 0);
            if (last) {
                g_string_append_c(out, '/');
                break;
            }
        } else if (!strcmp(in, ".") || !strcmp(in, "..")) {
            break;
        } else {
            const char *next = strchr(in + 1, '/');
            gsize len        = next ? (gsize)(next - in) : strlen(in);
            g_string_append_len(out, in, (gssize)len);
            in += len;
        }
    }
    return g_string_free(out, FALSE);
}

static int default_port(const char *scheme)
{
    if (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "ws")) {
        return 80;
    }
    if (!g_ascii_strcasecmp(scheme, "https") || !g_ascii_strcasecmp(scheme, "wss")) {
        return 443;
    }
    if (!g_ascii_strcasecmp(scheme, "ftp")) {
        return 21;
    }
    return -1;
}

char *archive_surt(const char *url)
{
    char *prepared = escape_unparseable(url);
    GUri *uri      = g_uri_parse(prepared, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED,
            NULL);
    g_free(prepared);
    if (!uri) {
        return NULL;
    }

    char *result     = NULL;
    const char *raw  = g_uri_get_host(uri);
    char *host       = raw ? g_ascii_strdown(raw, -1) : NULL;
    if (host && *host) {
        /* A trailing dot names the same host. */
        gsize len = strlen(host);
        if (len && host[len - 1] == '.') {
            host[len - 1] = '\0';
        }
        char *reversed = reverse_host(strip_www(host));
        if (*reversed) {
            GString *key = g_string_new(reversed);
            int port     = g_uri_get_port(uri);
            if (port > 0 && port != default_port(g_uri_get_scheme(uri))) {
                g_string_append_printf(key, ":%d", port);
            }

            char *resolved = remove_dot_segments(g_uri_get_path(uri));
            char *decoded  = decode_escapes(resolved);
            char *path     = g_ascii_strdown(decoded, -1);
            g_free(resolved);
            g_free(decoded);

            /* Empty segments name nothing; a trailing slash names the same
             * directory -- except the root, which would leave no path. */
            GString *collapsed = g_string_new(NULL);
            for (const char *c = path; *c; c++) {
                if (*c == '/' && collapsed->len && collapsed->str[collapsed->len - 1] == '/') {
                    continue;
                }
                g_string_append_c(collapsed, *c);
            }
            while (collapsed->len > 1 && collapsed->str[collapsed->len - 1] == '/') {
                g_string_truncate(collapsed, collapsed->len - 1);
            }
            if (!collapsed->len) {
                g_string_append_c(collapsed, '/');
            }
            g_string_append_c(key, ')');
            append_escaped(key, collapsed->str, collapsed->len);
            g_string_free(collapsed, TRUE);
            g_free(path);

            const char *query = g_uri_get_query(uri);
            if (query) {
                char *normalized = normalize_query(query);
                if (*normalized) {
                    g_string_append_printf(key, "?%s", normalized);
                }
                g_free(normalized);
            }
            result = g_string_free(key, FALSE);
        }
        g_free(reversed);
    }
    g_free(host);
    g_uri_unref(uri);
    return result;
}
