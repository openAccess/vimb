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

#ifndef _TOOLS_H
#define _TOOLS_H

#include <json-glib/json-glib.h>

#include "main.h"

/* The tools vimb offers to what drives it from outside the keyboard -- an
 * MCP client (mcp.h), the assistant -- each described by a JSON Schema and
 * run on the operations of ops.h. Tabs and archives are named by small
 * numbers, which stay theirs while they last.
 *
 * What the tools may do is the policy's to say. Whatever the policy, a URL
 * is opened only if it is http or https; and a crawl keeps to its scope (see
 * crawl.h), whatever the pages it reads say. */

typedef gboolean (*ToolUrlCheck)(const char *url, char **why, gpointer data);
typedef void (*ToolLearn)(JsonNode *value, gpointer data);
/* The answer to whether a script may run: call it once. */
typedef void (*ToolApproved)(gboolean yes, gpointer done_data);
typedef void (*ToolApprove)(const char *code, const char *description, ToolApproved done,
        gpointer done_data, gpointer data);

typedef struct {
    gboolean     allow_eval;            /* run_javascript */
    gboolean     allow_archive_options; /* strict_ssl and allow_internal for archives */
    char         **allowed_domains;     /* the sites URLs must be on; NULL: any */
    ToolUrlCheck url_check;             /* a further say on each URL opened; NULL: none */
    ToolApprove  approve_lua;           /* run_lua, and who lets a script run; NULL: none */
    ToolLearn    learn;                 /* told what scripts read from pages; NULL: nobody */
    gboolean     replay_any;            /* replay_archive any WACZ, not only this run's */
    gpointer     url_check_data;        /* for the callbacks */
} ToolPolicy;

typedef struct {
    gboolean ok;
    char     *error;
    JsonNode *value;            /* the result */
    char     *image_mime;       /* a screenshot's image, base64 */
    char     *image_data;
} ToolResult;

typedef void (*ToolDone)(const ToolResult *result, gpointer data);

/* The tools the policy allows: a JSON array of {name, description,
 * inputSchema}. */
JsonNode *tools_list(const ToolPolicy *policy);

/* Runs a tool. done is called once, from the main loop; the policy must
 * last until it is. */
void tools_call(const char *name, JsonObject *arguments, const ToolPolicy *policy,
        ToolDone done, gpointer data);

/* The number tools name a tab by. */
guint tools_tab_number(Client *c);

/* Stops the scripts run_lua is running: each ends, failed, when what it
 * waits for is over. */
void tools_cancel_scripts(void);

/* Lets go of the archives the tools started -- those still recording stop,
 * and are saved. */
void tools_release(void);

#endif /* end of include guard: _TOOLS_H */
