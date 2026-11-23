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

#ifndef _ARCHIVE_JSON_H
#define _ARCHIVE_JSON_H

#include <glib.h>

/* Appends text, which must be UTF-8, as a JSON string: quoted, with quotes,
 * backslashes and control characters escaped. */
void archive_json_append_string(GString *out, const char *text);

/* Appends ", \"name\": \"value\"" -- or without the comma for the first
 * member, when out ends with "{". */
void archive_json_append_member(GString *out, const char *name, const char *value);

/* Same, with a number for the value. */
void archive_json_append_number(GString *out, const char *name, guint64 value);

#endif /* end of include guard: _ARCHIVE_JSON_H */
