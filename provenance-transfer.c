/*
 * Transferring provenance refs with fetch, push and clone.
 */

#include "git-compat-util.h"
#include "provenance-transfer.h"
#include "commit.h"
#include "commit-reach.h"
#include "config.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "notes.h"
#include "notes-utils.h"
#include "odb.h"
#include "provenance.h"
#include "refs.h"
#include "refspec.h"
#include "remote.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
#include "transport.h"

/* The provenance refs, as the last component of their names. */
static const char *const kinds[] = { "lines", "reasoning" };

static const char *local_ref_of(const char *kind)
{
	return !strcmp(kind, "lines") ?
		PROVENANCE_LINES_REF : PROVENANCE_REASONING_REF;
}

static void tracking_ref_of(struct strbuf *out, const char *remote_name,
			    const char *kind)
{
	strbuf_addf(out, PROVENANCE_TRACKING_PREFIX "%s/%s", remote_name, kind);
}

/*
 * Return whether the reasoning ref is transferred according to the
 * configuration variable `key`, "provenance.reasoning.fetch" or
 * "provenance.reasoning.push".
 */
static int reasoning_wanted(struct repository *r, const char *key)
{
	const char *value;

	if (repo_config_get_string_tmp(r, key, &value) ||
	    !strcmp(value, "always"))
		return 1;
	if (!strcmp(value, "never"))
		return 0;
	if (!strcmp(value, "ondemand"))
		die(_("%s=ondemand is not supported yet"), key);
	die(_("invalid value for '%s': '%s'"), key, value);
}

/* Return whether "remote.<name>.provenance" of `remote_name` is false. */
static int opted_out(struct repository *r, const char *remote_name)
{
	char *key = xstrfmt("remote.%s.provenance", remote_name);
	int enabled = 1;

	repo_config_get_bool(r, key, &enabled);
	free(key);
	return !enabled;
}

/*
 * Return whether the provenance refs are transferred to and from
 * `remote`: provenance is enabled in `r`, `remote` is a configured
 * remote and it has not opted out.
 */
static int remote_transfers(struct repository *r, struct remote *remote)
{
	return remote_is_configured(remote, 0) && provenance_enabled(r) &&
	       !opted_out(r, remote->name);
}

/* Return whether `rs` maps the remote ref `name` to a local ref. */
static int refspec_maps(struct refspec *rs, const char *name)
{
	struct refspec_item query = { .src = (char *)name };
	int mapped;

	if (refspec_find_match(rs, &query))
		return 0;
	mapped = query.dst && *query.dst;
	free(query.dst);
	return mapped;
}

void provenance_fetch_refspecs(struct repository *r, struct remote *remote,
			       struct refspec *cmdline, struct refspec *out)
{
	if (!remote_transfers(r, remote))
		return;

	for (size_t i = 0; i < ARRAY_SIZE(kinds); i++) {
		const char *ref = local_ref_of(kinds[i]);
		struct strbuf tracking = STRBUF_INIT;

		if (!strcmp(kinds[i], "reasoning") &&
		    !reasoning_wanted(r, "provenance.reasoning.fetch"))
			continue;
		if ((cmdline && refspec_maps(cmdline, ref)) ||
		    refspec_maps(&remote->fetch, ref))
			continue;

		tracking_ref_of(&tracking, remote->name, kinds[i]);
		refspec_appendf(out, "+%s:%s", ref, tracking.buf);
		strbuf_release(&tracking);
	}
}

struct merge_state {
	struct notes_tree base, ours, theirs;
	const char *ref;
};

static void keep_ours(struct merge_state *s, const struct object_id *commit)
{
	warning(_("keeping the local provenance record of %s in '%s'; "
		  "the fetched one differs"),
		oid_to_hex(commit), s->ref);
}

/* Apply a record that "theirs" changed relative to "base" to "ours". */
static int merge_theirs(const struct object_id *commit,
			const struct object_id *theirs, char *path UNUSED,
			void *data)
{
	struct merge_state *s = data;
	const struct object_id *base = get_note(&s->base, commit);
	const struct object_id *ours = get_note(&s->ours, commit);

	if (base && oideq(base, theirs))
		return 0; /* they did not change it */
	if (ours && oideq(ours, theirs))
		return 0; /* both made the same change */
	if (!ours != !base || (ours && !oideq(ours, base))) {
		keep_ours(s, commit);
		return 0;
	}
	if (add_note(&s->ours, commit, theirs, combine_notes_overwrite))
		BUG("combine_notes_overwrite failed");
	return 0;
}

/* Apply the removal of records that "theirs" removed to "ours". */
static int merge_removed(const struct object_id *commit,
			 const struct object_id *base, char *path UNUSED,
			 void *data)
{
	struct merge_state *s = data;
	const struct object_id *ours;

	if (get_note(&s->theirs, commit))
		return 0; /* handled by merge_theirs() */
	ours = get_note(&s->ours, commit);
	if (!ours)
		return 0;
	if (!oideq(ours, base)) {
		keep_ours(s, commit);
		return 0;
	}
	remove_note(&s->ours, commit->hash);
	return 0;
}

/*
 * Write a merge commit of the notes commits `ours` (the local ref
 * `ref`) and `theirs`, whose tree is the union of both, into `result`.
 */
static int merge_notes(struct repository *r, const char *ref,
		       const char *remote_name, struct commit *ours,
		       struct commit *theirs, struct object_id *result)
{
	struct merge_state s = { .ref = ref };
	struct commit_list *bases = NULL, *parents = NULL;
	struct strbuf msg = STRBUF_INIT;

	if (repo_get_merge_bases(r, ours, theirs, &bases) < 0)
		return error(_("unable to find the merge base of '%s'"), ref);

	/*
	 * The notes trees are read from commits, not refs: their
	 * names only identify them, as nothing writes them to a ref.
	 */
	if (bases) {
		init_notes(&s.base, oid_to_hex(&bases->item->object.oid),
			   combine_notes_overwrite, 0);
	} else {
		warning(_("the fetched '%s' shares no history with the local "
			  "one; merging their records as a union"), ref);
		init_notes(&s.base, ref, combine_notes_overwrite,
			   NOTES_INIT_EMPTY);
	}
	init_notes(&s.ours, oid_to_hex(&ours->object.oid),
		   combine_notes_overwrite, 0);
	init_notes(&s.theirs, oid_to_hex(&theirs->object.oid),
		   combine_notes_overwrite, 0);

	for_each_note(&s.theirs, 0, merge_theirs, &s);
	for_each_note(&s.base, 0, merge_removed, &s);

	commit_list_insert(theirs, &parents);
	commit_list_insert(ours, &parents);
	strbuf_addf(&msg, "Merge provenance from %s\n", remote_name);
	create_notes_commit(r, &s.ours, parents, msg.buf, msg.len, result);

	strbuf_release(&msg);
	commit_list_free(parents);
	commit_list_free(bases);
	free_notes(&s.base);
	free_notes(&s.ours);
	free_notes(&s.theirs);
	return 0;
}

static struct commit *notes_commit(struct repository *r,
				   const struct object_id *oid,
				   const char *ref)
{
	struct commit *commit = lookup_commit_reference(r, oid);

	if (!commit || repo_parse_commit(r, commit)) {
		error(_("'%s' does not point to a commit"), ref);
		return NULL;
	}
	return commit;
}

/* Merge the tracking ref `tracking` into the local ref `ref`. */
static int merge_one(struct repository *r, const char *remote_name,
		     const char *ref, const char *tracking, int verbose)
{
	struct ref_store *refs = get_main_ref_store(r);
	struct object_id theirs_oid, ours_oid, result;
	struct commit *ours, *theirs;
	struct ref_transaction *transaction;
	struct strbuf err = STRBUF_INIT, msg = STRBUF_INIT;
	const char *what;
	int ret;

	if (refs_read_ref(refs, tracking, &theirs_oid))
		return 0;

	if (refs_read_ref(refs, ref, &ours_oid)) {
		oidclr(&ours_oid, r->hash_algo);
		oidcpy(&result, &theirs_oid);
		what = _("created '%s' from '%s'\n");
	} else {
		if (oideq(&ours_oid, &theirs_oid))
			return 0;
		if (!(ours = notes_commit(r, &ours_oid, ref)) ||
		    !(theirs = notes_commit(r, &theirs_oid, tracking)))
			return -1;

		ret = repo_in_merge_bases(r, theirs, ours);
		if (ret < 0)
			return -1;
		if (ret)
			return 0; /* already merged */
		ret = repo_in_merge_bases(r, ours, theirs);
		if (ret < 0)
			return -1;
		if (ret) {
			oidcpy(&result, &theirs_oid);
			what = _("fast-forwarded '%s' to '%s'\n");
		} else {
			if (merge_notes(r, ref, remote_name, ours, theirs,
					&result))
				return -1;
			what = _("merged '%s' with '%s'\n");
		}
	}

	strbuf_addf(&msg, "fetch: merge provenance from %s", remote_name);
	transaction = ref_store_transaction_begin(refs, 0, &err);
	if (!transaction ||
	    ref_transaction_update(transaction, ref, &result, &ours_oid,
				   NULL, NULL, 0, msg.buf, &err) ||
	    ref_transaction_commit(transaction, &err)) {
		ret = error("%s", err.buf);
	} else {
		ret = 0;
		if (verbose > 0)
			fprintf(stderr, what, ref, tracking);
	}

	ref_transaction_free(transaction);
	strbuf_release(&err);
	strbuf_release(&msg);
	return ret;
}

/* Return whether the fetch described by `ref_map` updated `refname`. */
static int fetch_updated(const struct ref *ref_map, const char *refname)
{
	for (const struct ref *rm = ref_map; rm; rm = rm->next)
		if (rm->peer_ref && !strcmp(rm->peer_ref->name, refname) &&
		    !oideq(&rm->old_oid, &rm->peer_ref->old_oid))
			return 1;
	return 0;
}

int provenance_merge_fetched(struct repository *r, const char *remote_name,
			     const struct ref *ref_map, int verbose)
{
	int ret = 0;

	if (opted_out(r, remote_name))
		return 0;

	for (size_t i = 0; i < ARRAY_SIZE(kinds); i++) {
		struct strbuf tracking = STRBUF_INIT;

		tracking_ref_of(&tracking, remote_name, kinds[i]);
		if (fetch_updated(ref_map, tracking.buf) &&
		    merge_one(r, remote_name, local_ref_of(kinds[i]),
			      tracking.buf, verbose))
			ret = -1;
		strbuf_release(&tracking);
	}
	return ret;
}

/* Return whether the local ref `name`, as a push names it, is a branch. */
static int names_branch(struct repository *r, const char *name)
{
	struct object_id oid;
	char *full = NULL;
	int ret;

	if (!strcmp(name, "HEAD")) {
		const char *head = refs_resolve_ref_unsafe(get_main_ref_store(r),
							   "HEAD", 0, NULL,
							   NULL);
		return head && starts_with(head, "refs/heads/");
	}
	ret = repo_dwim_ref(r, name, strlen(name), &oid, &full, 0) == 1 &&
	      starts_with(full, "refs/heads/");
	free(full);
	return ret;
}

/*
 * Return whether the refspec item `item` of a push updates a branch
 * of the remote, a ref under refs/heads/: it is the "matching"
 * refspec, or its destination is a branch.  A destination that is
 * not a full ref name, or a missing one, is a branch if the source is
 * a local branch, as the push resolves it when the remote does not
 * have a ref of that name.
 */
static int pushes_branch(struct repository *r, const struct refspec_item *item)
{
	const char *dst;

	if (item->negative)
		return 0;
	if (item->matching)
		return 1;
	if (!item->src || !*item->src)
		return 0; /* a deletion */
	dst = item->dst && *item->dst ? item->dst : item->src;
	if (starts_with(dst, "refs/"))
		return starts_with(dst, "refs/heads/");
	if (item->pattern || item->exact_sha1)
		return 0;
	return names_branch(r, item->src);
}

/* Return whether `rs` names the ref `name` on either side. */
static int refspec_names(const struct refspec *rs, const char *name)
{
	for (int i = 0; i < rs->nr; i++) {
		const struct refspec_item *item = &rs->items[i];

		if ((item->src && !strcmp(item->src, name)) ||
		    (item->dst && !strcmp(item->dst, name)))
			return 1;
	}
	return 0;
}

void provenance_push_refspecs(struct repository *r, struct remote *remote,
			      const struct refspec *rs, int flags,
			      struct refspec *out)
{
	int branch = 0;

	if (flags & (TRANSPORT_PUSH_MIRROR | TRANSPORT_PUSH_PRUNE) ||
	    !remote_transfers(r, remote))
		return;

	if (flags & TRANSPORT_PUSH_ALL)
		branch = 1;
	for (int i = 0; !branch && i < rs->nr; i++)
		branch = pushes_branch(r, &rs->items[i]);
	if (!branch)
		return;

	for (size_t i = 0; i < ARRAY_SIZE(kinds); i++) {
		const char *ref = local_ref_of(kinds[i]);

		if (!strcmp(kinds[i], "reasoning") &&
		    !reasoning_wanted(r, "provenance.reasoning.push"))
			continue;
		if (refspec_names(rs, ref) ||
		    !refs_ref_exists(get_main_ref_store(r), ref))
			continue;
		refspec_appendf(out, "%s:%s", ref, ref);
	}
}

static int is_provenance_ref(const char *name)
{
	return !strcmp(name, PROVENANCE_LINES_REF) ||
	       !strcmp(name, PROVENANCE_REASONING_REF);
}

static int remote_moved(const struct ref *ref)
{
	return ref->status == REF_STATUS_REJECT_NONFASTFORWARD ||
	       ref->status == REF_STATUS_REJECT_FETCH_FIRST;
}

int provenance_push_rejected(const struct ref *refs, int *other_errors)
{
	int rejected = 0, others = 0;

	for (const struct ref *ref = refs; ref; ref = ref->next) {
		switch (ref->status) {
		case REF_STATUS_NONE:
		case REF_STATUS_OK:
		case REF_STATUS_UPTODATE:
		case REF_STATUS_ATOMIC_PUSH_FAILED:
			break;
		default:
			if (is_provenance_ref(ref->name) && remote_moved(ref))
				rejected = 1;
			else
				others = 1;
		}
	}
	if (other_errors)
		*other_errors = others;
	return rejected;
}

int provenance_fetch_rejected(struct repository *r, const char *remote_name,
			      const struct ref *refs)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	int ret;

	cmd.git_cmd = 1;
	strvec_pushl(&cmd.args, "fetch", "--quiet", "--no-tags",
		     "--no-write-fetch-head", "--", remote_name, NULL);
	for (const struct ref *ref = refs; ref; ref = ref->next) {
		const char *kind;

		if (!is_provenance_ref(ref->name) || !remote_moved(ref) ||
		    !skip_prefix(ref->name, "refs/provenance/", &kind))
			continue;
		strvec_pushf(&cmd.args,
			     "+%s:" PROVENANCE_TRACKING_PREFIX "%s/%s",
			     ref->name, remote_name, kind);
	}
	ret = run_command(&cmd);

	/*
	 * The fetch updated refs and added objects behind our back:
	 * drop the cached view of both, as a ref storage migration
	 * does, so that the caller sees the merged provenance refs.
	 */
	if (r->refs_private) {
		ref_store_release(r->refs_private);
		FREE_AND_NULL(r->refs_private);
	}
	odb_reprepare(r->objects);
	return ret;
}
