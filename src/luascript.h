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

#ifndef _LUASCRIPT_H
#define _LUASCRIPT_H

#include "main.h"

/* Lua scripting: scripts that drive the browser -- open pages, wait for
 * them, read them, archive them, crawl -- with the vimb table (see the man
 * page). Each script runs as a coroutine: a call that has to wait (a:open,
 * t:load, vimb.sleep...) suspends the script, and the main loop goes on
 * until the wait is over. */

/* Runs the script in path; with exit_when_done, vimb quits when it ends,
 * with status 0, or 1 if it failed. */
void luascript_run_file(Client *c, const char *path, gboolean exit_when_done);

/* Runs a chunk of Lua given on the command line (:lua). */
void luascript_run_string(Client *c, const char *code);

#endif /* end of include guard: _LUASCRIPT_H */
