/*
 * Showing provenance records to the user.
 */

#include "git-compat-util.h"
#include "provenance-view.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "gettext.h"
#include "hex.h"
#include "provenance.h"
#include "provenance-attribute.h"
#include "provenance-journal.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"

struct cached_record {
	struct oidmap_entry entry;
	/* the record is valid */
	int found;
	struct provenance_record rec;
};

void provenance_cache_init(struct provenance_cache *cache,
			   struct repository *r)
{
	cache->r = r;
	oidmap_init(&cache->records, 0);
	cache->have_ref = -1;
}

static void free_cached_record(void *p)
{
	struct cached_record *c = p;

	provenance_record_release(&c->rec);
	free(c);
}

void provenance_cache_release(struct provenance_cache *cache)
{
	oidmap_clear_with_free(&cache->records, free_cached_record);
	cache->have_ref = -1;
}

/*
 * Read and parse the line record of `commit` into `rec`.  Returns 0 on
 * success, or -1 if there is no record or, after a warning, if it does
 * not parse.  `rec` must be released in either case.
 */
static int read_lines(struct repository *r, const struct object_id *commit,
		      struct provenance_record *rec)
{
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret = -1;

	if (provenance_read_lines(r, commit, &buf) <= 0)
		goto out;
	if (provenance_record_parse(buf.buf, buf.len, r->hash_algo,
				    rec, &err)) {
		warning(_("ignoring malformed provenance record of %s: %s"),
			oid_to_hex(commit), err.buf);
		goto out;
	}
	ret = 0;
out:
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

static void load_record(struct provenance_cache *cache,
			struct cached_record *c)
{
	if (!read_lines(cache->r, &c->entry.oid, &c->rec)) {
		if (oideq(&c->rec.commit, &c->entry.oid))
			c->found = 1;
		else
			warning(_("ignoring provenance record of %s: it names commit %s"),
				oid_to_hex(&c->entry.oid),
				oid_to_hex(&c->rec.commit));
	}
	if (!c->found)
		provenance_record_release(&c->rec);
}

const struct provenance_record *
provenance_cache_get(struct provenance_cache *cache,
		     const struct object_id *commit)
{
	struct cached_record *c;

	if (cache->have_ref < 0)
		cache->have_ref = refs_ref_exists(get_main_ref_store(cache->r),
						  PROVENANCE_LINES_REF);
	if (!cache->have_ref)
		return NULL;

	c = oidmap_get(&cache->records, commit);
	if (!c) {
		CALLOC_ARRAY(c, 1);
		oidcpy(&c->entry.oid, commit);
		oidmap_put(&cache->records, c);
		load_record(cache, c);
	}
	return c->found ? &c->rec : NULL;
}

struct provenance_diff {
	enum provenance_diff_source source;
	/* PROVENANCE_DIFF_COMMIT */
	struct object_id commit;
	struct provenance_cache cache;
	/* the live sources */
	struct provenance_journal journal;
	struct provenance_attribution *live;
};

static struct provenance_diff *diff_state(struct diff_options *opt)
{
	struct provenance_diff *pd = opt->provenance;

	if (!pd) {
		CALLOC_ARRAY(pd, 1);
		provenance_cache_init(&pd->cache, opt->repo);
		opt->provenance = pd;
	}
	return pd;
}

static void release_live(struct provenance_diff *pd)
{
	provenance_attribution_free(pd->live);
	pd->live = NULL;
	provenance_journal_release(&pd->journal);
}

void provenance_diff_release(struct diff_options *opt)
{
	struct provenance_diff *pd = opt->provenance;

	if (!pd)
		return;
	release_live(pd);
	provenance_cache_release(&pd->cache);
	FREE_AND_NULL(opt->provenance);
}

void provenance_diff_unset(struct diff_options *opt)
{
	if (opt->provenance)
		opt->provenance->source = PROVENANCE_DIFF_NONE;
}

/*
 * Find the tree of the first parent of `commit` as recorded in the
 * commit itself, which is what its line record is relative to, even if
 * the history walk rewrote its parents.
 */
static int first_parent_tree(struct repository *r, struct commit *commit,
			     struct object_id *tree)
{
	const char *buf = repo_get_commit_buffer(r, commit, NULL);
	const char *p = buf;
	struct object_id parent_oid;
	struct commit *parent;
	int has_parent;

	if (!buf)
		return -1;
	has_parent = skip_prefix(p, "tree ", &p) &&
		     !parse_oid_hex_algop(p, &parent_oid, &p, r->hash_algo) &&
		     skip_prefix(p, "\nparent ", &p) &&
		     !parse_oid_hex_algop(p, &parent_oid, &p, r->hash_algo);
	repo_unuse_commit_buffer(r, commit, buf);

	if (!has_parent) {
		oidcpy(tree, r->hash_algo->empty_tree);
		return 0;
	}
	/* the parent may be missing, as at the boundary of a shallow clone */
	parent = lookup_commit(r, &parent_oid);
	if (!parent || repo_parse_commit_gently(r, parent, 1))
		return -1;
	oidcpy(tree, get_commit_tree_oid(parent));
	return 0;
}

void provenance_diff_set_commit(struct diff_options *opt,
				struct commit *commit,
				const struct object_id *old_tree)
{
	struct provenance_diff *pd;
	struct object_id tree;

	if (!opt->flags.provenance)
		return;
	pd = diff_state(opt);
	pd->source = PROVENANCE_DIFF_NONE;
	if (first_parent_tree(opt->repo, commit, &tree) ||
	    !oideq(&tree, old_tree))
		return;

	/* keep only the record of the commit being shown */
	if (!oideq(&pd->commit, &commit->object.oid)) {
		int have_ref = pd->cache.have_ref;

		provenance_cache_release(&pd->cache);
		pd->cache.have_ref = have_ref;
		oidcpy(&pd->commit, &commit->object.oid);
	}
	pd->source = PROVENANCE_DIFF_COMMIT;
}

void provenance_diff_set_live(struct diff_options *opt,
			      enum provenance_diff_source source,
			      const struct object_id *old_tree)
{
	struct repository *r = opt->repo;
	struct provenance_diff *pd;
	struct object_id head, base;

	if (!opt->flags.provenance)
		return;
	pd = diff_state(opt);
	pd->source = PROVENANCE_DIFF_NONE;
	release_live(pd);
	if (!provenance_enabled(r))
		return;

	if (refs_read_ref(get_main_ref_store(r), "HEAD", &head)) {
		/* an unborn branch: the next commit is a root commit */
		oidcpy(&base, r->hash_algo->empty_tree);
	} else {
		struct commit *commit = lookup_commit_reference(r, &head);

		if (!commit || repo_parse_commit(r, commit))
			return;
		oidcpy(&base, get_commit_tree_oid(commit));
	}
	if (source != PROVENANCE_DIFF_FILES &&
	    (!old_tree || !oideq(old_tree, &base)))
		return;

	if (provenance_journal_read(r, &pd->journal))
		return;
	pd->live = provenance_attribution_new(r, &base, &pd->journal);
	pd->source = source;
}

/* Set the marks of the lines covered by the ranges on `side` of `file`. */
static void apply_ranges(char *marks, size_t nr,
			 const struct provenance_file *file, char side)
{
	if (!file)
		return;
	for (size_t i = 0; i < file->ranges_nr; i++) {
		const struct provenance_range *range = &file->ranges[i];

		if (range->side != side)
			continue;
		for (unsigned long l = range->begin; l <= range->end; l++) {
			if (!l || l > nr)
				break;
			marks[l - 1] = provenance_mark_char(range->mark);
		}
	}
}

/*
 * Attribute `path` with the live source, as if its content were `buf`
 * (NULL if deleted), and apply its '+' ranges to the post-image marks
 * `post` and its '-' ranges to the pre-image marks `pre`, if given.
 */
static void apply_live(struct provenance_diff *pd, const char *path,
		       const char *buf, size_t len, unsigned mode,
		       char *post, size_t post_nr, char *pre, size_t pre_nr)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;

	if (!provenance_attribute_path(pd->live, path, buf, len, mode, &rec)) {
		const struct provenance_file *file =
			provenance_record_file(&rec, path);

		if (post)
			apply_ranges(post, post_nr, file, '+');
		if (pre)
			apply_ranges(pre, pre_nr, file, '-');
	}
	provenance_record_release(&rec);
}

static size_t count_lines(const char *buf, size_t len)
{
	size_t nr = 0;

	for (const char *p = buf, *end = buf + len; p < end; nr++) {
		const char *eol = memchr(p, '\n', end - p);

		p = eol ? eol + 1 : end;
	}
	return nr;
}

void provenance_diff_marks(struct diff_options *opt,
			   const struct diff_filespec *one,
			   const struct diff_filespec *two, char status,
			   const char *pre, size_t pre_len,
			   const char *post, size_t post_len,
			   struct provenance_marks *marks)
{
	struct provenance_diff *pd = opt->provenance;
	enum provenance_diff_source source = pd ? pd->source : PROVENANCE_DIFF_NONE;
	char *pre_marks, *post_marks;
	size_t pre_nr, post_nr;
	int same_path;

	/* the filepair is shown reversed: attribute it the right way round */
	if (opt->flags.reverse_diff) {
		SWAP(one, two);
		SWAP(pre, post);
		SWAP(pre_len, post_len);
	}
	pre_nr = count_lines(pre, pre_len);
	post_nr = count_lines(post, post_len);
	pre_marks = xmallocz(pre_nr);
	post_marks = xmallocz(post_nr);
	memset(pre_marks, '?', pre_nr);
	memset(post_marks, '?', post_nr);
	same_path = !strcmp(one->path, two->path);

	switch (source) {
	case PROVENANCE_DIFF_NONE:
		break;
	case PROVENANCE_DIFF_COMMIT: {
		const struct provenance_record *rec =
			provenance_cache_get(&pd->cache, &pd->commit);

		if (!rec)
			break;
		apply_ranges(post_marks, post_nr,
			     provenance_record_file(rec, two->path), '+');
		/* a copy leaves its source alone */
		if (status != DIFF_STATUS_COPIED)
			apply_ranges(pre_marks, pre_nr,
				     provenance_record_file(rec, one->path), '-');
		break;
	}
	case PROVENANCE_DIFF_INDEX:
	case PROVENANCE_DIFF_WORKTREE:
		/*
		 * The pre-image is HEAD: attribute the post-image as
		 * "git commit" would, the pre-image of a renamed path as
		 * a deletion.
		 */
		apply_live(pd, two->path,
			   DIFF_FILE_VALID(two) ? post : NULL, post_len,
			   two->mode, post_marks, post_nr,
			   same_path ? pre_marks : NULL, pre_nr);
		if (!same_path && status == DIFF_STATUS_RENAMED)
			apply_live(pd, one->path, NULL, 0, 0,
				   NULL, 0, pre_marks, pre_nr);
		break;
	case PROVENANCE_DIFF_FILES:
		/*
		 * The pre-image is the index, which the journal cannot
		 * speak for; the post-image is the working tree, whose
		 * lines are marked as "git commit -a" would mark them.
		 */
		apply_live(pd, two->path,
			   DIFF_FILE_VALID(two) ? post : NULL, post_len,
			   two->mode, post_marks, post_nr, NULL, 0);
		break;
	}

	if (opt->flags.reverse_diff) {
		SWAP(pre_marks, post_marks);
		SWAP(pre_nr, post_nr);
	}
	marks->pre = pre_marks;
	marks->pre_nr = pre_nr;
	marks->post = post_marks;
	marks->post_nr = post_nr;
}

char provenance_marks_get(const struct provenance_marks *marks, int post,
			  long line)
{
	const char *m = post ? marks->post : marks->pre;
	size_t nr = post ? marks->post_nr : marks->pre_nr;

	if (!m || line < 1 || (size_t)line > nr)
		return '?';
	return m[line - 1];
}

void provenance_marks_release(struct provenance_marks *marks)
{
	FREE_AND_NULL(marks->pre);
	FREE_AND_NULL(marks->post);
	marks->pre_nr = marks->post_nr = 0;
}
