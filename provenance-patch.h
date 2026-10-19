#ifndef PROVENANCE_PATCH_H
#define PROVENANCE_PATCH_H

struct commit;
struct diff_queue_struct;
struct object_id;
struct provenance_record;
struct repository;
struct strbuf;

/*
 * The line record of a commit in a patch mail, as "git format-patch"
 * writes it after the "---" separator and "git am" reads it.  See
 * "Line record in a patch mail" in
 * Documentation/technical/git-provenance-format.adoc.
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

/*
 * Look for a "Provenance:" block in the `len` bytes at `buf`, the
 * patch part of a mail, before the first line that starts a patch, and
 * parse the record it holds into `rec`, which must have been
 * initialized with PROVENANCE_RECORD_INIT.  Returns 1 if a block was
 * found and parsed, and 0 if there is none.  Returns -1 if the block
 * does not parse, with a one-line reason appended to `err`.  `rec` must
 * be released with provenance_record_release() in every case.
 */
int provenance_patch_parse(struct repository *r, const char *buf, size_t len,
			   struct provenance_record *rec, struct strbuf *err);

/*
 * Turn `rec`, a line record that a patch mail carried, into one that
 * describes `commit`, which "git am" made from the mail, by locating
 * its ranges by content: a `+` range among the runs of lines that the
 * commit adds to the path, against its first parent, and a `-` range
 * among the runs of lines it deletes, as the window of as many lines
 * whose anchor is that of the range, the closest to the line numbers of
 * the range where several match, and not overlapping a range located
 * before.  The ranges that are not found are dropped; if a path had
 * one, the lines that the commit adds to it or deletes from it and that
 * no range covers get `UNKNOWN` ranges.  Blocks left without ranges are
 * dropped too.  The `commit` line of `rec` is left alone.  Returns 0 on
 * success, or -1 after reporting an error.
 */
int provenance_patch_locate(struct repository *r, struct commit *commit,
			    struct provenance_record *rec);

#endif /* PROVENANCE_PATCH_H */
