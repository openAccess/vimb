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

#include "json.h"

void archive_json_append_string(GString *out, const char *text)
{
    g_string_append_c(out, '"');
    for (const guchar *c = (const guchar *)text; *c; c++) {
        switch (*c) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\b': g_string_append(out, "\\b"); break;
            case '\f': g_string_append(out, "\\f"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if (*c < 0x20) {
                    g_string_append_printf(out, "\\u%04x", *c);
                } else {
                    g_string_append_c(out, (char)*c);
                }
        }
    }
    g_string_append_c(out, '"');
}

static void append_name(GString *out, const char *name)
{
    if (out->len && out->str[out->len - 1] != '{') {
        g_string_append(out, ", ");
    }
    archive_json_append_string(out, name);
    g_string_append(out, ": ");
}

void archive_json_append_member(GString *out, const char *name, const char *value)
{
    append_name(out, name);
    archive_json_append_string(out, value);
}

void archive_json_append_number(GString *out, const char *name, guint64 value)
{
    append_name(out, name);
    g_string_append_printf(out, "%" G_GUINT64_FORMAT, value);
}
