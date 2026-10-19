/*
 * Summing the line records of a revision walk.
 */

#include "git-compat-util.h"
#include "provenance-stat.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "notes.h"
#include "odb.h"
#include "refs.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "tag.h"

void provenance_stat_add_record(struct provenance_stat *stat,
				const struct provenance_record *rec)
{
	for (size_t i = 0; i < rec->files_nr; i++) {
		const struct provenance_file *file = &rec->files[i];

		for (size_t j = 0; j < file->ranges_nr; j++) {
			const struct provenance_range *range = &file->ranges[j];
			uintmax_t *lines = range->side == '+' ?
				stat->added : stat->removed;

			if (range->end < range->begin)
				continue;
			lines[range->mark] += range->end - range->begin + 1;
		}
	}
}

uintmax_t provenance_stat_share(const struct provenance_stat *stat)
{
	uintmax_t total = 0, ai;

	for (size_t i = 0; i < ARRAY_SIZE(stat->added); i++)
		total += stat->added[i];
	ai = stat->added[PROVENANCE_MARK_AI] +
	     stat->added[PROVENANCE_MARK_MIXED];
	return total ? (ai * 100 + total / 2) / total : 0;
}

/*
 * Load the notes tree of refs/provenance/lines into `t`.  Returns 1 if
 * it was loaded, 0 if the ref does not exist, and -1, after reporting
 * an error, if it cannot be read or does not point to a tree.
 */
static int load_notes(struct repository *r, struct notes_tree *t)
{
	struct object_id oid;
	struct object *obj;
	int flags = 0;

	if (!refs_resolve_ref_unsafe(get_main_ref_store(r),
				     PROVENANCE_LINES_REF, 0, &oid, &flags) ||
	    (flags & REF_ISBROKEN))
		return error(_("unable to read ref '%s'"), PROVENANCE_LINES_REF);
	if (is_null_oid(&oid))
		return 0;

	obj = deref_tag(r, parse_object(r, &oid), PROVENANCE_LINES_REF, 0);
	if (!obj || (obj->type != OBJ_COMMIT && obj->type != OBJ_TREE))
		return error(_("ref '%s' does not point to a tree"),
			     PROVENANCE_LINES_REF);

	init_notes(t, PROVENANCE_LINES_REF, NULL, 0);
	return 1;
}

/*
 * Set `lines` to the lines of the line record of `commit` stored in
 * `blob`.  Returns 0 on success, and -1, after a warning, if the record
 * cannot be read or parsed.
 */
static int record_lines(struct repository *r, const struct object_id *commit,
			const struct object_id *blob,
			struct provenance_stat *lines)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct strbuf err = STRBUF_INIT;
	enum object_type type;
	size_t size;
	void *data;
	int ret = -1;

	memset(lines, 0, sizeof(*lines));
	data = odb_read_object(r->objects, blob, &type, &size);
	if (!data || type != OBJ_BLOB)
		warning(_("ignoring unreadable provenance record of %s"),
			oid_to_hex(commit));
	else if (provenance_record_parse(data, size, r->hash_algo,
					 &rec, &err))
		warning(_("ignoring malformed provenance record of %s: %s"),
			oid_to_hex(commit), err.buf);
	else
		ret = 0;
	if (!ret)
		provenance_stat_add_record(lines, &rec);

	provenance_record_release(&rec);
	strbuf_release(&err);
	free(data);
	return ret;
}

static void add_lines(struct provenance_stat *stat,
		      const struct provenance_stat *lines)
{
	for (size_t i = 0; i < ARRAY_SIZE(stat->added); i++) {
		stat->added[i] += lines->added[i];
		stat->removed[i] += lines->removed[i];
	}
}

int provenance_stat_walk(struct repository *r, struct rev_info *revs,
			 struct provenance_stat *stat)
{
	struct notes_tree t = { 0 };
	struct commit *commit;
	int have_notes, ret = 0;

	memset(stat, 0, sizeof(*stat));
	have_notes = load_notes(r, &t);
	if (have_notes < 0)
		return -1;
	if (prepare_revision_walk(revs)) {
		ret = error(_("revision walk setup failed"));
		goto out;
	}

	while ((commit = get_revision(revs))) {
		const struct object_id *blob;
		struct provenance_stat lines;

		stat->commits++;
		if (!have_notes)
			continue;
		blob = get_note(&t, &commit->object.oid);
		if (!blob || record_lines(r, &commit->object.oid, blob, &lines))
			continue;
		stat->recorded++;
		add_lines(stat, &lines);
	}

out:
	if (have_notes)
		free_notes(&t);
	return ret;
}
