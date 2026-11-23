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

#ifndef _CRAWL_H
#define _CRAWL_H

#include "ops.h"

/* Crawling: visiting pages from seeds outward, following links within a
 * scope, in one tab -- an archiving tab, to archive them.
 *
 * The scope is enforced here, not by whatever asks for the crawl: the
 * links come from pages, and text on a page must not be able to send the
 * crawl anywhere the scope doesn't reach. */

typedef enum {
    CRAWL_SCOPE_PAGE_LINKS,     /* the seeds, and the links on them */
    CRAWL_SCOPE_SITE,           /* the seeds' sites (registrable domains) */
    CRAWL_SCOPE_HOST,           /* the seeds' hosts */
    CRAWL_SCOPE_PATTERN,        /* URLs a regular expression matches */
    CRAWL_SCOPE_ANY,
} CrawlScope;

typedef struct {
    char       **seeds;
    guint      depth;           /* links followed from a seed: 0 is the seeds alone */
    CrawlScope scope;
    char       *pattern;        /* for CRAWL_SCOPE_PATTERN */
    guint      max_pages;       /* 0: no limit */
    double     max_seconds;     /* 0: no limit */
    double     delay;           /* seconds between pages from one host */
    double     timeout;         /* seconds a page has to load; 0: no limit */
    gboolean   scroll;          /* read each page to its end */
} CrawlOptions;

/* Told of each page as it is done. ok is whether it loaded. */
typedef void (*CrawlPageFunc)(const char *url, guint depth, gboolean ok, guint status,
        const char *title, const char *error, gpointer data);

/* Crawls in tab c; done is called with {"pages", "loaded", "failed",
 * "left", "seconds", "stopped" ("done", "max_pages", "max_seconds")}. An
 * invalid pattern fails at once. */
void crawl_run(Client *c, const CrawlOptions *options, CrawlPageFunc page, OpsCallback done,
        gpointer data);

#endif /* end of include guard: _CRAWL_H */
