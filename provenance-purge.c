/*
 * Purging reasoning records.
 */

#include "git-compat-util.h"
#include "provenance-purge.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "ident.h"
#include "notes.h"
#include "odb.h"
#include "oid-array.h"
#include "provenance.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"
#include "string-list.h"

/*
 * Replace each match of `pattern` in `buf` with the marker, and return
 * the number of matches.  `buf` is left alone if there is none.  An
 * empty match replaces nothing.
 */
static size_t redact(struct strbuf *buf, const regex_t *pattern)
{
	struct strbuf out = STRBUF_INIT;
	size_t pos = 0, matches = 0;

	while (pos < buf->len) {
		regmatch_t m;
		int flags = pos && buf->buf[pos - 1] != '\n' ? REG_NOTBOL : 0;

		if (regexec_buf(pattern, buf->buf + pos, buf->len - pos,
				1, &m, flags))
			break;
		if (m.rm_so == m.rm_eo) {
			/* step over the character after an empty match */
			strbuf_add(&out, buf->buf + pos, m.rm_so + 1);
			pos += m.rm_so + 1;
			continue;
		}
		strbuf_add(&out, buf->buf + pos, m.rm_so);
		strbuf_addstr(&out, PROVENANCE_PURGE_MARKER);
		pos += m.rm_eo;
		matches++;
	}

	if (matches) {
		strbuf_add(&out, buf->buf + pos, buf->len - pos);
		strbuf_swap(buf, &out);
	}
	strbuf_release(&out);
	return matches;
}

/*
 * Drop the sessions of `rec` whose ids are in `ids`, and its summary
 * if any was dropped.  Return whether any was.
 */
static int drop_sessions(struct provenance_reasoning *rec,
			 struct string_list *ids)
{
	size_t kept = 0;

	for (size_t i = 0; i < rec->sessions_nr; i++) {
		struct provenance_session *s = &rec->sessions[i];

		if (!unsorted_string_list_has_string(ids, s->id)) {
			rec->sessions[kept++] = *s;
			continue;
		}
		free(s->id);
		free(s->model);
		free(s->tool);
	}
	if (kept == rec->sessions_nr)
		return 0;
	rec->sessions_nr = kept;
	strbuf_reset(&rec->summary);
	return 1;
}

/*
 * Purge the record `blob` of `commit` from `t` as `opts` says.  Return
 * 1 and fill `purged` if it is changed, 0 if it is kept, and -1 if it
 * cannot be read.
 */
static int purge_one(struct repository *r, struct notes_tree *t,
		     const struct provenance_purge_opts *opts,
		     const struct object_id *commit,
		     const struct object_id *blob,
		     struct provenance_purged *purged)
{
	struct provenance_reasoning rec = PROVENANCE_REASONING_INIT;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	struct object_id oid;
	enum object_type type;
	size_t size;
	void *data;
	int ret = 0;

	oidcpy(&purged->commit, commit);
	purged->matches = 0;
	if (!opts->sessions && !opts->pattern) {
		purged->result = PROVENANCE_PURGE_REMOVED;
		remove_note(t, commit->hash);
		return 1;
	}

	data = odb_read_object(r->objects, blob, &type, &size);
	if (!data || type != OBJ_BLOB) {
		free(data);
		return error(_("unable to read provenance record '%s'"),
			     oid_to_hex(blob));
	}
	strbuf_attach(&buf, data, size, size + 1);

	if (provenance_reasoning_parse(buf.buf, buf.len, r->hash_algo,
				       &rec, &err)) {
		warning(_("keeping the reasoning record of %s: %s"),
			oid_to_hex(commit), err.buf);
		goto out;
	}

	if (opts->sessions) {
		if (!drop_sessions(&rec, opts->sessions))
			goto out;
		if (!rec.sessions_nr) {
			purged->result = PROVENANCE_PURGE_REMOVED;
			remove_note(t, commit->hash);
			ret = 1;
			goto out;
		}
		purged->result = PROVENANCE_PURGE_SESSION_DROPPED;
		strbuf_reset(&buf);
		provenance_reasoning_format(&rec, &buf);
	} else {
		/* the summary follows the first blank line */
		const char *blank = strstr(buf.buf, "\n\n");
		struct strbuf summary = STRBUF_INIT;
		size_t start = blank ? blank - buf.buf + 2 : buf.len;

		strbuf_add(&summary, buf.buf + start, buf.len - start);
		purged->matches = redact(&summary, opts->pattern);
		if (purged->matches) {
			strbuf_setlen(&buf, start);
			strbuf_addbuf(&buf, &summary);
		}
		strbuf_release(&summary);
		if (!purged->matches)
			goto out;
		purged->result = PROVENANCE_PURGE_REDACTED;
	}

	if (opts->dry_run) {
		ret = 1;
		goto out;
	}
	if (odb_write_object(r->objects, buf.buf, buf.len, OBJ_BLOB, &oid)) {
		ret = error(_("unable to write the record for '%s'"),
			    oid_to_hex(commit));
		goto out;
	}
	if (add_note(t, commit, &oid, combine_notes_overwrite))
		BUG("combine_notes_overwrite failed");
	ret = 1;

out:
	provenance_reasoning_release(&rec);
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

static int collect(const struct object_id *commit,
		   const struct object_id *blob UNUSED,
		   char *path UNUSED, void *data)
{
	oid_array_append(data, commit);
	return 0;
}

/*
 * The message of the notes commit, which names neither the pattern nor
 * anything it matched.
 */
static void purge_message(const struct provenance_purge_opts *opts,
			  struct strbuf *msg)
{
	strbuf_addstr(msg, "purge: ");
	if (opts->sessions) {
		strbuf_addstr(msg, "drop session");
		for (size_t i = 0; i < opts->sessions->nr; i++)
			strbuf_addf(msg, "%s %s", i ? "," : "",
				    opts->sessions->items[i].string);
	} else if (opts->pattern) {
		strbuf_addstr(msg, "redact a pattern");
	} else {
		strbuf_addstr(msg, "remove records");
	}
	strbuf_addch(msg, '\n');
}

int provenance_purge(struct repository *r,
		     const struct provenance_purge_opts *opts,
		     const struct oid_array *commits,
		     struct provenance_purged **out, size_t *out_nr)
{
	struct ref_store *refs = get_main_ref_store(r);
	struct notes_tree t = { 0 };
	struct oid_array all = OID_ARRAY_INIT;
	struct object_id old_oid, tree_oid, new_oid;
	struct strbuf msg = STRBUF_INIT;
	size_t alloc = 0;
	int ret = 0;

	*out = NULL;
	*out_nr = 0;
	if (refs_read_ref(refs, PROVENANCE_REASONING_REF, &old_oid))
		return 0;

	/*
	 * Read the notes tree from the commit that is replaced, not from
	 * the ref, whose name would only identify it.
	 */
	init_notes(&t, oid_to_hex(&old_oid), combine_notes_overwrite, 0);
	if (!commits) {
		for_each_note(&t, 0, collect, &all);
		commits = &all;
	}

	for (size_t i = 0; i < commits->nr; i++) {
		const struct object_id *blob = get_note(&t, &commits->oid[i]);
		struct provenance_purged purged;
		int changed;

		if (!blob)
			continue;
		changed = purge_one(r, &t, opts, &commits->oid[i], blob,
				    &purged);
		if (changed < 0) {
			ret = -1;
			goto out;
		}
		if (changed) {
			ALLOC_GROW(*out, *out_nr + 1, alloc);
			(*out)[(*out_nr)++] = purged;
		}
	}
	if (!*out_nr || opts->dry_run)
		goto out;

	purge_message(opts, &msg);
	if (write_notes_tree(&t, &tree_oid)) {
		ret = error(_("unable to write the notes tree for '%s'"),
			    PROVENANCE_REASONING_REF);
	} else if (commit_tree(msg.buf, msg.len, &tree_oid, NULL, &new_oid,
			       git_committer_info(IDENT_STRICT), NULL)) {
		ret = error(_("unable to write the notes commit for '%s'"),
			    PROVENANCE_REASONING_REF);
	} else if (refs_update_ref(refs, "purge", PROVENANCE_REASONING_REF,
				   &new_oid, &old_oid, 0,
				   UPDATE_REFS_MSG_ON_ERR)) {
		ret = -1;
	}

out:
	free_notes(&t);
	oid_array_clear(&all);
	strbuf_release(&msg);
	return ret;
}
