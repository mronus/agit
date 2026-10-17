#ifndef PROVENANCE_ATTRIBUTE_H
#define PROVENANCE_ATTRIBUTE_H

struct commit;
struct object_id;
struct provenance_journal;
struct provenance_reasoning;
struct provenance_record;
struct repository;

/*
 * Attribute the lines changed by a commit whose tree is `tree` and
 * whose first parent is `parent` (NULL for a root commit), using the
 * stamps in `journal`.  See the "Attribution" section of
 * Documentation/technical/git-provenance.adoc.
 *
 * `session` is the session named with "git commit --session", or NULL.
 * `human` identifies the human who made the commit, as
 * "Name <email>".
 *
 * On success the line record is stored in `lines` and the reasoning
 * record in `reasoning`, which must have been initialized with
 * PROVENANCE_RECORD_INIT and PROVENANCE_REASONING_INIT; their commit
 * ids are left for the caller to fill in.  The reasoning record names
 * no session if no session is attached to the commit.  Stamps whose
 * objects are missing are skipped with a warning.  Returns 0 on
 * success, or -1 after reporting an error.
 */
int provenance_attribute(struct repository *r,
			 struct commit *parent,
			 const struct object_id *tree,
			 const struct provenance_journal *journal,
			 const char *session,
			 const char *human,
			 struct provenance_record *lines,
			 struct provenance_reasoning *reasoning);

#endif /* PROVENANCE_ATTRIBUTE_H */
