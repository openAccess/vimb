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

#include <json-glib/json-glib.h>

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

/* Sandboxed scripts: Lua written by the assistant, run apart from the
 * user's own scripts -- in a Lua state of their own, with a memory limit,
 * only the libraries that can't reach outside (string, table, math, utf8,
 * and os's clock), and an instruction budget between waits. They have the
 * vimb table, less vimb.quit, t:eval, on_page and archive options; every URL
 * they would open is put to url_check, and what they read from pages is
 * passed to learn. */
typedef struct LuaRun LuaRun;
typedef gboolean (*LuaUrlCheck)(const char *url, char **why, gpointer data);
typedef void (*LuaLearn)(JsonNode *value, gpointer data);
typedef struct {
    LuaUrlCheck url_check;
    LuaLearn    learn;
    gpointer    data;
} LuaPolicy;

/* Called once, from the main loop, when the script ends: with its value (what
 * it returned, as JSON: null, a value, or an array of them) or why it failed;
 * and what it logged. */
typedef void (*LuaRunDone)(gboolean ok, const char *error, JsonNode *value, const char *log,
        gpointer data);

/* The run lasts until done is called -- which may be before this returns,
 * for a script that doesn't compile or never waits; the run is gone then. */
LuaRun *luascript_run_sandboxed(const char *code, const LuaPolicy *policy, LuaRunDone done,
        gpointer data);

/* Stops a sandboxed script: it ends, failed, once what it is waiting for is
 * over (done is still called). */
void luascript_cancel(LuaRun *run);

#endif /* end of include guard: _LUASCRIPT_H */
