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

#ifndef _ARCHIVE_CA_H
#define _ARCHIVE_CA_H

#include <gio/gio.h>

/* The certificate authority the archiving proxy signs its sites'
 * certificates with, to read what the browser sends over TLS.
 *
 * Nothing trusts it: WebKit is told to accept each certificate it signs for
 * its one host (webkit_network_session_allow_tls_certificate_for_host), and
 * only in the sessions that archive. It is kept, with its key, in a file only
 * its user may read, so that other tools (curl --cacert) can be pointed at
 * it; the site certificates live only in memory, all sharing one key. */
typedef struct ArchiveCa ArchiveCa;

/* Loads the authority kept in dir, making one there if there is none. */
ArchiveCa *archive_ca_open(const char *dir, GError **error);
void archive_ca_free(ArchiveCa *ca);

/* A certificate for host, with its key, signed by the authority. Host names
 * are matched without regard to case; an IP address gets an IP certificate.
 * Safe to call from any thread. */
GTlsCertificate *archive_ca_cert_for_host(ArchiveCa *ca, const char *host,
        GError **error);

/* The authority's own certificate, in PEM, and the file holding it. */
const char *archive_ca_pem(ArchiveCa *ca);
const char *archive_ca_file(ArchiveCa *ca);

#endif /* end of include guard: _ARCHIVE_CA_H */
