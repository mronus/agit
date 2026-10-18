/*
 * Giving provenance records to commits made without the extension.
 */

#include "git-compat-util.h"
#include "provenance-adopt.h"
#include "commit.h"
#include "commit-reach.h"
#include "diff.h"
#include "diffcore.h"
#include "gettext.h"
#include "hex.h"
#include "ident.h"
#include "notes.h"
#include "odb.h"
#include "provenance.h"
#include "provenance-attribute.h"
#include "provenance-commit.h"
#include "provenance-journal.h"
#include "refs.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "strmap.h"

static int add_candidate(const struct object_id *object_oid,
			 const struct object_id *note_oid UNUSED,
			 char *note_path UNUSED, void *data)
{
	struct provenance_adopt *pa = data;
	struct commit *commit;

	if (!odb_has_object(pa->r->objects, object_oid, 0) ||
	    odb_read_object_info(pa->r->objects, object_oid, NULL) != OBJ_COMMIT) {
		warning(_("ignoring the provenance record of %s: not a commit"),
			oid_to_hex(object_oid));
		return 0;
	}
	commit = lookup_commit(pa->r, object_oid);
	if (!commit || repo_parse_commit(pa->r, commit)) {
		warning(_("ignoring the provenance record of %s: cannot parse the commit"),
			oid_to_hex(object_oid));
		return 0;
	}
	/* a record that does not parse is warned about here */
	if (!provenance_cache_get(&pa->cache, object_oid))
		return 0;
	ALLOC_GROW(pa->candidates, pa->candidates_nr + 1, pa->candidates_alloc);
	pa->candidates[pa->candidates_nr++] = commit;
	return 0;
}

/* Keep the candidates that `bases` can reach, or those it cannot. */
static void filter_reachable(struct provenance_adopt *pa,
			     struct commit **bases, size_t bases_nr,
			     int reachable)
{
	struct commit_list *list = NULL;
	size_t nr = 0;

	for (size_t i = 0; i < bases_nr; i++)
		commit_list_insert(bases[i], &list);
	tips_reachable_from_bases(pa->r, list, pa->candidates,
				  pa->candidates_nr, UNINTERESTING);
	for (size_t i = 0; i < pa->candidates_nr; i++) {
		struct commit *c = pa->candidates[i];
		int found = !!(c->object.flags & UNINTERESTING);

		c->object.flags &= ~UNINTERESTING;
		if (found == reachable)
			pa->candidates[nr++] = c;
	}
	pa->candidates_nr = nr;
	commit_list_free(list);
}

static int compare_date(const void *a_, const void *b_)
{
	const struct commit *a = *(const struct commit **)a_;
	const struct commit *b = *(const struct commit **)b_;

	if (a->date != b->date)
		return a->date < b->date ? -1 : 1;
	return oidcmp(&a->object.oid, &b->object.oid);
}

void provenance_adopt_init(struct provenance_adopt *pa, struct repository *r,
			   struct commit **targets, size_t targets_nr,
			   struct commit **from, size_t from_nr)
{
	struct notes_tree t = { 0 };

	memset(pa, 0, sizeof(*pa));
	pa->r = r;
	provenance_cache_init(&pa->cache, r);

	init_notes(&t, PROVENANCE_LINES_REF, NULL, 0);
	for_each_note(&t, 0, add_candidate, pa);
	free_notes(&t);

	/* the walks below start from commits without marks */
	repo_clear_commit_marks(r, ALL_REV_FLAGS);
	/* a commit in the history of a target is not its source */
	filter_reachable(pa, targets, targets_nr, 0);
	if (from_nr)
		filter_reachable(pa, from, from_nr, 1);

	QSORT(pa->candidates, pa->candidates_nr, compare_date);
}

void provenance_adopt_release(struct provenance_adopt *pa)
{
	provenance_cache_release(&pa->cache);
	free(pa->candidates);
}

/*
 * Return 1 if `commit` has a line record that `force` does not let
 * adopt replace, 0 if it has none or an adopted one, and -1 if it
 * cannot be read.
 */
static int keeps_record(struct repository *r, struct commit *commit, int force)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret;

	ret = provenance_read_lines(r, &commit->object.oid, &buf);
	if (ret > 0 && force &&
	    !provenance_record_parse(buf.buf, buf.len, r->hash_algo,
				     &rec, &err) &&
	    rec.capture == PROVENANCE_CAPTURE_ADOPTED)
		ret = 0;
	provenance_record_release(&rec);
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

/* Collect the paths that `commit` changes against its first parent. */
static void changed_paths(struct repository *r, struct commit *commit,
			  struct strset *paths)
{
	const struct object_id *base = r->hash_algo->empty_tree;
	struct diff_options opt;

	if (commit->parents)
		base = get_commit_tree_oid(commit->parents->item);
	repo_diff_setup(r, &opt);
	opt.flags.recursive = 1;
	opt.output_format = DIFF_FORMAT_NO_OUTPUT;
	opt.detect_rename = 0;
	diff_setup_done(&opt);
	diff_tree_oid(base, get_commit_tree_oid(commit), "", &opt);
	for (int i = 0; i < diff_queued_diff.nr; i++)
		strset_add(paths, diff_queued_diff.queue[i]->two->path);
	diff_flush(&opt);
}

/* Whether the record `rec` has a block for one of `paths`. */
static int touches(const struct provenance_record *rec,
		   struct strset *paths)
{
	for (size_t i = 0; i < rec->files_nr; i++)
		if (strset_contains(paths, rec->files[i].path))
			return 1;
	return 0;
}

/* Read the reasoning record of `commit` into `rec`; 1 if it has one. */
static int read_reasoning(struct repository *r, struct commit *commit,
			  struct provenance_reasoning *rec)
{
	const struct object_id *oid = &commit->object.oid;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret = provenance_read_reasoning(r, oid, &buf);

	if (ret > 0 &&
	    provenance_reasoning_parse(buf.buf, buf.len, r->hash_algo,
				       rec, &err)) {
		warning(_("ignoring the reasoning record of %s: %s"),
			oid_to_hex(oid), err.buf);
		ret = 0;
	}
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

/* Whether a range of `rec` says who wrote its lines. */
static int has_known_lines(const struct provenance_record *rec)
{
	for (size_t i = 0; i < rec->files_nr; i++)
		for (size_t j = 0; j < rec->files[i].ranges_nr; j++)
			if (rec->files[i].ranges[j].mark != PROVENANCE_MARK_UNKNOWN)
				return 1;
	return 0;
}

/* The author of `commit`, as "Name <email>". */
static char *author_of(struct repository *r, struct commit *commit)
{
	const char *buffer = repo_get_commit_buffer(r, commit, NULL);
	const char *author;
	struct ident_split ident;
	char *ret = NULL;
	size_t len;

	author = find_commit_header(buffer, "author", &len);
	if (!author || split_ident_line(&ident, author, len))
		error(_("malformed author in commit %s"),
		      oid_to_hex(&commit->object.oid));
	else
		ret = xstrfmt("%.*s <%.*s>",
			      (int)(ident.name_end - ident.name_begin),
			      ident.name_begin,
			      (int)(ident.mail_end - ident.mail_begin),
			      ident.mail_begin);
	repo_unuse_commit_buffer(r, commit, buffer);
	return ret;
}

static int write_records(struct repository *r, struct commit *commit,
			 struct provenance_record *lines,
			 struct provenance_reasoning *reasoning)
{
	struct ref_transaction *transaction;
	struct strbuf err = STRBUF_INIT;
	int ret = 0;

	transaction = ref_store_transaction_begin(get_main_ref_store(r), 0, &err);
	if (!transaction ||
	    provenance_queue_records(r, transaction, &commit->object.oid,
				     lines, reasoning, "provenance adopt",
				     &err) ||
	    ref_transaction_commit(transaction, &err))
		ret = error("%s", err.buf);
	ref_transaction_free(transaction);
	strbuf_release(&err);
	return ret;
}

int provenance_adopt_commit(struct provenance_adopt *pa, struct commit *commit,
			    int force, enum provenance_adopt_result *result)
{
	struct repository *r = pa->r;
	struct provenance_record lines = PROVENANCE_RECORD_INIT;
	struct provenance_reasoning reasoning = PROVENANCE_REASONING_INIT;
	struct provenance_journal journal = PROVENANCE_JOURNAL_INIT;
	struct provenance_source *sources = NULL;
	struct provenance_reasoning *reasonings = NULL;
	struct strset paths = STRSET_INIT;
	size_t nr = 0;
	char *human = NULL;
	int ret;

	*result = PROVENANCE_ADOPT_NO_SOURCE;
	if (repo_parse_commit(r, commit))
		return error(_("could not parse commit %s"),
			     oid_to_hex(&commit->object.oid));
	ret = keeps_record(r, commit, force);
	if (ret) {
		*result = PROVENANCE_ADOPT_HAS_RECORD;
		return ret < 0 ? -1 : 0;
	}
	if (commit->parents && commit->parents->next) {
		*result = PROVENANCE_ADOPT_MERGE;
		return 0;
	}
	if (commit->parents && repo_parse_commit(r, commit->parents->item))
		return error(_("could not parse commit %s"),
			     oid_to_hex(&commit->parents->item->object.oid));

	changed_paths(r, commit, &paths);
	CALLOC_ARRAY(sources, pa->candidates_nr);
	CALLOC_ARRAY(reasonings, pa->candidates_nr);
	for (size_t i = 0; i < pa->candidates_nr; i++) {
		struct commit *c = pa->candidates[i];
		const struct provenance_record *rec =
			provenance_cache_get(&pa->cache, &c->object.oid);

		if (!rec || !touches(rec, &paths))
			continue;
		reasonings[nr] = (struct provenance_reasoning)
			PROVENANCE_REASONING_INIT;
		sources[nr].commit = c;
		sources[nr].lines = rec;
		sources[nr].tentative = 1;
		if (read_reasoning(r, c, &reasonings[nr]) > 0)
			sources[nr].reasoning = &reasonings[nr];
		nr++;
	}
	if (!nr)
		goto out;

	human = author_of(r, commit);
	if (!human ||
	    provenance_attribute_rewrite(r, commit->parents ?
					 commit->parents->item : NULL,
					 get_commit_tree_oid(commit),
					 sources, nr, PROVENANCE_REWRITE_ADOPT,
					 &journal, NULL, human,
					 &lines, &reasoning)) {
		ret = -1;
		goto out;
	}

	/* an all-UNKNOWN record would only stand in the way of a later adopt */
	if (!has_known_lines(&lines))
		goto out;
	ret = write_records(r, commit, &lines, &reasoning);
	if (!ret)
		*result = PROVENANCE_ADOPT_ADOPTED;

out:
	for (size_t i = 0; i < nr; i++)
		provenance_reasoning_release(&reasonings[i]);
	free(reasonings);
	free(sources);
	strset_clear(&paths);
	provenance_record_release(&lines);
	provenance_reasoning_release(&reasoning);
	free(human);
	return ret;
}
