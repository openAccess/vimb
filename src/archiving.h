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

#ifndef _ARCHIVING_H
#define _ARCHIVING_H

#include "main.h"

/* Archiving: making a web archive (a WACZ) of what is browsed in a tab.
 *
 * :archive [name] opens the current page in a new tab whose network session
 * goes through an archiving proxy (src/archive/proxy.h), and everything that
 * tab -- and any tab or popup opened from it -- loads is recorded. The
 * session starts from nothing: no cookies, no cache, nobody signed in, so
 * the archive holds what any visitor would be sent. :archive stop packages
 * the capture as <archive-dir>/<name>.wacz; the tabs stay, unrecorded.
 *
 * A capture is kept as loose files in <archive-dir>/<name>.capture until it
 * is packaged, so that a crash loses nothing: vimb packages what it finds
 * there the next time it starts. */

/* :archive [start [name] | pause | resume | stop | name NAME | status | NAME] */
VbCmdResult archiving_command(Client *c, const char *args);

/* The network session an archive's tabs use. */
WebKitNetworkSession *archiving_session(struct Archive *archive);

/* Called by the tab code for tabs whose c->archive is set. */
void archiving_tab_added(Client *c);
void archiving_tab_closed(Client *c);
void archiving_load_changed(Client *c, WebKitLoadEvent event);
void archiving_title_changed(Client *c);
void archiving_uri_changed(Client *c);

/* Whether the sites an archiving tab shows had their certificates checked,
 * by the proxy -- WebKit sees only the proxy's own. */
gboolean archiving_tls_checked(Client *c);

/* What the status bar shows for an archiving tab ("[REC name]"), or NULL. */
const char *archiving_status(Client *c);

/* For scripts (see luascript.h): an archive started without a tab, kept --
 * held -- by its script so that it doesn't stop when the last of its tabs
 * closes, and stopped with a callback once it is saved. verify and
 * allow_internal are -1 to follow strict-ssl and archive-allow-internal. */
struct Archive *archiving_create(Client *c, const char *name, int verify, int allow_internal,
        GError **error);
gboolean archiving_alive(struct Archive *archive);
void archiving_hold(struct Archive *archive);
void archiving_release(struct Archive *archive);
typedef void (*ArchivingSaved)(const char *path, const char *error, gpointer data);
void archiving_stop_async(struct Archive *archive, ArchivingSaved callback, gpointer data);
gboolean archiving_set_paused(struct Archive *archive, gboolean pause);
const char *archiving_name(struct Archive *archive);
void archiving_rename(struct Archive *archive, const char *name);
const char *archiving_state(struct Archive *archive);   /* "recording", ... */
guint archiving_recorded(struct Archive *archive);
const char *archiving_saved(struct Archive *archive);   /* the WACZ, once saved */

/* Packages any capture a crash left behind, in the background. */
void archiving_recover(Client *c);

/* The WACZs saved in this run, oldest first: char *s, vimb's. */
GPtrArray *archiving_saved_all(void);

/* Finishes every archive: what is still recording is packaged, at once. */
void archiving_shutdown(void);

#endif /* end of include guard: _ARCHIVING_H */
