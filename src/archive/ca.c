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
#include <glib/gstdio.h>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <string.h>
#include <time.h>

#include "ca.h"

#define CA_FILE_NAME "vimb-archive-ca.pem"
#define CA_DAYS      3650
#define SITE_DAYS    30
#define DAY          (24 * 60 * 60)

struct ArchiveCa {
    char                *file;
    char                *pem;
    gnutls_x509_crt_t   cert;
    gnutls_x509_privkey_t key;
    gnutls_x509_privkey_t site_key;     /* shared by every site certificate */
    char                *site_key_pem;
    GMutex              lock;
    GHashTable          *sites;         /* lowercased host -> GTlsCertificate */
};

static gboolean check(int ret, const char *what, GError **error)
{
    if (ret < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what,
                gnutls_strerror(ret));
        return FALSE;
    }
    return TRUE;
}

static gnutls_x509_privkey_t new_key(GError **error)
{
    gnutls_x509_privkey_t key;
    if (!check(gnutls_x509_privkey_init(&key), "Could not make a key", error)) {
        return NULL;
    }
    if (!check(gnutls_x509_privkey_generate(key, GNUTLS_PK_ECDSA,
                    GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1), 0),
                "Could not make a key", error)) {
        gnutls_x509_privkey_deinit(key);
        return NULL;
    }
    return key;
}

/* A random positive serial number, as RFC 5280 asks: up to 20 bytes. */
static gboolean set_serial(gnutls_x509_crt_t crt, GError **error)
{
    guint8 serial[16];
    if (!check(gnutls_rnd(GNUTLS_RND_NONCE, serial, sizeof(serial)),
                "Could not make a serial number", error)) {
        return FALSE;
    }
    serial[0] &= 0x7f;
    serial[0] |= 0x01;
    return check(gnutls_x509_crt_set_serial(crt, serial, sizeof(serial)),
            "Could not set a serial number", error);
}

static char *export_cert(gnutls_x509_crt_t crt, GError **error)
{
    gnutls_datum_t out;
    if (!check(gnutls_x509_crt_export2(crt, GNUTLS_X509_FMT_PEM, &out),
                "Could not export a certificate", error)) {
        return NULL;
    }
    char *pem = g_strndup((char *)out.data, out.size);
    gnutls_free(out.data);
    return pem;
}

/* PKCS#8, which GTlsCertificate reads whatever the key's kind. */
static char *export_key(gnutls_x509_privkey_t key, GError **error)
{
    gnutls_datum_t out;
    if (!check(gnutls_x509_privkey_export2_pkcs8(key, GNUTLS_X509_FMT_PEM, NULL,
                    GNUTLS_PKCS_PLAIN, &out),
                "Could not export a key", error)) {
        return NULL;
    }
    char *pem = g_strndup((char *)out.data, out.size);
    gnutls_free(out.data);
    return pem;
}

static gboolean make_authority(ArchiveCa *ca, GError **error)
{
    time_t now = time(NULL);
    guint8 key_id[64];
    size_t key_id_size = sizeof(key_id);

    if (!(ca->key = new_key(error))
            || !check(gnutls_x509_crt_init(&ca->cert), "Could not make a certificate", error)) {
        return FALSE;
    }
    gnutls_x509_crt_t crt = ca->cert;
    char *name = g_strdup_printf("vimb archiving proxy (%s@%s)", g_get_user_name(),
            g_get_host_name());
    gboolean ok = check(gnutls_x509_crt_set_version(crt, 3), "version", error)
        && set_serial(crt, error)
        && check(gnutls_x509_crt_set_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0,
                    name, strlen(name)), "name", error)
        && check(gnutls_x509_crt_set_activation_time(crt, now - DAY), "activation", error)
        && check(gnutls_x509_crt_set_expiration_time(crt, now + (time_t)CA_DAYS * DAY),
                "expiration", error)
        && check(gnutls_x509_crt_set_key(crt, ca->key), "key", error)
        && check(gnutls_x509_crt_set_basic_constraints(crt, 1, 0), "constraints", error)
        && check(gnutls_x509_crt_set_key_usage(crt,
                    GNUTLS_KEY_KEY_CERT_SIGN | GNUTLS_KEY_CRL_SIGN), "usage", error)
        && check(gnutls_x509_crt_get_key_id(crt, GNUTLS_KEYID_USE_SHA1, key_id,
                    &key_id_size), "key id", error)
        && check(gnutls_x509_crt_set_subject_key_id(crt, key_id, key_id_size),
                "subject key id", error)
        && check(gnutls_x509_crt_sign2(crt, crt, ca->key, GNUTLS_DIG_SHA256, 0),
                "Could not sign the authority", error);
    g_free(name);
    return ok;
}

static gboolean save_authority(ArchiveCa *ca, GError **error)
{
    char *key_pem = export_key(ca->key, error);
    if (!key_pem) {
        return FALSE;
    }
    char *content = g_strconcat(ca->pem, key_pem, NULL);
    gboolean ok   = g_file_set_contents_full(ca->file, content, -1,
            G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    g_free(content);
    g_free(key_pem);
    return ok;
}

static gboolean load_authority(ArchiveCa *ca, const char *content, gsize len,
        GError **error)
{
    gnutls_datum_t data = {(unsigned char *)content, (unsigned int)len};

    return check(gnutls_x509_crt_init(&ca->cert), "Could not load the authority", error)
        && check(gnutls_x509_crt_import(ca->cert, &data, GNUTLS_X509_FMT_PEM),
                "Could not read the authority's certificate", error)
        && check(gnutls_x509_privkey_init(&ca->key), "Could not load the authority", error)
        && check(gnutls_x509_privkey_import(ca->key, &data, GNUTLS_X509_FMT_PEM),
                "Could not read the authority's key", error);
}

ArchiveCa *archive_ca_open(const char *dir, GError **error)
{
    ArchiveCa *ca = g_new0(ArchiveCa, 1);
    char *content = NULL;
    gsize len;

    g_mutex_init(&ca->lock);
    ca->sites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    ca->file  = g_build_filename(dir, CA_FILE_NAME, NULL);

    if (g_mkdir_with_parents(dir, 0700) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not make %s: %s", dir, g_strerror(errno));
        goto fail;
    }
    if (g_file_get_contents(ca->file, &content, &len, NULL)) {
        if (!load_authority(ca, content, len, error)) {
            goto fail;
        }
        if (!(ca->pem = export_cert(ca->cert, error))) {
            goto fail;
        }
    } else {
        if (!make_authority(ca, error) || !(ca->pem = export_cert(ca->cert, error))
                || !save_authority(ca, error)) {
            goto fail;
        }
    }
    if (!(ca->site_key = new_key(error)) || !(ca->site_key_pem = export_key(ca->site_key, error))) {
        goto fail;
    }
    g_free(content);
    return ca;

fail:
    g_free(content);
    archive_ca_free(ca);
    return NULL;
}

void archive_ca_free(ArchiveCa *ca)
{
    if (!ca) {
        return;
    }
    if (ca->cert) {
        gnutls_x509_crt_deinit(ca->cert);
    }
    if (ca->key) {
        gnutls_x509_privkey_deinit(ca->key);
    }
    if (ca->site_key) {
        gnutls_x509_privkey_deinit(ca->site_key);
    }
    g_hash_table_unref(ca->sites);
    g_mutex_clear(&ca->lock);
    g_free(ca->site_key_pem);
    g_free(ca->pem);
    g_free(ca->file);
    g_free(ca);
}

static char *make_site_pem(ArchiveCa *ca, const char *host, GError **error)
{
    gnutls_x509_crt_t crt;
    time_t now = time(NULL);
    guint8 key_id[64];
    size_t key_id_size = sizeof(key_id);
    char *pem = NULL;

    if (!check(gnutls_x509_crt_init(&crt), "Could not make a certificate", error)) {
        return NULL;
    }

    gboolean ok = check(gnutls_x509_crt_set_version(crt, 3), "version", error)
        && set_serial(crt, error)
        && check(gnutls_x509_crt_set_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0,
                    host, strlen(host)), "name", error)
        && check(gnutls_x509_crt_set_activation_time(crt, now - DAY), "activation", error)
        && check(gnutls_x509_crt_set_expiration_time(crt, now + (time_t)SITE_DAYS * DAY),
                "expiration", error)
        && check(gnutls_x509_crt_set_key(crt, ca->site_key), "key", error)
        && check(gnutls_x509_crt_set_basic_constraints(crt, 0, -1), "constraints", error)
        && check(gnutls_x509_crt_set_key_usage(crt, GNUTLS_KEY_DIGITAL_SIGNATURE),
                "usage", error)
        && check(gnutls_x509_crt_set_key_purpose_oid(crt, GNUTLS_KP_TLS_WWW_SERVER, 0),
                "purpose", error);

    if (ok) {
        GInetAddress *ip = g_inet_address_new_from_string(host);
        if (ip) {
            ok = check(gnutls_x509_crt_set_subject_alt_name(crt, GNUTLS_SAN_IPADDRESS,
                        g_inet_address_to_bytes(ip), g_inet_address_get_native_size(ip),
                        GNUTLS_FSAN_SET), "address", error);
            g_object_unref(ip);
        } else {
            ok = check(gnutls_x509_crt_set_subject_alt_name(crt, GNUTLS_SAN_DNSNAME,
                        host, strlen(host), GNUTLS_FSAN_SET), "name", error);
        }
    }
    ok = ok
        && check(gnutls_x509_crt_get_subject_key_id(ca->cert, key_id, &key_id_size, NULL),
                "authority key id", error)
        && check(gnutls_x509_crt_set_authority_key_id(crt, key_id, key_id_size),
                "authority key id", error)
        && check(gnutls_x509_crt_sign2(crt, ca->cert, ca->key, GNUTLS_DIG_SHA256, 0),
                "Could not sign a site certificate", error);
    if (ok) {
        pem = export_cert(crt, error);
    }
    gnutls_x509_crt_deinit(crt);
    return pem;
}

GTlsCertificate *archive_ca_cert_for_host(ArchiveCa *ca, const char *host,
        GError **error)
{
    char *name = g_ascii_strdown(host, -1);
    GTlsCertificate *cert;

    g_mutex_lock(&ca->lock);
    cert = g_hash_table_lookup(ca->sites, name);
    if (cert) {
        g_object_ref(cert);
        g_free(name);
    } else {
        char *pem = make_site_pem(ca, name, error);
        if (pem) {
            char *both = g_strconcat(pem, ca->site_key_pem, NULL);
            cert = g_tls_certificate_new_from_pem(both, -1, error);
            g_free(both);
            g_free(pem);
        }
        if (cert) {
            g_hash_table_insert(ca->sites, name, g_object_ref(cert));
        } else {
            g_free(name);
        }
    }
    g_mutex_unlock(&ca->lock);
    return cert;
}

const char *archive_ca_pem(ArchiveCa *ca)
{
    return ca->pem;
}

const char *archive_ca_file(ArchiveCa *ca)
{
    return ca->file;
}
