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

#ifndef _OPS_H
#define _OPS_H

#include <json-glib/json-glib.h>

#include "main.h"

/* Operations on tabs, for whatever drives the browser rather than a person
 * at the keyboard: scripts (luascript.h), the tools MCP clients and the
 * assistant use (tools.h).
 * Each takes a callback, called once, from the main loop, when the operation
 * is over -- with its result as JSON, or why it failed.
 *
 * What runs in a page runs in a script world of vimb's own, which the page's
 * scripts can neither see nor change. */

typedef void (*OpsCallback)(gboolean ok, const char *error, JsonNode *value, gpointer data);

/* Tabs are named by id, so that whoever holds one can tell it has closed. */
guint64 ops_tab_id(Client *c);
Client *ops_find_tab(guint64 id);

/* Loads url in c, and calls back when the load is over -- value
 * {"url", "status" (HTTP), "title"} -- or failed, or timeout seconds have
 * passed (0: no limit). A load superseded by another fails. */
void ops_load(Client *c, const char *url, double timeout, OpsCallback callback, gpointer data);

/* Runs script and gives back its value as JSON (undefined as null). */
void ops_eval(Client *c, const char *script, OpsCallback callback, gpointer data);

/* Reads the page to its end, a screen at a time -- what loads only as it is
 * read loads -- then goes back to the top. value {"screens", "reached_end"}. */
void ops_scroll(Client *c, OpsCallback callback, gpointer data);

/* The page's text (innerText), its HTML as it is now (the live DOM, after
 * scripts), and the HTML as the server sent it. */
void ops_text(Client *c, OpsCallback callback, gpointer data);
void ops_html(Client *c, OpsCallback callback, gpointer data);
void ops_source(Client *c, OpsCallback callback, gpointer data);

/* The http(s) links on the page, absolute, without fragments, once each, in
 * document order. */
void ops_links(Client *c, OpsCallback callback, gpointer data);

/* What the tab shows, as a PNG: value {"mime": "image/png", "data": base64}. */
void ops_screenshot(Client *c, OpsCallback callback, gpointer data);

/* The HTTP status of the page's main resource; 0 if none. */
guint ops_status(Client *c);

/* Called when a tab closes: what waits on it fails. */
void ops_tab_closed(Client *c);

#endif /* end of include guard: _OPS_H */
