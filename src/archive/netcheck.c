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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "netcheck.h"

#ifdef __linux__
/* The owner of the socket in a /proc/net/tcp-style table whose local port is
 * the peer's and whose remote port is ours: the peer's own end of the
 * connection. -1 when there is no such socket. */
static long owner_in_table(const char *path, guint16 peer_port, guint16 our_port)
{
    char *content = NULL;
    long uid      = -1;

    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        return -1;
    }
    char **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    /* sl local_address rem_address st tx:rx tr:when retrnsmt uid ...; the
     * addresses are hex, IPv4 or IPv6, each followed by :port in hex. */
    for (char **l = lines + 1; *l && uid < 0; l++) {
        char **fields = g_strsplit_set(g_strstrip(*l), " ", -1);
        char *f[8];
        int n = 0;
        for (char **p = fields; *p && n < 8; p++) {
            if (**p) {
                f[n++] = *p;
            }
        }
        if (n == 8) {
            const char *local_port  = strrchr(f[1], ':');
            const char *remote_port = strrchr(f[2], ':');
            guint64 lp, rp, owner;
            if (local_port && remote_port
                    && g_ascii_string_to_unsigned(local_port + 1, 16, 0, 0xffff, &lp, NULL)
                    && g_ascii_string_to_unsigned(remote_port + 1, 16, 0, 0xffff, &rp, NULL)
                    && lp == peer_port && rp == our_port
                    && g_ascii_string_to_unsigned(f[7], 10, 0, G_MAXLONG, &owner, NULL)) {
                uid = (long)owner;
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    return uid;
}
#endif

gboolean archive_peer_addresses_same_user(GSocketAddress *local, GSocketAddress *remote)
{
#ifdef __linux__
    if (!G_IS_INET_SOCKET_ADDRESS(local) || !G_IS_INET_SOCKET_ADDRESS(remote)) {
        return FALSE;
    }
    guint16 our_port  = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
    guint16 peer_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(remote));
    long uid = owner_in_table("/proc/net/tcp", peer_port, our_port);
    if (uid < 0) {
        uid = owner_in_table("/proc/net/tcp6", peer_port, our_port);
    }
    return uid >= 0 && (uid_t)uid == getuid();
#else
    return TRUE;
#endif
}

gboolean archive_peer_check_works(void)
{
#ifdef __linux__
    return TRUE;
#else
    return FALSE;
#endif
}

gboolean archive_peer_is_same_user(GSocketConnection *connection)
{
    GSocketAddress *local  = g_socket_connection_get_local_address(connection, NULL);
    GSocketAddress *remote = g_socket_connection_get_remote_address(connection, NULL);
    gboolean same          = archive_peer_addresses_same_user(local, remote);
    g_clear_object(&local);
    g_clear_object(&remote);
    return same;
}

static gboolean ipv4_is_internal(const guint8 *b)
{
    return b[0] == 0                                    /* 0.0.0.0/8 */
        || b[0] == 10                                   /* 10.0.0.0/8 */
        || b[0] == 127                                  /* loopback */
        || (b[0] == 100 && (b[1] & 0xc0) == 64)         /* 100.64.0.0/10 */
        || (b[0] == 169 && b[1] == 254)                 /* link-local, metadata */
        || (b[0] == 172 && (b[1] & 0xf0) == 16)         /* 172.16.0.0/12 */
        || (b[0] == 192 && b[1] == 0 && b[2] == 0)      /* 192.0.0.0/24 */
        || (b[0] == 192 && b[1] == 168)                 /* 192.168.0.0/16 */
        || (b[0] == 198 && (b[1] & 0xfe) == 18)         /* 198.18.0.0/15 */
        || b[0] >= 224;                                 /* multicast, reserved */
}

gboolean archive_address_is_internal(GInetAddress *address)
{
    const guint8 *b = g_inet_address_to_bytes(address);

    if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4) {
        return ipv4_is_internal(b);
    }
    if (g_inet_address_get_is_any(address) || g_inet_address_get_is_loopback(address)
            || g_inet_address_get_is_link_local(address)
            || g_inet_address_get_is_site_local(address)
            || g_inet_address_get_is_multicast(address)
            || (b[0] & 0xfe) == 0xfc) {                 /* fc00::/7 */
        return TRUE;
    }
    /* An IPv4 address in IPv6 clothes: ::ffff:a.b.c.d, or NAT64's
     * 64:ff9b::a.b.c.d. */
    static const guint8 mapped[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
    static const guint8 nat64[12]  = {0,0x64,0xff,0x9b, 0,0,0,0, 0,0,0,0};
    if (!memcmp(b, mapped, 12) || !memcmp(b, nat64, 12)) {
        return ipv4_is_internal(b + 12);
    }
    return FALSE;
}
