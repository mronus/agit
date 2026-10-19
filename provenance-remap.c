/*
 * Carrying provenance records over to the commits that replace theirs,
 * and pruning the records of commits that are gone.
 */

#include "git-compat-util.h"
#include "provenance-remap.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "ident.h"
#include "notes.h"
#include "odb.h"
#include "oid-array.h"
#include "provenance.h"
#include "provenance-commit.h"
#include "provenance-patch.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"

/* A provenance ref, with the notes tree of the commit it points to. */
struct records_ref {
	const char *name;
	/* the null id if the ref does not exist */
	struct object_id old_oid;
	struct notes_tree t;
	int changed;
};

static void records_ref_init(struct repository *r, struct records_ref *ref,
			     const char *name)
{
	ref->name = name;
	ref->changed = 0;
	memset(&ref->t, 0, sizeof(ref->t));
	if (refs_read_ref(get_main_ref_store(r), name, &ref->old_oid)) {
		oidclr(&ref->old_oid, r->hash_algo);
		init_notes(&ref->t, name, combine_notes_overwrite,
			   NOTES_INIT_EMPTY);
	} else {
		/* the commit that the update replaces, as "purge" reads it */
		init_notes(&ref->t, oid_to_hex(&ref->old_oid),
			   combine_notes_overwrite, 0);
	}
}

/*
 * Queue the update of `ref` to a notes commit of its tree, on top of
 * the commit it points to, if the tree has changed.
 */
static int queue_ref(struct repository *r, struct ref_transaction *transaction,
		     struct records_ref *ref, const char *msg,
		     const char *reflog_msg, struct strbuf *err)
{
	struct object_id tree_oid, new_oid;
	struct commit_list *parents = NULL;
	int ret;

	if (!ref->changed)
		return 0;
	if (write_notes_tree(&ref->t, &tree_oid)) {
		strbuf_addf(err, _("unable to write the notes tree for '%s'"),
			    ref->name);
		return -1;
	}
	if (!is_null_oid(&ref->old_oid)) {
		struct commit *parent = lookup_commit(r, &ref->old_oid);

		if (!parent || repo_parse_commit(r, parent)) {
			strbuf_addf(err, _("unable to parse '%s'"), ref->name);
			return -1;
		}
		commit_list_insert(parent, &parents);
	}
	if (commit_tree(msg, strlen(msg), &tree_oid, parents, &new_oid,
			git_committer_info(IDENT_STRICT), NULL)) {
		strbuf_addf(err, _("unable to write the notes commit for '%s'"),
			    ref->name);
		ret = -1;
	} else {
		ret = ref_transaction_update(transaction, ref->name, &new_oid,
					     &ref->old_oid, NULL, NULL, 0,
					     reflog_msg, err);
	}
	commit_list_free(parents);
	return ret;
}

/* Update the `nr` refs at `refs` that have changed, in one transaction. */
static int write_refs(struct repository *r, struct records_ref *refs,
		      size_t nr, const char *msg, const char *reflog_msg)
{
	struct ref_transaction *transaction;
	struct strbuf err = STRBUF_INIT;
	int changed = 0, ret = 0;

	for (size_t i = 0; i < nr; i++)
		changed |= refs[i].changed;
	if (!changed)
		return 0;

	transaction =ref_store_transaction_begin(get_main_ref_store(r), 0,
						  &err);
	if (!transaction)
		ret = -1;
	for (size_t i = 0; !ret && i < nr; i++)
		ret = queue_ref(r, transaction, &refs[i], msg, reflog_msg,
				&err);
	if (!ret)
		ret = ref_transaction_commit(transaction, &err);
	if (ret)
		ret = error("%s", err.buf);
	ref_transaction_free(transaction);
	strbuf_release(&err);
	return ret;
}

static int read_record(struct repository *r, const struct object_id *blob,
		       struct strbuf *buf)
{
	enum object_type type;
	size_t size;
	void *data = odb_read_object(r->objects, blob, &type, &size);

	if (!data || type != OBJ_BLOB) {
		free(data);
		return error(_("unable to read provenance record '%s'"),
			     oid_to_hex(blob));
	}
	strbuf_attach(buf, data, size, size + 1);
	return 0;
}

/*
 * Replace the id on the first "commit" line of the record in `buf`
 * with that of `commit`.
 */
static void set_commit_line(struct strbuf *buf, const struct object_id *commit)
{
	const char *line = buf->buf;
	const char *hex = oid_to_hex(commit);
	size_t pos, len;

	while (!starts_with(line, "commit ")) {
		line = strchr(line, '\n');
		if (!line)
			BUG("provenance: a parsed record without a commit line");
		line++;
	}
	pos = (size_t)(line - buf->buf) + strlen("commit ");
	len = (size_t)(strchrnul(buf->buf + pos, '\n') - (buf->buf + pos));
	strbuf_splice(buf, pos, len, hex, strlen(hex));
}

/*
 * Turn `rec`, the line record of another commit with the same changes,
 * into the record of `commit` in `out`, as "git am" turns the record
 * that a patch mail carries into that of the commit it makes, but
 * adopted.
 */
static int reattach(struct repository *r, struct commit *commit,
		    struct provenance_record *rec, struct strbuf *out)
{
	struct provenance_rewrite rw = { 0 };
	struct provenance_record lines = PROVENANCE_RECORD_INIT;
	struct provenance_reasoning reasoning = PROVENANCE_REASONING_INIT;
	const char *buffer = repo_get_commit_buffer(r, commit, NULL);
	const char *author;
	char *ident = NULL;
	size_t len;
	int ret = -1;

	author = find_commit_header(buffer, "author", &len);
	if (!author) {
		error(_("malformed author in commit %s"),
		      oid_to_hex(&commit->object.oid));
		goto out;
	}
	ident = xmemdupz(author, len);
	if (provenance_patch_locate(r, commit, rec))
		goto out;

	rw.mail = commit;
	rw.mail_lines = rec;
	rw.adopt = 1;
	if (provenance_commit_records(r, commit->parents,
				      get_commit_tree_oid(commit), ident,
				      NULL, &rw, &lines, &reasoning))
		goto out;
	oidcpy(&lines.commit, &commit->object.oid);
	provenance_record_format(&lines, out);
	ret = 0;

out:
	repo_unuse_commit_buffer(r, commit, buffer);
	provenance_record_release(&lines);
	provenance_reasoning_release(&reasoning);
	free(ident);
	return ret;
}

/*
 * Write `buf` as the record of `new_oid` in `ref`, in place of that of
 * `old_oid`.  With `dry_run`, the old blob `blob` stands in for it.
 */
static int move_record(struct repository *r, struct records_ref *ref,
		       const struct object_id *old_oid,
		       const struct object_id *new_oid,
		       const struct strbuf *buf, const struct object_id *blob,
		       int dry_run)
{
	struct object_id oid;

	if (dry_run)
		oidcpy(&oid, blob);
	else if (odb_write_object(r->objects, buf->buf, buf->len, OBJ_BLOB,
				  &oid))
		return error(_("unable to write the record for '%s'"),
			     oid_to_hex(new_oid));
	remove_note(&ref->t, old_oid->hash);
	if (add_note(&ref->t, new_oid, &oid, combine_notes_overwrite))
		BUG("combine_notes_overwrite failed");
	ref->changed = 1;
	return 0;
}

/* Copy the id of the note of `commit` in `t` to `blob`, if it has one. */
static int find_record(struct notes_tree *t, const struct object_id *commit,
		       struct object_id *blob)
{
	const struct object_id *note = get_note(t, commit);

	if (note)
		oidcpy(blob, note);
	return !!note;
}

static int remap_one(struct repository *r, struct records_ref *lines,
		     struct records_ref *reasoning, struct provenance_remap *p,
		     int dry_run)
{
	const struct object_id *old_oid = &p->old_oid, *new_oid = &p->new_oid;
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct provenance_reasoning why = PROVENANCE_REASONING_INIT;
	struct strbuf lines_buf = STRBUF_INIT, reasoning_buf = STRBUF_INIT;
	struct strbuf err = STRBUF_INIT;
	struct object_id lines_blob, reasoning_blob;
	int has_lines, has_reasoning;
	struct commit *commit;
	int ret = 0;

	p->result = PROVENANCE_REMAP_NONE;
	if (oideq(old_oid, new_oid))
		return 0;
	has_lines = find_record(&lines->t, old_oid, &lines_blob);
	has_reasoning = find_record(&reasoning->t, old_oid, &reasoning_blob);
	if (!has_lines && !has_reasoning)
		return 0;

	if (odb_read_object_info(r->objects, new_oid, NULL) != OBJ_COMMIT ||
	    !(commit = lookup_commit(r, new_oid)) ||
	    repo_parse_commit(r, commit)) {
		p->result = PROVENANCE_REMAP_NOT_COMMIT;
		warning(_("not moving the records of %s: %s is not a commit"),
			oid_to_hex(old_oid), oid_to_hex(new_oid));
		return 0;
	}
	if (get_note(&lines->t, new_oid)) {
		p->result = PROVENANCE_REMAP_HAS_RECORD;
		warning(_("not moving the records of %s: %s has a line record"),
			oid_to_hex(old_oid), oid_to_hex(new_oid));
		return 0;
	}

	if ((has_lines && read_record(r, &lines_blob, &lines_buf)) ||
	    (has_reasoning && read_record(r, &reasoning_blob, &reasoning_buf))) {
		ret = -1;
		goto out;
	}
	if ((has_lines &&
	     provenance_record_parse(lines_buf.buf, lines_buf.len,
				     r->hash_algo, &rec, &err)) ||
	    (has_reasoning &&
	     provenance_reasoning_parse(reasoning_buf.buf, reasoning_buf.len,
					r->hash_algo, &why, &err))) {
		p->result = PROVENANCE_REMAP_BAD_RECORD;
		warning(_("not moving the records of %s: %s"),
			oid_to_hex(old_oid), err.buf);
		goto out;
	}

	p->result = PROVENANCE_REMAP_MOVED;
	if (has_lines) {
		oidcpy(&rec.commit, new_oid);
		if (!provenance_verify(r, commit, &rec, &err)) {
			set_commit_line(&lines_buf, new_oid);
		} else {
			/* the changes differ: find the ranges by content */
			p->result = PROVENANCE_REMAP_REATTACHED;
			strbuf_reset(&lines_buf);
			if (reattach(r, commit, &rec, &lines_buf)) {
				ret = -1;
				goto out;
			}
		}
		if (move_record(r, lines, old_oid, new_oid, &lines_buf,
				&lines_blob, dry_run)) {
			ret = -1;
			goto out;
		}
	}
	if (has_reasoning) {
		set_commit_line(&reasoning_buf, new_oid);
		if (move_record(r, reasoning, old_oid, new_oid, &reasoning_buf,
				&reasoning_blob, dry_run))
			ret = -1;
	}

out:
	provenance_record_release(&rec);
	provenance_reasoning_release(&why);
	strbuf_release(&lines_buf);
	strbuf_release(&reasoning_buf);
	strbuf_release(&err);
	return ret;
}

int provenance_remap(struct repository *r, struct provenance_remap *pairs,
		     size_t nr, int dry_run)
{
	struct records_ref refs[2];
	int ret = 0;

	records_ref_init(r, &refs[0], PROVENANCE_LINES_REF);
	records_ref_init(r, &refs[1], PROVENANCE_REASONING_REF);
	for (size_t i = 0; !ret && i < nr; i++)
		ret = remap_one(r, &refs[0], &refs[1], &pairs[i], dry_run);
	if (!ret && !dry_run)
		ret = write_refs(r, refs, ARRAY_SIZE(refs),
				 "remap: carry records over to new commits\n",
				 "remap");

	for (size_t i = 0; i < ARRAY_SIZE(refs); i++)
		free_notes(&refs[i].t);
	return ret;
}

struct prune_data {
	struct repository *r;
	struct oid_array *missing;
};

static int collect_missing(const struct object_id *commit,
			   const struct object_id *blob UNUSED,
			   char *path UNUSED, void *data)
{
	struct prune_data *pd = data;

	/* as "git notes prune" decides it */
	if (!odb_has_object(pd->r->objects, commit,
			    ODB_HAS_OBJECT_RECHECK_PACKED |
			    ODB_HAS_OBJECT_FETCH_PROMISOR))
		oid_array_append(pd->missing, commit);
	return 0;
}

int provenance_prune(struct repository *r, int dry_run,
		     struct oid_array *pruned)
{
	struct records_ref refs[2];
	int ret = 0;

	records_ref_init(r, &refs[0], PROVENANCE_LINES_REF);
	records_ref_init(r, &refs[1], PROVENANCE_REASONING_REF);
	for (size_t i = 0; i < ARRAY_SIZE(refs); i++) {
		struct oid_array missing = OID_ARRAY_INIT;
		struct prune_data pd = { .r = r, .missing = &missing };

		for_each_note(&refs[i].t, 0, collect_missing, &pd);
		for (size_t j = 0; j < missing.nr; j++) {
			remove_note(&refs[i].t, missing.oid[j].hash);
			oid_array_append(pruned, &missing.oid[j]);
		}
		refs[i].changed = missing.nr > 0;
		oid_array_clear(&missing);
	}
	if (!dry_run)
		ret = write_refs(r, refs, ARRAY_SIZE(refs),
				 "prune: remove the records of missing commits\n",
				 "prune");

	for (size_t i = 0; i < ARRAY_SIZE(refs); i++)
		free_notes(&refs[i].t);
	return ret;
}
