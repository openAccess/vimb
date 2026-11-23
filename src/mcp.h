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

#ifndef _MCP_H
#define _MCP_H

#include "tools.h"

/* vimb as a Model Context Protocol server, offering the tools of tools.h.
 *
 * Over stdio (vimb --mcp): JSON-RPC messages a line each, read from in_fd and
 * written to out_fd -- which must not be stdout itself, so that nothing else
 * vimb prints can get into the stream. When in_fd closes, the archives the
 * tools started are saved and vimb quits.
 *
 * The protocol versions served, by either transport, are those of the
 * initialize handshake, 2024-11-05 to 2025-11-25. */

gboolean mcp_serve_stdio(int in_fd, int out_fd, const ToolPolicy *policy, GError **error);

/* Over HTTP (vimb --mcp-http PORT): MCP's streamable HTTP transport at
 * http://127.0.0.1:PORT/mcp, served to this user's programs only -- the
 * kernel is asked whose the connection is -- and not to web pages (by Origin
 * and Host). Each client has a session, begun by initialize; a tool's call
 * answers as server-sent events, kept alive while it runs. Linux only. */
gboolean mcp_serve_http(guint port, const ToolPolicy *policy, GError **error);

#endif /* end of include guard: _MCP_H */
