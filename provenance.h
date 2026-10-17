#ifndef PROVENANCE_H
#define PROVENANCE_H

#include "hash.h"

struct commit;
struct repository;
struct strbuf;

/*
 * Provenance records describe who wrote the lines of a commit (line
 * records) and why the commit was made (reasoning records).  They are
 * stored as notes under the refs below, keyed by the object id of the
 * commit they describe.  See Documentation/technical/
 * git-provenance-format.adoc for the format.
 */
#define PROVENANCE_LINES_REF "refs/provenance/lines"
#define PROVENANCE_REASONING_REF "refs/provenance/reasoning"

enum provenance_capture {
	PROVENANCE_CAPTURE_OBSERVED,
	PROVENANCE_CAPTURE_DECLARED,
	PROVENANCE_CAPTURE_ADOPTED,
};

enum provenance_actor_kind {
	PROVENANCE_ACTOR_AI,
	PROVENANCE_ACTOR_HUMAN,
};

enum provenance_mark {
	PROVENANCE_MARK_AI,
	PROVENANCE_MARK_MIXED,
	PROVENANCE_MARK_HUMAN,
	PROVENANCE_MARK_UNKNOWN,
};

struct provenance_actor {
	char *id;
	enum provenance_actor_kind kind;
	/* PROVENANCE_ACTOR_AI */
	char *model;
	char *tool;
	char *session;
	char *source;
	/* PROVENANCE_ACTOR_HUMAN */
	char *ident;
};

struct provenance_range {
	/* '+' for post-image ranges, '-' for pre-image ranges */
	char side;
	/* 1-based, inclusive */
	unsigned long begin;
	unsigned long end;
	enum provenance_mark mark;
	/* comma-separated actor ids, or "-" */
	char *actors;
	struct object_id anchor;
};

struct provenance_file {
	char *path;
	struct provenance_range *ranges;
	size_t ranges_nr, ranges_alloc;
};

/* A parsed line record. */
struct provenance_record {
	int version;
	struct object_id commit;
	enum provenance_capture capture;
	/* "<tool>/<version>" for PROVENANCE_CAPTURE_OBSERVED, else NULL */
	char *capture_tool;
	struct provenance_actor *actors;
	size_t actors_nr, actors_alloc;
	struct provenance_file *files;
	size_t files_nr, files_alloc;
};

#define PROVENANCE_RECORD_INIT { 0 }

/*
 * Parse the line record in `buf` into `rec`, which must have been
 * initialized with PROVENANCE_RECORD_INIT.  Object ids are parsed
 * with the hash algorithm `algop`.  Returns 0 on success; on failure
 * returns -1 and appends a one-line reason to `err`.  `rec` must be
 * released with provenance_record_release() in either case.
 */
int provenance_record_parse(const char *buf, size_t len,
			    const struct git_hash_algo *algop,
			    struct provenance_record *rec,
			    struct strbuf *err);

/*
 * Append the serialized form of `rec` to `out`.  The output parses
 * back to an identical record.
 */
void provenance_record_format(const struct provenance_record *rec,
			      struct strbuf *out);

void provenance_record_release(struct provenance_record *rec);

/*
 * Read the line record (or reasoning record) attached to `commit` in
 * repository `r` and append its raw content to `out`.  Returns 1 if a
 * record was found, and 0 if there is none, including when the ref
 * does not exist.  Returns -1, after reporting an error, if the ref
 * cannot be read or does not point to a tree, or if the record is not
 * a readable blob.  The lookup follows the notes fan-out directly and
 * does not touch any global notes state.
 */
int provenance_read_lines(struct repository *r,
			  const struct object_id *commit,
			  struct strbuf *out);
int provenance_read_reasoning(struct repository *r,
			      const struct object_id *commit,
			      struct strbuf *out);

/*
 * Check that the line record `rec` is consistent with `commit`: it
 * names the commit, uses only declared actors and valid marks, and
 * its ranges lie within the files they name and match their anchors.
 * Returns 0 if it is; otherwise returns -1 and appends a one-line
 * description of the first failure to `err`.
 */
int provenance_verify(struct repository *r, struct commit *commit,
		      const struct provenance_record *rec,
		      struct strbuf *err);

#endif /* PROVENANCE_H */
