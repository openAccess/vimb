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

#ifndef _ARCHIVE_NETCHECK_H
#define _ARCHIVE_NETCHECK_H

#include <gio/gio.h>

/* Whether the other end of a TCP connection to this machine's loopback
 * address belongs to the user this process runs as -- so that the archiving
 * proxy serves only its own browser, not other users of the machine.
 *
 * WebKit can't be made to send the proxy a credential for plain-HTTP
 * requests (libsoup retries a 407 with the wrong URL), so the kernel is asked
 * instead, through /proc/net/tcp. Elsewhere than Linux this says yes. */
gboolean archive_peer_is_same_user(GSocketConnection *connection);

/* The same, for a connection known by its addresses: ours, and the peer's. */
gboolean archive_peer_addresses_same_user(GSocketAddress *local, GSocketAddress *remote);

/* Whether the check above can tell at all, rather than always saying yes. */
gboolean archive_peer_check_works(void);

/* Whether a page must not reach address through the archiving proxy unless
 * that is allowed: loopback, private and link-local networks (which hold
 * cloud metadata services), carrier-grade NAT, unique-local IPv6, and the
 * unspecified, multicast and reserved ranges. */
gboolean archive_address_is_internal(GInetAddress *address);

#endif /* end of include guard: _ARCHIVE_NETCHECK_H */
