#ifndef PROVENANCE_PURGE_H
#define PROVENANCE_PURGE_H

#include "hash.h"

struct oid_array;
struct repository;
struct string_list;

/*
 * Rewriting refs/provenance/reasoning to remove what its records must
 * not hold.  See the "Privacy" section of
 * Documentation/technical/git-provenance.adoc.
 */

/* The marker that replaces each match of a redacted pattern. */
#define PROVENANCE_PURGE_MARKER "***REMOVED***"

struct provenance_purge_opts {
	/* drop the sessions with these ids, if not NULL */
	struct string_list *sessions;
	/* replace the matches of this pattern in the summaries, if not NULL */
	const regex_t *pattern;
	/* compute what would be purged, without writing it */
	int dry_run;
};

enum provenance_purge_result {
	/* the record was removed */
	PROVENANCE_PURGE_REMOVED,
	/* matches of the pattern were replaced in the summary */
	PROVENANCE_PURGE_REDACTED,
	/* sessions and the summary were dropped from the record */
	PROVENANCE_PURGE_SESSION_DROPPED,
};

struct provenance_purged {
	/* the commit that the record is attached to */
	struct object_id commit;
	enum provenance_purge_result result;
	/* PROVENANCE_PURGE_REDACTED: the number of matches replaced */
	size_t matches;
};

/*
 * Purge the reasoning records of the `commits` in `r`, or of every
 * commit that has one if `commits` is NULL, as `opts` says: with
 * neither sessions nor a pattern, remove them; with sessions, drop the
 * `session` lines of those ids and the summary from each record that
 * names one, and remove a record left without a session; with a
 * pattern, replace each of its matches in the summaries with
 * PROVENANCE_PURGE_MARKER.  A record that is not changed is kept as it
 * is, as is one that cannot be parsed, with a warning.
 *
 * Unless `opts->dry_run` is set, the records that are left are written
 * as one notes commit without a parent, so that the old records are
 * no longer reachable from the ref, and the ref is updated to it with
 * the reflog message "purge".  Nothing is written if no record is
 * changed.
 *
 * Set `*out` to an array of `*out_nr` entries, one per record that is
 * changed, for the caller to free.  Returns 0 on success and -1, after
 * reporting an error, if the records cannot be read or written.
 */
int provenance_purge(struct repository *r,
		     const struct provenance_purge_opts *opts,
		     const struct oid_array *commits,
		     struct provenance_purged **out, size_t *out_nr);

#endif /* PROVENANCE_PURGE_H */
