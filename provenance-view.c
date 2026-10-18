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
#include "path.h"
#include "provenance.h"
#include "provenance-attribute.h"
#include "provenance-journal.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"
#include "string-list.h"

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

struct provenance_diff {
	enum provenance_diff_source source;
	/* PROVENANCE_DIFF_COMMIT */
	struct object_id commit;
	struct provenance_cache cache;
	/* the live sources */
	struct provenance_journal journal;
	struct provenance_attribution *live;
	/*
	 * The commit shown, or the one that the live sources would make,
	 * is a merge: the lines its record does not cover came from its
	 * other parents.
	 */
	unsigned merge : 1;
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
 * the history walk rewrote its parents, and whether it records other
 * parents.
 */
static int first_parent_tree(struct repository *r, struct commit *commit,
			     struct object_id *tree, int *merge)
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
	*merge = has_parent && starts_with(p, "\nparent ");
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
	int merge;

	if (!opt->flags.provenance)
		return;
	pd = diff_state(opt);
	pd->source = PROVENANCE_DIFF_NONE;
	if (first_parent_tree(opt->repo, commit, &tree, &merge) ||
	    !oideq(&tree, old_tree))
		return;
	pd->merge = merge;

	/* keep only the record of the commit being shown */
	if (!oideq(&pd->commit, &commit->object.oid)) {
		int have_ref = pd->cache.have_ref;

		provenance_cache_release(&pd->cache);
		pd->cache.have_ref = have_ref;
		oidcpy(&pd->commit, &commit->object.oid);
	}
	pd->source = PROVENANCE_DIFF_COMMIT;
}

/*
 * The other parents of the merge that "git commit" would conclude, as
 * MERGE_HEAD names them, or NULL.
 */
static struct commit_list *merge_heads(struct repository *r)
{
	struct strbuf buf = STRBUF_INIT;
	struct commit_list *heads = NULL, **tail = &heads;
	const char *p;

	if (strbuf_read_file(&buf, git_path_merge_head(r), 0) < 0)
		return NULL;
	for (p = buf.buf; *p; ) {
		struct object_id oid;
		struct commit *commit;

		if (parse_oid_hex_algop(p, &oid, &p, r->hash_algo))
			break;
		commit = lookup_commit_reference(r, &oid);
		if (!commit)
			break;
		tail = commit_list_append(commit, tail);
		p += strspn(p, "\n");
	}
	strbuf_release(&buf);
	return heads;
}

void provenance_diff_set_live(struct diff_options *opt,
			      enum provenance_diff_source source,
			      const struct object_id *old_tree)
{
	struct repository *r = opt->repo;
	struct provenance_diff *pd;
	struct object_id head, base;
	struct commit_list *others = NULL;

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
	if (source != PROVENANCE_DIFF_FILES)
		others = merge_heads(r);
	pd->live = provenance_attribution_new(r, &base, others, &pd->journal);
	pd->merge = !!others;
	commit_list_free(others);
	if (pd->live)
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
		/* the lines a merge did not write came from other parents */
		if (pd->merge) {
			memset(pre_marks, ' ', pre_nr);
			memset(post_marks, ' ', post_nr);
		}
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
		if (pd->merge) {
			memset(pre_marks, ' ', pre_nr);
			memset(post_marks, ' ', post_nr);
		}
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

/* As read_lines(), for the reasoning record of `commit`. */
static int read_reasoning(struct repository *r, const struct object_id *commit,
			  struct provenance_reasoning *rec)
{
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret = -1;

	if (provenance_read_reasoning(r, commit, &buf) <= 0)
		goto out;
	if (provenance_reasoning_parse(buf.buf, buf.len, r->hash_algo,
				       rec, &err)) {
		warning(_("ignoring malformed reasoning record of %s: %s"),
			oid_to_hex(commit), err.buf);
		goto out;
	}
	ret = 0;
out:
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

/* The share of the added lines that are marked AI or MIXED, in percent. */
static void format_share(const struct provenance_record *rec,
			 struct strbuf *sb)
{
	uintmax_t total = 0, ai = 0;

	for (size_t i = 0; i < rec->files_nr; i++) {
		const struct provenance_file *file = &rec->files[i];

		for (size_t j = 0; j < file->ranges_nr; j++) {
			const struct provenance_range *range = &file->ranges[j];
			uintmax_t n;

			if (range->side != '+' || range->end < range->begin)
				continue;
			n = range->end - range->begin + 1;
			total += n;
			if (range->mark == PROVENANCE_MARK_AI ||
			    range->mark == PROVENANCE_MARK_MIXED)
				ai += n;
		}
	}
	strbuf_addf(sb, "%"PRIuMAX"%%",
		    total ? (ai * 100 + total / 2) / total : 0);
}

/* The distinct models (or sessions) of the AI actors, in order. */
static void format_actors(const struct provenance_record *rec, int sessions,
			  struct strbuf *sb)
{
	struct string_list seen = STRING_LIST_INIT_NODUP;

	for (size_t i = 0; i < rec->actors_nr; i++) {
		const struct provenance_actor *actor = &rec->actors[i];
		char *value;

		if (actor->kind != PROVENANCE_ACTOR_AI)
			continue;
		value = sessions ? actor->session : actor->model;
		if (unsorted_string_list_has_string(&seen, value))
			continue;
		if (seen.nr)
			strbuf_addch(sb, ',');
		strbuf_addstr(sb, value);
		string_list_append(&seen, value);
	}
	string_list_clear(&seen, 0);
}

int provenance_format_field(struct repository *r,
			    const struct object_id *commit,
			    const char *field, size_t len,
			    struct strbuf *sb)
{
	int share = !xstrncmpz("share", field, len);
	int models = !xstrncmpz("models", field, len);
	int sessions = !xstrncmpz("sessions", field, len);

	if (!xstrncmpz("summary", field, len)) {
		struct provenance_reasoning rec = PROVENANCE_REASONING_INIT;

		if (!read_reasoning(r, commit, &rec)) {
			strbuf_strip_suffix(&rec.summary, "\n");
			strbuf_addbuf(sb, &rec.summary);
		}
		provenance_reasoning_release(&rec);
	} else if (share || models || sessions) {
		struct provenance_record rec = PROVENANCE_RECORD_INIT;

		if (!read_lines(r, commit, &rec)) {
			if (share)
				format_share(&rec, sb);
			else
				format_actors(&rec, sessions, sb);
		}
		provenance_record_release(&rec);
	} else {
		return -1;
	}
	return 0;
}
