/*-
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>

#define _WITH_GETLINE

#include <archive.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sysexits.h>

#include <pkg.h>
#include "pkgcli.h"

#define EQ 1
#define LT 2
#define LTE 3
#define GT 4
#define GTE 5
struct version_entry {
	char *version;
	int type;
};

struct audit_entry {
	char *pkgname;
	struct version_entry v1;
	struct version_entry v2;
	char *url;
	char *desc;
	SLIST_ENTRY(audit_entry) next;
};

SLIST_HEAD(audit_head, audit_entry);

/*
 * The _sorted stuff.
 *
 * We are using the optimized search based on the following observations:
 *
 * - number of VuXML entries is more likely to be far greater than
 *   the number of installed ports; thus we should try to optimize
 *   the walk through all entries for a given port;
 *
 * - fnmatch() is good and fast, but if we will compare the audit entry
 *   name prefix without globbing characters to the prefix of port name
 *   of the same length and they are different, there is no point to
 *   check the rest;
 *
 * - (most important bit): if parsed VuXML entries are lexicographically
 *   sorted per the largest prefix with no globbing characters and we
 *   know how many succeeding entries have the same prefix we can
 *
 *   a. skip the rest of the entries once the non-globbing prefix is
 *      lexicographically larger than the port name prefix of the
 *      same length: all successive prefixes will be larger as well;
 *
 *   b. if we have non-globbing prefix that is lexicographically smaller
 *      than port name prefix, we can skip all succeeding entries with
 *      the same prefix; and as some port names tend to repeat due to
 *      multiple vulnerabilities, it could be a large win.
 */
struct audit_entry_sorted {
	struct audit_entry *e;	/* Entry itself */
	size_t noglob_len;	/* Prefix without glob characters */
	size_t next_pfx_incr;	/* Index increment for the entry with
				   different prefix */
};

/*
 * Another small optimization to skip the beginning of the
 * VuXML entry array, if possible.
 *
 * audit_entry_first_byte_idx[ch] represents the index
 * of the first VuXML entry in the sorted array that has
 * its non-globbing prefix that is started with the character
 * 'ch'.  It allows to skip entries from the beginning of the
 * VuXML array that aren't relevant for the checked port name.
 */
static size_t audit_entry_first_byte_idx[256];

void
usage_audit(void)
{
	fprintf(stderr, "usage: pkg audit [-F] <pattern>\n\n");
	fprintf(stderr, "For more information see 'pkg help audit'.\n");
}

static int
fetch_and_extract(const char *src, const char *dest)
{
	struct archive *a = NULL;
	struct archive_entry *ae = NULL;
	int fd = -1;
	char tmp[MAXPATHLEN];
	const char *tmpdir;
	int retcode = EPKG_FATAL;
	int ret;
	time_t t = 0;
	struct stat st;

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	strlcpy(tmp, tmpdir, sizeof(tmp));
	strlcat(tmp, "/auditfile.tbz", sizeof(tmp));

	if (stat(dest, &st) != -1) {
		t = st.st_mtime;
	}
	switch (pkg_fetch_file(src, tmp, t)) {
	case EPKG_OK:
		break;
	case EPKG_UPTODATE:
		printf("Audit file up-to-date.\n");
		retcode = EPKG_OK;
		goto cleanup;
	default:
		warnx("Cannot fetch audit file!");
		goto cleanup;
	}

	a = archive_read_new();
#if ARCHIVE_VERSION_NUMBER < 3000002
	archive_read_support_compression_all(a);
#else
	archive_read_support_filter_all(a);
#endif

	archive_read_support_format_tar(a);

	if (archive_read_open_filename(a, tmp, 4096) != ARCHIVE_OK) {
		warnx("archive_read_open_filename(%s): %s",
				tmp, archive_error_string(a));
		goto cleanup;
	}

	while ((ret = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
		fd = open(dest, O_RDWR|O_CREAT|O_TRUNC,
				S_IRUSR|S_IRGRP|S_IROTH);
		if (fd < 0) {
			warn("open(%s)", dest);
			goto cleanup;
		}

		if (archive_read_data_into_fd(a, fd) != ARCHIVE_OK) {
			warnx("archive_read_data_into_fd(%s): %s",
					dest, archive_error_string(a));
			goto cleanup;
		}
	}

	retcode = EPKG_OK;

	cleanup:
	unlink(tmp);
	if (a != NULL)
#if ARCHIVE_VERSION_NUMBER < 3000002
		archive_read_finish(a);
#else
		archive_read_free(a);
#endif
	if (fd > 0)
		close(fd);

	return (retcode);
}

/* Fuuuu */
static void
parse_pattern(struct audit_entry *e, char *pattern, size_t len)
{
	size_t i;
	char *start = pattern;
	char *end;
	char **dest = &e->pkgname;
	char **next_dest = NULL;
	struct version_entry *v = &e->v1;
	int skipnext;
	int type;
	for (i = 0; i < len; i++) {
		type = 0;
		skipnext = 0;
		if (pattern[i] == '=') {
			type = EQ;
		}
		if (pattern[i] == '<') {
			if (pattern[i+1] == '=') {
				skipnext = 1;
				type = LTE;
			} else {
				type = LT;
			}
		}
		if (pattern[i] == '>') {
			if (pattern[i+1] == '=') {
				skipnext = 1;
				type = GTE;
			} else {
				type = GT;
			}
		}

		if (type != 0) {
			v->type = type;
			next_dest = &v->version;
			v = &e->v2;
		}

		if (next_dest != NULL || i == len - 1) {
			end = pattern + i;
			*dest = strndup(start, end - start);

			i += skipnext;
			start = pattern + i + 1;
			dest = next_dest;
			next_dest = NULL;
		}
	}
}

static int
parse_db(const char *path, struct audit_head *h)
{
	struct audit_entry *e;
	FILE *fp;
	size_t linecap = 0;
	ssize_t linelen;
	char *line = NULL;
	char *column;
	uint8_t column_id;

	if ((fp = fopen(path, "r")) == NULL)
		return (EPKG_FATAL);

	while ((linelen = getline(&line, &linecap, fp)) > 0) {
		column_id = 0;

		if (line[0] == '#')
			continue;

		if ((e = calloc(1, sizeof(struct audit_entry))) == NULL)
			err(1, "calloc(audit_entry)");

		while ((column = strsep(&line, "|")) != NULL)
		{
			switch (column_id) {
			case 0:
				parse_pattern(e, column, linelen);
				break;
			case 1:
				e->url = strdup(column);
				break;
			case 2:
				e->desc = strdup(column);
				break;
			default:
				warn("extra column in audit file");
			}
			column_id++;
		}
		SLIST_INSERT_HEAD(h, e, next);
	}

	return EPKG_OK;
}

/*
 * Returns the length of the largest prefix without globbing
 * characters, as per fnmatch().
 */
static size_t
str_noglob_len(const char *s)
{
	size_t n;

	for (n = 0; s[n] && s[n] != '*' && s[n] != '?' &&
	    s[n] != '[' && s[n] != '{' && s[n] != '\\'; n++);

	return n;
}

/*
 * Helper for quicksort that lexicographically orders prefixes.
 */
static int
audit_entry_compare(const void *a, const void *b)
{
	const struct audit_entry_sorted *e1, *e2;
	size_t min_len;
	int result;

	e1 = (const struct audit_entry_sorted *)a;
	e2 = (const struct audit_entry_sorted *)b;

	min_len = (e1->noglob_len < e2->noglob_len ?
	    e1->noglob_len : e2->noglob_len);
	result = strncmp(e1->e->pkgname, e2->e->pkgname, min_len);
	/*
	 * Additional check to see if some word is a prefix of an
	 * another one and, thus, should go before the former.
	 */
	if (result == 0) {
		if (e1->noglob_len < e2->noglob_len)
			result = -1;
		else if (e1->noglob_len > e2->noglob_len)
			result = 1;
	}

	return (result);
}

/*
 * Sorts VuXML entries and calculates increments to jump to the
 * next distinct prefix.
 */
static struct audit_entry_sorted *
preprocess_db(struct audit_head *h)
{
	struct audit_entry *e;
	struct audit_entry_sorted *ret;
	size_t i, n, tofill;

	n = 0;
	SLIST_FOREACH(e, h, next)
		n++;

	ret = (struct audit_entry_sorted *)calloc(n + 1, sizeof(ret[0]));
	if (ret == NULL)
		err(1, "calloc(audit_entry_sorted*)");
	bzero((void *)ret, (n + 1) * sizeof(ret[0]));

	n = 0;
	SLIST_FOREACH(e, h, next) {
		ret[n].e = e;
		ret[n].noglob_len = str_noglob_len(e->pkgname);
		ret[n].next_pfx_incr = 1;
		n++;
	}

	qsort(ret, n, sizeof(*ret), audit_entry_compare);

	/*
	 * Determining jump indexes to the next different prefix.
	 * Only non-1 increments are calculated there.
	 *
	 * Due to the current usage that picks only increment for the
	 * first of the non-unique prefixes in a row, we could
	 * calculate only that one and skip calculations for the
	 * succeeding, but for the uniformity and clarity we're
	 * calculating 'em all.
	 */
	for (n = 1, tofill = 0; ret[n].e; n++) {
		if (ret[n - 1].noglob_len != ret[n].noglob_len) {
			struct audit_entry_sorted *base;

			base = ret + n - tofill;
			for (i = 0; tofill > 1; i++, tofill--)
				base[i].next_pfx_incr = tofill;
			tofill = 1;
		} else if (strcmp(ret[n - 1].e->pkgname,
		    ret[n].e->pkgname) == 0) {
			tofill++;
		} else {
			tofill = 1;
		}
	}

	/* Calculate jump indexes for the first byte of the package name */
	bzero(audit_entry_first_byte_idx, sizeof(audit_entry_first_byte_idx));
	for (n = 1, i = 0; n < 256; n++) {
		while (ret[i].e != NULL &&
		    (size_t)(ret[i].e->pkgname[0]) < n)
			i++;
		audit_entry_first_byte_idx[n] = i;
	}

	return (ret);
}

static bool
match_version(const char *pkgversion, struct version_entry *v)
{
	bool res = false;

	/*
	 * Return true so it is easier for the caller to handle case where there is
	 * only one version to match: the missing one will always match.
	 */
	if (v->version == NULL)
		return true;

	switch (pkg_version_cmp(pkgversion, v->version)) {
	case -1:
		if (v->type == LT || v->type == LTE)
			res = true;
		break;
	case 0:
		if (v->type == EQ || v->type == LTE || v->type == GTE)
			res = true;
		break;
	case 1:
		if (v->type == GT || v->type == GTE)
			res = true;
		break;
	}
	return res;
}

static bool
is_vulnerable(struct audit_entry_sorted *a, struct pkg *pkg)
{
	struct audit_entry *e;
	const char *pkgname;
	const char *pkgversion;
	bool res = false, res1, res2;

	pkg_get(pkg,
		PKG_NAME, &pkgname,
		PKG_VERSION, &pkgversion
	);

	a += audit_entry_first_byte_idx[(size_t)pkgname[0]];
	for (; (e = a->e) != NULL; a += a->next_pfx_incr) {
		int cmp;
		size_t i;

		/*
		 * Audit entries are sorted, so if we had found one
		 * that is lexicographically greater than our name,
		 * it and the rest won't match our name.
		 */
		cmp = strncmp(pkgname, e->pkgname, a->noglob_len);
		if (cmp > 0)
			continue;
		else if (cmp < 0)
			break;

		for (i = 0; i < a->next_pfx_incr; i++) {
			e = a[i].e;
			if (fnmatch(e->pkgname, pkgname, 0) != 0)
				continue;

			res1 = match_version(pkgversion, &e->v1);
			res2 = match_version(pkgversion, &e->v2);
			if (res1 && res2) {
				res = true;
				if (quiet) {
					printf("%s-%s\n", pkgname, pkgversion);
				} else {
					printf("%s-%s is vulnerable:\n", pkgname, pkgversion);
					printf("%s\n", e->desc);
					printf("WWW: %s\n\n", e->url);
				}
			}
		}
	}

	return res;
}

static void
free_audit_list(struct audit_head *h)
{
	struct audit_entry *e;

	while (!SLIST_EMPTY(h)) {
		e = SLIST_FIRST(h);
		SLIST_REMOVE_HEAD(h, next);
		free(e->v1.version);
		free(e->v2.version);
		free(e->url);
		free(e->desc);
	}
}

int
exec_audit(int argc, char **argv)
{
	struct audit_head h = SLIST_HEAD_INITIALIZER();
	struct audit_entry_sorted *cooked_audit_entries = NULL;
	struct pkgdb *db = NULL;
	struct pkgdb_it *it = NULL;
	struct pkg *pkg = NULL;
	const char *db_dir;
	char *name;
	char *version;
	char audit_file[MAXPATHLEN + 1];
	unsigned int vuln = 0;
	bool fetch = false;
	int ch;
	int ret = EX_OK;
	const char *portaudit_site = NULL;

	if (pkg_config_string(PKG_CONFIG_DBDIR, &db_dir) != EPKG_OK) {
		warnx("PKG_DBIR is missing");
		return (EX_CONFIG);
	}
	snprintf(audit_file, sizeof(audit_file), "%s/auditfile", db_dir);

	while ((ch = getopt(argc, argv, "qF")) != -1) {
		switch (ch) {
		case 'q':
			quiet = true;
			break;
		case 'F':
			fetch = true;
			break;
		default:
			usage_audit();
			return(EX_USAGE);
		}
	}
	argc -= optind;
	argv += optind;

	if (fetch == true) {
		if (pkg_config_string(PKG_CONFIG_PORTAUDIT_SITE, &portaudit_site) != EPKG_OK) {
			warnx("PORTAUDIT_SITE is missing");
			return (EX_CONFIG);
		}
		if (fetch_and_extract(portaudit_site, audit_file) != EPKG_OK) {
			return (EX_IOERR);
		}
	}

	if (argc > 2) {
		usage_audit();
		return (EX_USAGE);
	}

	if (argc == 1) {
		name = argv[0];
		version = strrchr(name, '-');
		if (version == NULL)
			err(EX_USAGE, "bad package name format: %s", name);
		version[0] = '\0';
		version++;
		pkg_new(&pkg, PKG_FILE);
		pkg_set(pkg,
		    PKG_NAME, name,
		    PKG_VERSION, version);
		if (parse_db(audit_file, &h) != EPKG_OK) {
			if (errno == ENOENT)
				warnx("unable to open audit file, try running 'pkg audit -F' first");
			else
				warn("unable to open audit file %s", audit_file);
			ret = EX_DATAERR;
			goto cleanup;
		}
		cooked_audit_entries = preprocess_db(&h);
		is_vulnerable(cooked_audit_entries, pkg);
		goto cleanup;
	}

	if (pkgdb_open(&db, PKGDB_DEFAULT) != EPKG_OK) {
		/*
		 * if the database doesn't exist a normal user can't create it
		 * it just means there is no package
		 */
		if (geteuid() == 0)
			return (EX_IOERR);
		return (EX_OK);
	}

	if ((it = pkgdb_query(db, NULL, MATCH_ALL)) == NULL)
	{
		warnx("cannot query local database");
		ret = EX_IOERR;
		goto cleanup;
	}

	if (parse_db(audit_file, &h) != EPKG_OK) {
		if (errno == ENOENT)
			warnx("unable to open audit file, try running 'pkg audit -F' first");
		else
			warn("unable to open audit file %s", audit_file);
		ret = EX_DATAERR;
		goto cleanup;
	}
	cooked_audit_entries = preprocess_db(&h);

	while ((ret = pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC)) == EPKG_OK)
		if (is_vulnerable(cooked_audit_entries, pkg))
			vuln++;

	if (ret == EPKG_END && vuln == 0)
		ret = EX_OK;

	if (!quiet)
		printf("%u problem(s) in your installed packages found.\n", vuln);

cleanup:
	pkgdb_it_free(it);
	pkgdb_close(db);
	pkg_free(pkg);
	free_audit_list(&h);

	return (ret);
}
