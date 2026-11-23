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

#ifndef _ASSISTANT_H
#define _ASSISTANT_H

#include <json-glib/json-glib.h>

#include "main.h"

/* The assistant: a model (backend.h) that browses and archives with the
 * tools of tools.h, asked with :ask and answering in a pane beside the tabs.
 *
 * It opens only URLs that came from the user, or from pages and the tools'
 * results -- never ones it made up, which could carry what it read away --
 * and runs no JavaScript of its own. */

VbCmdResult assistant_ask(Client *c, const char *text);         /* :ask */
VbCmdResult assistant_command(Client *c, const char *args);     /* :assistant */

/* Called when the assistant's turn is over, from the main loop: error is
 * NULL if it ended as it should. */
typedef void (*AssistantDone)(const char *error, gpointer data);
void assistant_on_done(AssistantDone done, gpointer data);

/* What the assistant did, for a summary: {backend, rounds, tool_calls,
 * reply} -- the reply its last turn's words. */
JsonObject *assistant_report(void);
gboolean assistant_busy(void);

void assistant_shutdown(void);

#endif /* end of include guard: _ASSISTANT_H */
