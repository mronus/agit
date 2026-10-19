#ifndef PROVENANCE_PATCH_H
#define PROVENANCE_PATCH_H

struct diff_queue_struct;
struct object_id;
struct repository;
struct strbuf;

/*
 * The line record of a commit in a patch mail, as "git format-patch"
 * writes it after the "---" separator.  See "Line record in a patch
 * mail" in Documentation/technical/git-provenance-format.adoc.
 */

/*
 * Append the "Provenance:" block of the line record of `commit` to
 * `out`, with the `file` blocks of the paths that the patch in `q`
 * shows.  Returns 1 if it was appended, and 0 if the commit has no line
 * record or the record has no `file` block for these paths.  A record
 * that cannot be read or parsed, or that names another commit, counts
 * as none, with a warning.
 */
int provenance_patch_format(struct repository *r,
			    const struct object_id *commit,
			    const struct diff_queue_struct *q,
			    struct strbuf *out);

#endif /* PROVENANCE_PATCH_H */
