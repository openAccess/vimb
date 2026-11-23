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

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <string.h>
#include <zlib.h>

#include "../../src/archive/ca.h"
#include "../../src/archive/http1.h"
#include "../../src/archive/netcheck.h"
#include "../../src/archive/proxy.h"
#include "../../src/archive/spool.h"
#include "../../src/archive/warc.h"

/* ---- helpers ----------------------------------------------------------- */

static void collect(const guint8 *data, gsize len, gpointer user_data)
{
    g_string_append_len(user_data, (const char *)data, (gssize)len);
}

typedef struct {
    Http1Parser *parser;
    GString     *payload;
    GString     *raw;
} Parsed;

static Parsed *parsed_new(Http1Kind kind)
{
    Parsed *p  = g_new0(Parsed, 1);
    p->parser  = http1_parser_new(kind, 64 * 1024);
    p->payload = g_string_new(NULL);
    p->raw     = g_string_new(NULL);
    http1_parser_set_payload_func(p->parser, collect, p->payload);
    http1_parser_set_raw_body_func(p->parser, collect, p->raw);
    return p;
}

static void parsed_free(Parsed *p)
{
    http1_parser_free(p->parser);
    g_string_free(p->payload, TRUE);
    g_string_free(p->raw, TRUE);
    g_free(p);
}

/* Feeds text a byte at a time, the hardest way it can arrive; returns how
 * much the message took. */
static gsize feed_bytewise(Parsed *p, const char *text)
{
    gsize len = strlen(text), used = 0;
    for (gsize i = 0; i < len && !http1_parser_is_done(p->parser); i++) {
        GError *error = NULL;
        gssize n = http1_parser_feed(p->parser, (const guint8 *)text + i, 1, &error);
        g_assert_no_error(error);
        used += (gsize)n;
    }
    return used;
}

static char *make_temp_dir(void)
{
    GError *error = NULL;
    char *dir = g_dir_make_tmp("vimb-archive-test-XXXXXX", &error);
    g_assert_no_error(error);
    return dir;
}

static void remove_tree(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            remove_tree(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_remove(path);
}

/* Every gzip member of a WARC file, decompressed, one after another. */
static GString *read_warc(const char *path)
{
    char *content;
    gsize len;
    GError *error = NULL;
    g_file_get_contents(path, &content, &len, &error);
    g_assert_no_error(error);

    GString *out = g_string_new(NULL);
    z_stream zs  = {0};
    guint8 buf[16384];
    g_assert_cmpint(inflateInit2(&zs, 15 + 16), ==, Z_OK);
    zs.next_in  = (Bytef *)content;
    zs.avail_in = (uInt)len;
    while (zs.avail_in) {
        zs.next_out  = buf;
        zs.avail_out = sizeof(buf);
        int ret = inflate(&zs, Z_NO_FLUSH);
        g_assert_true(ret == Z_OK || ret == Z_STREAM_END);
        g_string_append_len(out, (char *)buf, (gssize)(sizeof(buf) - zs.avail_out));
        if (ret == Z_STREAM_END) {
            inflateReset(&zs);
        }
    }
    inflateEnd(&zs);
    g_free(content);
    return out;
}

/* ---- http1 ------------------------------------------------------------- */

static void test_http1_length(void)
{
    const char *text = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\n"
        "helloHTTP/1.1 204 No Content\r\n\r\n";
    Parsed *p  = parsed_new(HTTP1_RESPONSE);
    gsize used = feed_bytewise(p, text);

    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_cmpuint(used, ==, strlen(text) - strlen("HTTP/1.1 204 No Content\r\n\r\n"));
    g_assert_cmpstr(p->payload->str, ==, "hello");
    g_assert_cmpstr(p->raw->str, ==, "hello");
    const Http1Head *head = http1_parser_head(p->parser);
    g_assert_cmpuint(head->status, ==, 200);
    g_assert_cmpstr(head->reason, ==, "OK");
    g_assert_cmpstr(http1_head_get(head, "content-type"), ==, "text/plain");
    g_assert_true(http1_parser_keep_alive(p->parser));
    g_assert_cmpuint(http1_parser_raw_head(p->parser)->len, ==, used - 5);
    parsed_free(p);
}

static void test_http1_chunked(void)
{
    const char *body = "4;ext=1\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-Trailer: yes\r\n\r\n";
    char *text = g_strconcat("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", body,
            "NEXT", NULL);
    Parsed *p  = parsed_new(HTTP1_RESPONSE);
    gsize used = feed_bytewise(p, text);

    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_cmpuint(used, ==, strlen(text) - 4);
    g_assert_cmpint(http1_parser_body_kind(p->parser), ==, HTTP1_BODY_CHUNKED);
    g_assert_cmpstr(p->payload->str, ==, "Wikipedia in\r\n\r\nchunks.");
    g_assert_cmpstr(p->raw->str, ==, body);
    parsed_free(p);
    g_free(text);
}

static void test_http1_until_close(void)
{
    Parsed *p     = parsed_new(HTTP1_RESPONSE);
    GError *error = NULL;
    feed_bytewise(p, "HTTP/1.0 200 OK\r\n\r\nall of it");
    g_assert_false(http1_parser_is_done(p->parser));
    g_assert_true(http1_parser_eof(p->parser, &error));
    g_assert_no_error(error);
    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_cmpstr(p->payload->str, ==, "all of it");
    g_assert_false(http1_parser_keep_alive(p->parser));
    parsed_free(p);
}

static void test_http1_no_body(void)
{
    /* A HEAD's answer gives the length the GET's would have had. */
    Parsed *p = parsed_new(HTTP1_RESPONSE);
    http1_parser_set_request_method(p->parser, "HEAD");
    feed_bytewise(p, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_cmpint(http1_parser_body_kind(p->parser), ==, HTTP1_BODY_NONE);

    http1_parser_reset(p->parser);
    feed_bytewise(p, "HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\n\r\n");
    g_assert_true(http1_parser_is_done(p->parser));

    /* An interim response is a message of its own. */
    http1_parser_set_request_method(p->parser, "GET");
    http1_parser_reset(p->parser);
    feed_bytewise(p, "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n");
    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_cmpuint(http1_parser_head(p->parser)->status, ==, 103);
    parsed_free(p);
}

static void test_http1_request(void)
{
    Parsed *p = parsed_new(HTTP1_REQUEST);
    feed_bytewise(p, "\r\nPOST http://example.com/a?b=1 HTTP/1.1\r\nHost: example.com\r\n"
            "X-Folded: one\r\n two\r\nContent-Length: 3\r\nConnection: Keep-Alive, close\r\n\r\nabc");
    const Http1Head *head = http1_parser_head(p->parser);
    g_assert_cmpstr(head->method, ==, "POST");
    g_assert_cmpstr(head->target, ==, "http://example.com/a?b=1");
    g_assert_cmpstr(http1_head_get(head, "X-Folded"), ==, "one two");
    g_assert_cmpstr(p->raw->str, ==, "abc");
    g_assert_true(http1_head_has_token(head, "connection", "CLOSE"));
    g_assert_false(http1_parser_keep_alive(p->parser));
    /* The empty line before the request is not part of its head. */
    g_assert_cmpint(http1_parser_raw_head(p->parser)->data[0], ==, 'P');
    parsed_free(p);

    p = parsed_new(HTTP1_REQUEST);
    feed_bytewise(p, "GET / HTTP/1.0\r\n\r\n");
    g_assert_true(http1_parser_is_done(p->parser));
    g_assert_false(http1_parser_keep_alive(p->parser));
    parsed_free(p);
}

static void expect_error(Http1Kind kind, const char *text, int code)
{
    Http1Parser *parser = http1_parser_new(kind, 128);
    GError *error       = NULL;
    gssize n = http1_parser_feed(parser, (const guint8 *)text, strlen(text), &error);
    g_assert_cmpint(n, ==, -1);
    g_assert_error(error, HTTP1_ERROR, code);
    g_clear_error(&error);
    http1_parser_free(parser);
}

static void test_http1_errors(void)
{
    expect_error(HTTP1_REQUEST, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
            "Content-Length: 4\r\n\r\n", HTTP1_ERROR_MALFORMED);
    expect_error(HTTP1_RESPONSE, "HTTP/1.1 200 OK\r\nContent-Length: 4, 5\r\n\r\n",
            HTTP1_ERROR_MALFORMED);
    expect_error(HTTP1_RESPONSE, "HTTP/2 200\r\n\r\n", HTTP1_ERROR_MALFORMED);
    expect_error(HTTP1_RESPONSE, "SSH-2.0-OpenSSH_9.9\r\n\r\n", HTTP1_ERROR_MALFORMED);
    expect_error(HTTP1_RESPONSE, "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n", HTTP1_ERROR_MALFORMED);
    expect_error(HTTP1_RESPONSE, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
            HTTP1_ERROR_MALFORMED);
    char *big = g_strdup_printf("HTTP/1.1 200 OK\r\nX: %0200d\r\n\r\n", 0);
    expect_error(HTTP1_RESPONSE, big, HTTP1_ERROR_TOO_LARGE);
    g_free(big);

    /* Cut short within a body. */
    Http1Parser *parser = http1_parser_new(HTTP1_RESPONSE, 1024);
    GError *error       = NULL;
    const char *text    = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
    http1_parser_feed(parser, (const guint8 *)text, strlen(text), &error);
    g_assert_false(http1_parser_eof(parser, &error));
    g_assert_error(error, HTTP1_ERROR, HTTP1_ERROR_TRUNCATED);
    g_clear_error(&error);
    http1_parser_free(parser);
}

/* ---- spool ------------------------------------------------------------- */

static gboolean append_piece(const guint8 *data, gsize len, gpointer user_data, GError **error)
{
    g_string_append_len(user_data, (const char *)data, (gssize)len);
    return TRUE;
}

static void test_spool(void)
{
    char *dir = make_temp_dir();
    for (int limit = 0; limit <= 1; limit++) {
        /* Kept in memory, then spilled to a file after 10 bytes. */
        Spool *spool  = spool_new(dir, limit ? G_MAXSIZE : 10);
        GError *error = NULL;
        GString *all  = g_string_new(NULL);
        for (int i = 0; i < 1000; i++) {
            char piece[16];
            g_snprintf(piece, sizeof(piece), "%d,", i);
            g_string_append(all, piece);
            g_assert_true(spool_append(spool, piece, strlen(piece), &error));
        }
        GString *back = g_string_new(NULL);
        g_assert_true(spool_foreach(spool, append_piece, back, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(spool_length(spool), ==, all->len);
        g_assert_cmpstr(back->str, ==, all->str);
        g_assert_null(spool_get_bytes(spool, 10, NULL));
        GBytes *bytes = spool_get_bytes(spool, all->len, &error);
        g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), all->str, all->len);
        g_bytes_unref(bytes);
        g_string_free(back, TRUE);
        g_string_free(all, TRUE);
        spool_free(spool);
    }
    /* A spilled spool's file is unlinked at once. */
    GDir *d = g_dir_open(dir, 0, NULL);
    g_assert_null(g_dir_read_name(d));
    g_dir_close(d);
    remove_tree(dir);
    g_free(dir);
}

/* ---- warc -------------------------------------------------------------- */

static void test_warc_formats(void)
{
    guint8 empty_sha1[20];
    gsize len = sizeof(empty_sha1);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_get_digest(sum, empty_sha1, &len);
    g_checksum_free(sum);
    char *label = warc_label_sha1(empty_sha1);
    g_assert_cmpstr(label, ==, "sha1:3I42H3S6NNFQ2MSVX7XZKYAYSCX5QBYJ");
    g_free(label);

    GDateTime *date = g_date_time_new_utc(2026, 11, 23, 19, 27, 14.055123);
    char *text = warc_format_date(date);
    g_assert_cmpstr(text, ==, "2026-11-23T19:27:14.055123Z");
    g_free(text);
    g_date_time_unref(date);

    char *id = warc_new_record_id();
    g_assert_true(g_str_has_prefix(id, "<urn:uuid:") && g_str_has_suffix(id, ">"));
    g_assert_cmpuint(strlen(id), ==, strlen("<urn:uuid:>") + 36);
    g_free(id);
}

static void test_warc_writer(void)
{
    char *dir     = make_temp_dir();
    char *path    = g_build_filename(dir, "t.warc.gz", NULL);
    GError *error = NULL;

    WarcWriter *writer = warc_writer_open(path, "test", &error);
    g_assert_no_error(error);
    g_assert_null(warc_writer_open(path, "test", NULL));      /* never overwrites */

    const char *http = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    Spool *block = spool_new(dir, 1024);
    spool_append(block, http, strlen(http), NULL);
    WarcRecordInfo info = {
        .type = "response", .target_uri = "https://example.com/",
        .content_type = "application/http;msgtype=response", .ip_address = "192.0.2.1",
    };
    char *id;
    guint64 offset, length;
    g_assert_true(warc_writer_write(writer, &info, block, &id, &offset, &length, &error));
    WarcRecordInfo bad = {.type = "response", .target_uri = "https://example.com/\r\nX: y"};
    g_assert_false(warc_writer_write(writer, &bad, NULL, NULL, NULL, NULL, &error));
    g_clear_error(&error);
    g_assert_true(warc_writer_close(writer, &error));
    spool_free(block);

    /* The record is one gzip member, found where it was said to be. */
    char *content;
    gsize size;
    g_file_get_contents(path, &content, &size, NULL);
    g_assert_cmpuint(offset + length, ==, size);
    guint8 out[4096];
    z_stream zs = {0};
    inflateInit2(&zs, 15 + 16);
    zs.next_in   = (Bytef *)content + offset;
    zs.avail_in  = (uInt)length;
    zs.next_out  = out;
    zs.avail_out = sizeof(out);
    g_assert_cmpint(inflate(&zs, Z_FINISH), ==, Z_STREAM_END);
    char *record = g_strndup((char *)out, sizeof(out) - zs.avail_out);
    inflateEnd(&zs);
    g_assert_true(g_str_has_prefix(record, "WARC/1.1\r\nWARC-Type: response\r\n"));
    g_assert_nonnull(strstr(record, id));
    g_assert_nonnull(strstr(record, "WARC-Warcinfo-ID: <urn:uuid:"));
    g_assert_nonnull(strstr(record, "WARC-IP-Address: 192.0.2.1\r\n"));
    g_assert_nonnull(strstr(record, "WARC-Block-Digest: sha1:"));
    char *expect_length = g_strdup_printf("Content-Length: %zu\r\n\r\n%s\r\n\r\n", strlen(http), http);
    g_assert_true(g_str_has_suffix(record, expect_length));

    GString *all = read_warc(path);
    g_assert_true(g_str_has_prefix(all->str, "WARC/1.1\r\nWARC-Type: warcinfo\r\n"));
    g_assert_nonnull(strstr(all->str, "WARC-Filename: t.warc.gz\r\n"));
    g_string_free(all, TRUE);

    g_free(expect_length);
    g_free(record);
    g_free(content);
    g_free(id);
    remove_tree(dir);
    g_free(path);
    g_free(dir);
}

/* ---- ca ---------------------------------------------------------------- */

static gnutls_x509_crt_t import_cert(const char *pem)
{
    gnutls_x509_crt_t crt;
    gnutls_datum_t data = {(unsigned char *)pem, (unsigned int)strlen(pem)};
    gnutls_x509_crt_init(&crt);
    g_assert_cmpint(gnutls_x509_crt_import(crt, &data, GNUTLS_X509_FMT_PEM), ==, 0);
    return crt;
}

static void test_ca(void)
{
    char *dir     = make_temp_dir();
    GError *error = NULL;
    ArchiveCa *ca = archive_ca_open(dir, &error);
    g_assert_no_error(error);

    GStatBuf st;
    g_assert_cmpint(g_stat(archive_ca_file(ca), &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    gnutls_x509_crt_t authority = import_cert(archive_ca_pem(ca));
    const char *hosts[] = {"Example.COM", "198.51.100.7", "2001:db8::1"};
    for (gsize i = 0; i < G_N_ELEMENTS(hosts); i++) {
        GTlsCertificate *cert = archive_ca_cert_for_host(ca, hosts[i], &error);
        g_assert_no_error(error);
        GTlsCertificate *again = archive_ca_cert_for_host(ca, hosts[i], &error);
        g_assert_true(cert == again);                       /* cached */
        g_object_unref(again);

        char *pem;
        g_object_get(cert, "certificate-pem", &pem, NULL);
        gnutls_x509_crt_t crt = import_cert(pem);
        unsigned int status = 0;
        g_assert_cmpint(gnutls_x509_crt_verify(crt, &authority, 1, 0, &status), ==, 0);
        g_assert_cmpuint(status, ==, 0);
        if (i == 0) {
            g_assert_true(gnutls_x509_crt_check_hostname(crt, "example.com"));
            g_assert_false(gnutls_x509_crt_check_hostname(crt, "www.example.com"));
        } else {
            GInetAddress *ip = g_inet_address_new_from_string(hosts[i]);
            g_assert_true(gnutls_x509_crt_check_ip(crt, g_inet_address_to_bytes(ip),
                        g_inet_address_get_native_size(ip), 0));
            g_object_unref(ip);
        }
        gnutls_x509_crt_deinit(crt);
        g_free(pem);
        g_object_unref(cert);
    }

    /* Opening the directory again finds the same authority. */
    char *first = g_strdup(archive_ca_pem(ca));
    archive_ca_free(ca);
    ca = archive_ca_open(dir, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(archive_ca_pem(ca), ==, first);
    archive_ca_free(ca);

    gnutls_x509_crt_deinit(authority);
    g_free(first);
    remove_tree(dir);
    g_free(dir);
}

/* ---- netcheck ---------------------------------------------------------- */

static void test_internal_addresses(void)
{
    const struct { const char *address; gboolean internal; } cases[] = {
        {"127.0.0.1", TRUE}, {"10.1.2.3", TRUE}, {"172.16.0.1", TRUE}, {"172.31.255.255", TRUE},
        {"172.32.0.1", FALSE}, {"192.168.1.1", TRUE}, {"169.254.169.254", TRUE},
        {"100.64.0.1", TRUE}, {"100.128.0.1", FALSE}, {"0.0.0.0", TRUE}, {"224.0.0.1", TRUE},
        {"8.8.8.8", FALSE}, {"93.184.215.14", FALSE},
        {"::1", TRUE}, {"::", TRUE}, {"fe80::1", TRUE}, {"fc00::1", TRUE}, {"fd12:3456::1", TRUE},
        {"ff02::1", TRUE}, {"2606:4700::1111", FALSE}, {"::ffff:10.0.0.1", TRUE},
        {"::ffff:8.8.8.8", FALSE}, {"64:ff9b::7f00:1", TRUE},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        GInetAddress *a = g_inet_address_new_from_string(cases[i].address);
        g_assert_nonnull(a);
        if (archive_address_is_internal(a) != cases[i].internal) {
            g_error("%s should %sbe internal", cases[i].address, cases[i].internal ? "" : "not ");
        }
        g_object_unref(a);
    }
}

/* ---- the proxy, against an origin server of our own --------------------- */

/* Exactly what the origin sends for /chunked: to be found in the WARC as is. */
static const char chunked_response[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Order: 1\r\nx-order: 2\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "7\r\nchunked\r\n6\r\n body!\r\n0\r\n\r\n";

static gboolean origin_run(GThreadedSocketService *service, GSocketConnection *connection,
        GObject *source, gpointer data)
{
    GInputStream *in   = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    Http1Parser *parser = http1_parser_new(HTTP1_REQUEST, 64 * 1024);
    guint8 buf[4096];
    while (!http1_parser_is_done(parser)) {
        gssize n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL);
        if (n <= 0 || http1_parser_feed(parser, buf, (gsize)n, NULL) < 0) {
            http1_parser_free(parser);
            return TRUE;
        }
    }
    const char *target = http1_parser_head(parser)->target;
    const char *reply  = !strcmp(target, "/chunked") ? chunked_response
        : "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    g_output_stream_write_all(out, reply, strlen(reply), NULL, NULL, NULL);
    http1_parser_free(parser);
    return TRUE;
}

/* The origin runs on a thread of its own, with the main loop its accepting
 * needs, so that the test thread can block on the proxy. */
typedef struct {
    GThread      *thread;
    GMainContext *context;
    GMainLoop    *loop;
    guint16      port;
    GMutex       lock;
    GCond        cond;
    gboolean     ready;
} Origin;

static gpointer origin_thread(gpointer data)
{
    Origin *o     = data;
    GError *error = NULL;
    g_main_context_push_thread_default(o->context);
    GSocketService *service = g_threaded_socket_service_new(4);
    g_signal_connect(service, "run", G_CALLBACK(origin_run), NULL);
    guint16 port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(service), NULL, &error);
    g_assert_no_error(error);
    g_socket_service_start(service);
    g_mutex_lock(&o->lock);
    o->port  = port;
    o->ready = TRUE;
    g_cond_signal(&o->cond);
    g_mutex_unlock(&o->lock);
    g_main_loop_run(o->loop);
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    g_object_unref(service);
    g_main_context_pop_thread_default(o->context);
    return NULL;
}

static void origin_start(Origin *o)
{
    o->context = g_main_context_new();
    o->loop    = g_main_loop_new(o->context, FALSE);
    g_mutex_init(&o->lock);
    g_cond_init(&o->cond);
    o->thread = g_thread_new("origin", origin_thread, o);
    g_mutex_lock(&o->lock);
    while (!o->ready) {
        g_cond_wait(&o->cond, &o->lock);
    }
    g_mutex_unlock(&o->lock);
}

static void origin_stop(Origin *o)
{
    g_main_loop_quit(o->loop);
    g_thread_join(o->thread);
    g_main_loop_unref(o->loop);
    g_main_context_unref(o->context);
    g_mutex_clear(&o->lock);
    g_cond_clear(&o->cond);
}

/* Sends a request through the proxy and returns everything it answers. */
static GString *through_proxy(guint16 proxy_port, const char *request)
{
    GSocketClient *client = g_socket_client_new();
    GError *error         = NULL;
    GSocketConnection *c  = g_socket_client_connect_to_host(client, "127.0.0.1", proxy_port,
            NULL, &error);
    g_assert_no_error(error);
    g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(c)), request,
            strlen(request), NULL, NULL, &error);
    g_assert_no_error(error);
    GString *reply = g_string_new(NULL);
    guint8 buf[4096];
    gssize n;
    while ((n = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(c)), buf,
                    sizeof(buf), NULL, NULL)) > 0) {
        g_string_append_len(reply, (char *)buf, n);
    }
    g_object_unref(c);
    g_object_unref(client);
    return reply;
}

static void test_proxy_records_as_received(void)
{
    char *dir     = make_temp_dir();
    char *warc    = g_build_filename(dir, "p.warc.gz", NULL);
    GError *error = NULL;

    Origin origin = {0};
    origin_start(&origin);
    guint16 origin_port = origin.port;

    for (int internal_allowed = 1; internal_allowed >= 0; internal_allowed--) {
        ArchiveProxyOptions options = {
            .ca_dir = dir, .warc_path = internal_allowed ? warc : NULL, .tmp_dir = dir,
            .software = "test", .allow_internal = internal_allowed, .check_peer_user = TRUE,
        };
        ArchiveProxy *proxy = archive_proxy_start(&options, &error);
        g_assert_no_error(error);

        char *request = g_strdup_printf("GET http://127.0.0.1:%u/chunked HTTP/1.1\r\n"
                "Host: 127.0.0.1:%u\r\nProxy-Connection: keep-alive\r\nConnection: close\r\n\r\n",
                origin_port, origin_port);
        GString *reply = through_proxy(archive_proxy_get_port(proxy), request);
        if (internal_allowed) {
            /* The browser gets what the site sent, byte for byte. */
            g_assert_cmpstr(reply->str, ==, chunked_response);
        } else {
            g_assert_true(g_str_has_prefix(reply->str, "HTTP/1.1 403 Forbidden\r\n"));
            g_assert_nonnull(strstr(reply->str, "internal network"));
        }
        g_string_free(reply, TRUE);

        /* Not a proxy request at all. */
        reply = through_proxy(archive_proxy_get_port(proxy), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        g_assert_true(g_str_has_prefix(reply->str, "HTTP/1.1 400 Bad Request\r\n"));
        g_string_free(reply, TRUE);
        g_free(request);
        g_assert_true(archive_proxy_stop(proxy, &error));
        g_assert_no_error(error);
    }
    origin_stop(&origin);

    /* And the archive holds the same bytes, with the request as the site got
     * it: in origin-form, without the proxy's own headers. */
    GString *all = read_warc(warc);
    g_assert_nonnull(strstr(all->str, chunked_response));
    char *uri = g_strdup_printf("WARC-Target-URI: http://127.0.0.1:%u/chunked\r\n", origin_port);
    g_assert_nonnull(strstr(all->str, uri));
    g_assert_nonnull(strstr(all->str, "\r\n\r\nGET /chunked HTTP/1.1\r\nHost: "));
    g_assert_null(strstr(all->str, "Proxy-Connection"));
    /* The payload digest is of the body without its chunked coding. */
    guint8 digest[20];
    gsize len = sizeof(digest);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sum, (const guchar *)"chunked body!", -1);
    g_checksum_get_digest(sum, digest, &len);
    g_checksum_free(sum);
    char *label = warc_label_sha1(digest);
    char *field = g_strdup_printf("WARC-Payload-Digest: %s\r\n", label);
    g_assert_nonnull(strstr(all->str, field));

    g_free(field);
    g_free(label);
    g_free(uri);
    g_string_free(all, TRUE);
    remove_tree(dir);
    g_free(warc);
    g_free(dir);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/archive/http1/length", test_http1_length);
    g_test_add_func("/archive/http1/chunked", test_http1_chunked);
    g_test_add_func("/archive/http1/until-close", test_http1_until_close);
    g_test_add_func("/archive/http1/no-body", test_http1_no_body);
    g_test_add_func("/archive/http1/request", test_http1_request);
    g_test_add_func("/archive/http1/errors", test_http1_errors);
    g_test_add_func("/archive/spool", test_spool);
    g_test_add_func("/archive/warc/formats", test_warc_formats);
    g_test_add_func("/archive/warc/writer", test_warc_writer);
    g_test_add_func("/archive/ca", test_ca);
    g_test_add_func("/archive/netcheck/internal", test_internal_addresses);
    g_test_add_func("/archive/proxy/records-as-received", test_proxy_records_as_received);

    return g_test_run();
}
