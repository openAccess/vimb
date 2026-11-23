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

/* vimb-archive-pack: packages a directory of gzipped WARCs -- a capture, or
 * any tool's output -- as a WACZ. Not installed.
 *
 *   vimb-archive-pack [--title TITLE] [--remove] DIR OUT.wacz
 *
 * A pages.jsonl in DIR is the page list; without one, the pages are the
 * top-level page loads the WARCs record. */

#include <stdio.h>

#include "wacz.h"

int main(int argc, char **argv)
{
    char *title       = NULL;
    gboolean remove   = FALSE;
    GError *error     = NULL;
    GOptionEntry entries[] = {
        {"title", 't', 0, G_OPTION_ARG_STRING, &title, "What to call the archive", "TITLE"},
        {"remove", 0, 0, G_OPTION_ARG_NONE, &remove,
            "Delete the WARCs and page list once packaged", NULL},
        {NULL}
    };
    GOptionContext *context = g_option_context_new("DIR OUT.wacz - package WARCs as a WACZ");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error) || argc != 3) {
        fprintf(stderr, "%s\n", error ? error->message : "Give a directory and a WACZ to write");
        return 2;
    }
    g_option_context_free(context);
    if (!archive_wacz_package(argv[1], argv[2], "vimb-archive-pack", title, remove, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    printf("%s\n", argv[2]);
    return 0;
}
