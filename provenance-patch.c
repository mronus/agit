/*
 * The line record of a commit in a patch mail.
 */

#include "git-compat-util.h"
#include "provenance-patch.h"
#include "commit.h"
#include "diffcore.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "provenance.h"
#include "repository.h"
#include "strbuf.h"
#include "tree-walk.h"
#include "xdiff-interface.h"

#define BLOCK_HEADER "Provenance:"
#define BLOCK_INDENT "    "

/* Whether the patch in `q` shows `path`. */
static int shows_path(const struct diff_queue_struct *q, const char *path)
{
	for (int i = 0; i < q->nr; i++)
		if (!strcmp(q->queue[i]->one->path, path) ||
		    !strcmp(q->queue[i]->two->path, path))
			return 1;
	return 0;
}

int provenance_patch_format(struct repository *r,
			    const struct object_id *commit,
			    const struct diff_queue_struct *q,
			    struct strbuf *out)
{
	struct provenance_cache cache;
	const struct provenance_record *rec;
	int ret = 0;

	provenance_cache_init(&cache, r);
	rec = provenance_cache_get(&cache, commit);
	if (rec) {
		/* the same record, with the blocks the patch shows */
		struct provenance_record shown = *rec;
		struct strbuf buf = STRBUF_INIT;
		const char *p;

		ALLOC_ARRAY(shown.files, rec->files_nr);
		shown.files_nr = 0;
		for (size_t i = 0; i < rec->files_nr; i++)
			if (shows_path(q, rec->files[i].path))
				shown.files[shown.files_nr++] = rec->files[i];

		if (shown.files_nr) {
			provenance_record_format(&shown, &buf);
			strbuf_addstr(out, BLOCK_HEADER "\n");
			for (p = buf.buf; *p; ) {
				const char *eol = strchrnul(p, '\n');

				strbuf_addf(out, BLOCK_INDENT "%.*s\n",
					    (int)(eol - p), p);
				p = *eol ? eol + 1 : eol;
			}
			ret = 1;
		}
		free(shown.files);
		strbuf_release(&buf);
	}
	provenance_cache_release(&cache);
	return ret;
}

static int line_starts_with(const char *line, size_t len, const char *prefix)
{
	size_t plen = strlen(prefix);

	return len >= plen && !memcmp(line, prefix, plen);
}

/* Whether the line starts a patch, as git-apply and git-mailinfo see it. */
static int starts_patch(const char *line, size_t len)
{
	return line_starts_with(line, len, "diff ") ||
	       line_starts_with(line, len, "--- ") ||
	       line_starts_with(line, len, "+++ ") ||
	       line_starts_with(line, len, "Index: ");
}

int provenance_patch_parse(struct repository *r, const char *buf, size_t len,
			   struct provenance_record *rec, struct strbuf *err)
{
	const char *end = buf + len;
	struct strbuf text = STRBUF_INIT;
	int found = 0, ret;

	while (buf < end) {
		const char *eol = memchr(buf, '\n', end - buf);
		size_t linelen = (eol ? eol : end) - buf;

		if (found) {
			if (!line_starts_with(buf, linelen, BLOCK_INDENT))
				break;
			strbuf_add(&text, buf + strlen(BLOCK_INDENT),
				   linelen - strlen(BLOCK_INDENT));
			strbuf_addch(&text, '\n');
		} else if (starts_patch(buf, linelen)) {
			break;
		} else if (linelen == strlen(BLOCK_HEADER) &&
			   line_starts_with(buf, linelen, BLOCK_HEADER)) {
			found = 1;
		}
		buf = eol ? eol + 1 : end;
	}

	if (!found)
		ret = 0;
	else if (provenance_record_parse(text.buf, text.len, r->hash_algo,
					 rec, err))
		ret = -1;
	else
		ret = 1;
	strbuf_release(&text);
	return ret;
}

/* A run of consecutive lines that a commit changes, 0-based. */
struct run {
	size_t start, nr;
};

/* One side of the changes of a commit to a path. */
struct version {
	mmfile_t content;
	/* line i starts at start[i]; start[nr] is the end of the content */
	size_t *start;
	size_t nr;
	/* the runs of lines that the commit adds (or deletes) */
	struct run *runs;
	size_t runs_nr, runs_alloc;
	/* per line: a range has been located on it */
	char *taken;
};

static void version_release(struct version *v)
{
	free(v->content.ptr);
	free(v->start);
	free(v->runs);
	free(v->taken);
}

/* Find the lines of the content of `v`. */
static void split_lines(struct version *v)
{
	const char *p = v->content.ptr, *end = p + v->content.size;
	size_t alloc = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', end - p);

		ALLOC_GROW(v->start, v->nr + 1, alloc);
		v->start[v->nr++] = p - v->content.ptr;
		p = eol ? eol + 1 : end;
	}
	ALLOC_GROW(v->start, v->nr + 1, alloc);
	v->start[v->nr] = v->content.size;
	CALLOC_ARRAY(v->taken, v->nr);
}

/* Read `path` of `tree`: empty unless it is a regular file there. */
static int read_version(struct repository *r, const struct object_id *tree,
			const char *path, struct version *v)
{
	struct object_id oid;
	unsigned short mode;

	if (oideq(tree, r->hash_algo->empty_tree) ||
	    get_tree_entry(r, tree, path, &oid, &mode) || !S_ISREG(mode)) {
		v->content.ptr = xstrdup("");
		v->content.size = 0;
	} else {
		enum object_type type;
		size_t size;
		void *data = odb_read_object(r->objects, &oid, &type, &size);

		if (!data || type != OBJ_BLOB) {
			free(data);
			return error(_("unable to read blob '%s'"),
				     oid_to_hex(&oid));
		}
		v->content.ptr = data;
		v->content.size = size;
	}
	split_lines(v);
	return 0;
}

static void add_run(struct version *v, long start, long nr)
{
	if (!nr)
		return;
	ALLOC_GROW(v->runs, v->runs_nr + 1, v->runs_alloc);
	v->runs[v->runs_nr].start = start;
	v->runs[v->runs_nr].nr = nr;
	v->runs_nr++;
}

struct versions {
	struct version *pre, *post;
};

static int collect_runs(long start_a, long count_a,
			long start_b, long count_b, void *data)
{
	struct versions *vs = data;

	add_run(vs->pre, start_a, count_a);
	add_run(vs->post, start_b, count_b);
	return 0;
}

/* Find the runs of lines that differ between `pre` and `post`. */
static int find_runs(struct version *pre, struct version *post)
{
	struct versions vs = { .pre = pre, .post = post };
	xpparam_t xpp = { 0 };
	xdemitconf_t xecfg = { 0 };
	xdemitcb_t ecb = { NULL };

	xecfg.hunk_func = collect_runs;
	ecb.priv = &vs;
	return xdi_diff(&pre->content, &post->content, &xpp, &xecfg, &ecb);
}

/* The anchor of the `nr` lines of `v` from the 0-based line `start`. */
static void hash_lines(struct repository *r, const struct version *v,
		       size_t start, size_t nr, struct object_id *oid)
{
	hash_object_file(r->hash_algo, v->content.ptr + v->start[start],
			 v->start[start + nr] - v->start[start], OBJ_BLOB, oid);
}

struct candidate {
	size_t start;
	size_t distance;
};

static int candidate_cmp(const void *a_, const void *b_)
{
	const struct candidate *a = a_, *b = b_;

	if (a->distance != b->distance)
		return a->distance < b->distance ? -1 : 1;
	return a->start < b->start ? -1 : a->start > b->start;
}

/*
 * Locate `range` among the runs of `v`, closest to its line numbers
 * first, and move it there.  Returns 1 if it was found, and 0 if not.
 */
static int locate_range(struct repository *r, struct version *v,
			struct provenance_range *range)
{
	struct candidate *c = NULL;
	size_t c_nr = 0, c_alloc = 0, nr, want;
	int found = 0;

	if (!range->begin || range->end < range->begin ||
	    range->end - range->begin >= v->nr)
		return 0;
	nr = range->end - range->begin + 1;
	want = range->begin - 1;

	for (size_t i = 0; i < v->runs_nr; i++) {
		const struct run *run = &v->runs[i];
		size_t end = run->start + run->nr;

		for (size_t s = run->start; s + nr <= end; s++) {
			ALLOC_GROW(c, c_nr + 1, c_alloc);
			c[c_nr].start = s;
			c[c_nr].distance = s < want ? want - s : s - want;
			c_nr++;
		}
	}
	QSORT(c, c_nr, candidate_cmp);

	for (size_t i = 0; !found && i < c_nr; i++) {
		struct object_id oid;
		size_t s = c[i].start, j;

		/* a line cannot be taken by two ranges */
		for (j = 0; j < nr && !v->taken[s + j]; j++)
			;
		if (j < nr)
			continue;
		hash_lines(r, v, s, nr, &oid);
		if (!oideq(&oid, &range->anchor))
			continue;
		memset(v->taken + s, 1, nr);
		range->begin = s + 1;
		range->end = s + nr;
		found = 1;
	}
	free(c);
	return found;
}

/*
 * Add an `UNKNOWN` range to `file` for each stretch of lines of `v` that
 * the commit changes and that no located range covers.
 */
static void add_unknown(struct repository *r, struct version *v, char side,
			struct provenance_file *file)
{
	for (size_t i = 0; i < v->runs_nr; i++) {
		const struct run *run = &v->runs[i];
		size_t end = run->start + run->nr;

		for (size_t s = run->start, e; s < end; s = e) {
			struct provenance_range *range;
			char taken = v->taken[s];

			for (e = s + 1; e < end && v->taken[e] == taken; e++)
				; /* find the end of the stretch */
			if (taken)
				continue;
			ALLOC_GROW(file->ranges, file->ranges_nr + 1,
				   file->ranges_alloc);
			range = &file->ranges[file->ranges_nr++];
			memset(range, 0, sizeof(*range));
			range->side = side;
			range->begin = s + 1;
			range->end = e;
			range->mark = PROVENANCE_MARK_UNKNOWN;
			range->actors = xstrdup("-");
			hash_lines(r, v, s, e - s, &range->anchor);
		}
	}
}

/* Locate the ranges of `file` in the changes from `pre` to `post`. */
static int locate_file(struct repository *r, const struct object_id *pre,
		       const struct object_id *post,
		       struct provenance_file *file)
{
	struct version old = { 0 }, new = { 0 };
	size_t kept = 0;
	int lost = 0, ret = 0;

	if (read_version(r, pre, file->path, &old) ||
	    read_version(r, post, file->path, &new)) {
		ret = -1;
		goto out;
	}
	if (find_runs(&old, &new)) {
		ret = error(_("unable to diff '%s'"), file->path);
		goto out;
	}

	for (size_t i = 0; i < file->ranges_nr; i++) {
		struct provenance_range *range = &file->ranges[i];

		if (locate_range(r, range->side == '+' ? &new : &old, range)) {
			file->ranges[kept++] = *range;
		} else {
			free(range->actors);
			lost = 1;
		}
	}
	file->ranges_nr = kept;
	if (lost) {
		add_unknown(r, &new, '+', file);
		add_unknown(r, &old, '-', file);
	}

out:
	version_release(&old);
	version_release(&new);
	return ret;
}

int provenance_patch_locate_ranges(struct repository *r,
				   const mmfile_t *pre, const mmfile_t *post,
				   struct provenance_range *ranges, size_t nr,
				   char *found)
{
	struct version old = { 0 }, new = { 0 };
	int ret = 0;

	old.content.ptr = xmemdupz(pre->ptr, pre->size);
	old.content.size = pre->size;
	split_lines(&old);
	new.content.ptr = xmemdupz(post->ptr, post->size);
	new.content.size = post->size;
	split_lines(&new);
	if (find_runs(&old, &new))
		ret = -1;
	for (size_t i = 0; !ret && i < nr; i++)
		found[i] = locate_range(r, ranges[i].side == '+' ? &new : &old,
					&ranges[i]);
	version_release(&old);
	version_release(&new);
	return ret;
}

int provenance_patch_locate(struct repository *r, struct commit *commit,
			    struct provenance_record *rec)
{
	const struct object_id *pre = r->hash_algo->empty_tree, *post;
	size_t kept = 0;

	if (repo_parse_commit(r, commit))
		return error(_("could not parse commit %s"),
			     oid_to_hex(&commit->object.oid));
	post = get_commit_tree_oid(commit);
	if (commit->parents) {
		struct commit *parent = commit->parents->item;

		if (repo_parse_commit(r, parent))
			return error(_("could not parse commit %s"),
				     oid_to_hex(&parent->object.oid));
		pre = get_commit_tree_oid(parent);
	}

	for (size_t i = 0; i < rec->files_nr; i++)
		if (locate_file(r, pre, post, &rec->files[i]))
			return -1;

	/* drop the blocks of the paths that kept no range */
	for (size_t i = 0; i < rec->files_nr; i++) {
		struct provenance_file *file = &rec->files[i];

		if (file->ranges_nr) {
			rec->files[kept++] = *file;
		} else {
			free(file->path);
			free(file->ranges);
		}
	}
	rec->files_nr = kept;
	return 0;
}
