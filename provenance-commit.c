/*
 * The provenance records of the commits that git makes.
 */

#include "git-compat-util.h"
#include "provenance-commit.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "ident.h"
#include "notes.h"
#include "object-name.h"
#include "odb.h"
#include "provenance.h"
#include "provenance-attribute.h"
#include "provenance-journal.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"

/*
 * Read the records of `commit` into `lines` and `reasoning`, and set
 * `has_reasoning` if it has a reasoning record.  Returns 1 if it has a
 * line record, 0 if it has none or one that cannot be parsed, and -1
 * if they cannot be read.
 */
static int read_source(struct repository *r, struct commit *commit,
		       struct provenance_record *lines,
		       struct provenance_reasoning *reasoning,
		       int *has_reasoning)
{
	const struct object_id *oid = &commit->object.oid;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret;

	*has_reasoning = 0;
	ret = provenance_read_lines(r, oid, &buf);
	if (ret > 0 &&
	    provenance_record_parse(buf.buf, buf.len, r->hash_algo, lines, &err)) {
		warning(_("ignoring the line record of %s: %s"),
			oid_to_hex(oid), err.buf);
		ret = 0;
	}

	strbuf_reset(&buf);
	strbuf_reset(&err);
	if (ret > 0) {
		switch (provenance_read_reasoning(r, oid, &buf)) {
		case -1:
			ret = -1;
			break;
		case 1:
			if (provenance_reasoning_parse(buf.buf, buf.len,
						       r->hash_algo,
						       reasoning, &err))
				warning(_("ignoring the reasoning record of %s: %s"),
					oid_to_hex(oid), err.buf);
			else
				*has_reasoning = 1;
			break;
		}
	}

	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

int provenance_commit_records(struct repository *r,
			      struct commit *parent,
			      const struct object_id *tree,
			      const char *author,
			      const char *session,
			      const struct provenance_rewrite *rw,
			      struct provenance_record *lines,
			      struct provenance_reasoning *reasoning)
{
	struct commit *commits[2];
	struct provenance_record records[2] = {
		PROVENANCE_RECORD_INIT, PROVENANCE_RECORD_INIT,
	};
	struct provenance_reasoning reasonings[2] = {
		PROVENANCE_REASONING_INIT, PROVENANCE_REASONING_INIT,
	};
	struct provenance_source sources[2];
	struct provenance_journal journal = PROVENANCE_JOURNAL_INIT;
	struct ident_split ident;
	size_t nr = 0;
	unsigned flags = 0;
	char *human = NULL;
	int ret = 0;

	if (rw && rw->amend)
		commits[nr++] = rw->amend;
	if (rw && rw->pick)
		commits[nr++] = rw->pick;
	if (rw && rw->revert && (rw->amend || !rw->pick))
		BUG("provenance: a revert must have a pick and no amend");

	/* the changes of a commit without a line record are UNKNOWN */
	for (size_t i = 0; i < nr; i++) {
		int has_reasoning;
		int has_lines = read_source(r, commits[i], &records[i],
					    &reasonings[i], &has_reasoning);

		if (has_lines < 0) {
			ret = -1;
			goto out;
		}
		sources[i].commit = commits[i];
		sources[i].lines = has_lines ? &records[i] : NULL;
		sources[i].reasoning = has_reasoning ? &reasonings[i] : NULL;
	}

	if (split_ident_line(&ident, author, strlen(author))) {
		ret = error(_("malformed author ident '%s'"), author);
		goto out;
	}
	human = xstrfmt("%.*s <%.*s>",
			(int)(ident.name_end - ident.name_begin), ident.name_begin,
			(int)(ident.mail_end - ident.mail_begin), ident.mail_begin);

	if (rw && rw->amend)
		flags |= PROVENANCE_REWRITE_AMEND;
	if (rw && rw->revert)
		flags |= PROVENANCE_REWRITE_REVERT;
	if (provenance_journal_read(r, &journal) ||
	    provenance_attribute_rewrite(r, parent, tree, sources, nr,
					 flags, &journal, session,
					 human, lines, reasoning))
		ret = -1;

out:
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		provenance_record_release(&records[i]);
		provenance_reasoning_release(&reasonings[i]);
	}
	provenance_journal_release(&journal);
	free(human);
	return ret;
}

/*
 * Write `record` as the note for `commit` under the notes ref `ref`,
 * and queue the update of `ref` into `transaction`.
 *
 * The notes commit is authored by the committer ident, not by the
 * environment's author: the records are written by whoever makes the
 * commit, and a rebase or cherry-pick that keeps the original author
 * may leave GIT_AUTHOR_* unset or unparsable.
 */
static int queue_note(struct repository *r,
		      struct ref_transaction *transaction,
		      const char *ref, const struct object_id *commit,
		      const struct strbuf *record, const char *reflog_msg,
		      struct strbuf *err)
{
	struct notes_tree t = { 0 };
	struct object_id old_oid, blob_oid, tree_oid, notes_oid;
	struct commit_list *parents = NULL;
	struct strbuf msg = STRBUF_INIT;
	int ret;

	if (refs_read_ref(get_main_ref_store(r), ref, &old_oid))
		oidcpy(&old_oid, null_oid(r->hash_algo));
	if (odb_write_object(r->objects, record->buf, record->len,
			     OBJ_BLOB, &blob_oid)) {
		strbuf_addf(err, _("unable to write the record for '%s'"), ref);
		return -1;
	}

	/*
	 * init_notes() also makes `ref` the notes namespace for ref
	 * decorations, which none of the commands that make commits
	 * load.
	 */
	init_notes(&t, ref, combine_notes_overwrite, NOTES_INIT_WRITABLE);
	add_note(&t, commit, &blob_oid, NULL);
	ret = write_notes_tree(&t, &tree_oid);
	free_notes(&t);
	if (ret) {
		strbuf_addf(err, _("unable to write the notes tree for '%s'"),
			    ref);
		return -1;
	}

	if (!is_null_oid(&old_oid)) {
		struct commit *parent = lookup_commit(r, &old_oid);

		if (!parent || repo_parse_commit(r, parent)) {
			strbuf_addf(err, _("unable to parse '%s'"), ref);
			return -1;
		}
		commit_list_insert(parent, &parents);
	}

	strbuf_addf(&msg, "Provenance for %s\n", oid_to_hex(commit));
	if (commit_tree(msg.buf, msg.len, &tree_oid, parents, &notes_oid,
			git_committer_info(IDENT_STRICT), NULL)) {
		strbuf_addf(err, _("unable to write the notes commit for '%s'"),
			    ref);
		ret = -1;
	} else {
		ret = ref_transaction_update(transaction, ref, &notes_oid,
					     &old_oid, NULL, NULL, 0,
					     reflog_msg, err);
	}
	commit_list_free(parents);
	strbuf_release(&msg);
	return ret;
}

int provenance_queue_records(struct repository *r,
			     struct ref_transaction *transaction,
			     const struct object_id *commit,
			     struct provenance_record *lines,
			     struct provenance_reasoning *reasoning,
			     const char *reflog_msg,
			     struct strbuf *err)
{
	struct strbuf buf = STRBUF_INIT, msg = STRBUF_INIT;
	int ret;

	strbuf_addf(&msg, "%s: provenance for %s", reflog_msg,
		    repo_find_unique_abbrev(r, commit, -1));

	oidcpy(&lines->commit, commit);
	provenance_record_format(lines, &buf);
	ret = queue_note(r, transaction, PROVENANCE_LINES_REF, commit,
			 &buf, msg.buf, err);

	if (!ret && reasoning->sessions_nr) {
		strbuf_reset(&buf);
		oidcpy(&reasoning->commit, commit);
		provenance_reasoning_format(reasoning, &buf);
		ret = queue_note(r, transaction, PROVENANCE_REASONING_REF,
				 commit, &buf, msg.buf, err);
	}

	strbuf_release(&buf);
	strbuf_release(&msg);
	return ret;
}
