/*
 * The attribution walk: which actor wrote each line that a commit changes.
 */

#include "git-compat-util.h"
#include "provenance-attribute.h"
#include "blame.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "oidset.h"
#include "provenance.h"
#include "provenance-journal.h"
#include "provenance-patch.h"
#include "quote.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "string-list.h"
#include "strmap.h"
#include "tree-walk.h"
#include "version.h"
#include "xdiff-interface.h"

/*
 * The owner of a line that is still as it was in the first parent;
 * such lines are not part of the record.
 */
#define BASELINE -1

/*
 * A set of actors, sorted by actor index.  Actors 0 to sessions_nr - 1
 * are the sessions of the journal, in the order in which they appear
 * in it, followed by the AI actors that only the line records of the
 * sources of a rewrite declare, actor sessions_nr is the human making
 * the commit, actor sessions_nr + 1 stands for an unknown origin, such
 * as that of the lines a rewrite takes from a commit without a line
 * record, and actor sessions_nr + 2 for the other parents of a merge,
 * which are not actors: they never appear in the record.  Sets are
 * interned, so that two lines have the same owners exactly if they
 * have the same set index.
 */
struct owner_set {
	int *actors;
	size_t nr;
};

struct line_owner {
	/* index into the interned sets, or BASELINE */
	int set;
	/* BASELINE: the 1-based line number in the first parent */
	unsigned long base;
};

struct session_info {
	const char *id;
	char *actor_id;
	const char *model;
	const char *tool;
	timestamp_t started;
	timestamp_t ended;
	/* summary blob of the most recent "reason" entry, or NULL */
	const struct object_id *summary;
	/* paths claimed by "claim" entries, sorted */
	struct string_list claims;
	/* path -> the hunks claimed by "claim --patch" (struct hunks) */
	struct strmap hunks;
	/* the session has "end" or "edit" entries */
	unsigned stamped : 1;
	/* every human line is the session's ("--session" without stamps) */
	unsigned claim_all : 1;
	/* the session owns a line of the commit */
	unsigned attached : 1;
	/* the session is only known from the line record of a source */
	unsigned imported : 1;
	/* the "source" of the actor in the line record of a source, or NULL */
	const char *source;
};

/*
 * The runs of lines that the hunks of the patches a session claims add
 * to a path (`+` ranges, numbered as in the post-image of their hunk)
 * or remove from it (`-` ranges, as in the pre-image), with their
 * anchors.  They are located in the changes of the commit when its
 * lines are attributed.
 */
struct hunks {
	struct provenance_range *ranges;
	/* per range: the journal entry of its patch */
	size_t *entry;
	size_t nr, alloc, entry_alloc;
};

/* A path changed by an interval. */
struct change {
	struct object_id pre;
	struct object_id post;
};

/* The changes that one "end" or "edit" entry attributes to a session. */
struct interval {
	size_t entry;
	/* the "begin" entry it starts at, or `entry` */
	size_t begin;
	int session;
	/* path -> struct change */
	struct strmap changes;
	/* taken on another commit: used by content (see apply_foreign()) */
	unsigned foreign : 1;
};

/* A source of a rewrite, or another parent of a merge. */
struct seed {
	/* NULL for another parent */
	const struct provenance_source *src;
	struct commit *commit;
	/* the trees of the source and of its first parent */
	const struct object_id *post_tree;
	const struct object_id *pre_tree;
	/* per actor of the source's line record: its index, -1 for a human */
	int *actors;
	/* without a line record: the paths it changes, -> struct change */
	struct strmap changes;
	/*
	 * For a revert: who wrote the lines of each path that the source
	 * changes, before it did (path -> struct restored_file).
	 */
	struct strmap restored;
	/* a line of the commit takes its owners from the source */
	unsigned contributed : 1;
	/*
	 * Another parent of a merge: its pre-image is the first parent,
	 * and the lines that differ belong to the pseudo-actor `other` of
	 * struct attribution.
	 */
	unsigned other : 1;
};

/* The line record of a commit that a blame finds, imported once. */
struct blamed_record {
	struct oidmap_entry entry;
	/* NULL if the commit has no line record */
	const struct provenance_record *rec;
	/* per actor of `rec`: its index, -1 for a human */
	int *actors;
};

/* Who wrote a line that a revert restores. */
struct restored_line {
	/* the record of the commit that wrote it, or NULL */
	const struct blamed_record *b;
	/* the range of that record that covers it, or NULL if unknown */
	const struct provenance_range *range;
};

/* The lines of a file before the commit that a revert undoes. */
struct restored_file {
	size_t nr;
	struct restored_line lines[FLEX_ARRAY];
};

struct attribution {
	struct repository *r;
	struct provenance_journal *journal;
	/* the commit HEAD points at, or the null oid */
	struct object_id head;

	/* the sources, then the other parents of a merge */
	struct seed *seeds;
	size_t sources_nr, seeds_nr;
	/* the non-empty blobs that the other parents change paths to */
	struct oidset other_blobs;
	/* PROVENANCE_REWRITE_* */
	unsigned flags;

	/*
	 * For reverts: the records of the commits that a blame of the
	 * parent of a source finds (struct blamed_record, by commit).
	 */
	struct provenance_cache cache;
	struct oidmap blamed;

	struct session_info *sessions;
	size_t sessions_nr, sessions_alloc;
	/*
	 * The actor indices of the human, of an unknown origin and of the
	 * other parents of a merge.
	 */
	int human, unknown, other;
	/* the actor index of the lines that nobody is known to own */
	int fallback;

	struct owner_set *sets;
	size_t sets_nr, sets_alloc;

	struct interval *intervals;
	size_t intervals_nr, intervals_alloc;

	/* per journal entry: a warning about it has been printed */
	char *warned;

	/* per actor: order of first use in the record, 1-based; 0 if unused */
	int *rank;
	int ranked;
};

static int find_set(struct attribution *a, const int *actors, size_t nr)
{
	struct owner_set *s;

	for (size_t i = 0; i < a->sets_nr; i++)
		if (a->sets[i].nr == nr &&
		    !memcmp(a->sets[i].actors, actors, st_mult(nr, sizeof(*actors))))
			return i;

	ALLOC_GROW(a->sets, a->sets_nr + 1, a->sets_alloc);
	s = &a->sets[a->sets_nr];
	DUP_ARRAY(s->actors, actors, nr);
	s->nr = nr;
	return a->sets_nr++;
}

static int singleton(struct attribution *a, int actor)
{
	return find_set(a, &actor, 1);
}

/* Return the set `set` with `actor` added. */
static int set_add(struct attribution *a, int set, int actor)
{
	const struct owner_set *s = &a->sets[set];
	int *actors;
	size_t i, nr = 0;
	int ret;

	ALLOC_ARRAY(actors, s->nr + 1);
	for (i = 0; i < s->nr && s->actors[i] < actor; i++)
		actors[nr++] = s->actors[i];
	if (i < s->nr && s->actors[i] == actor) {
		free(actors);
		return set;
	}
	actors[nr++] = actor;
	for (; i < s->nr; i++)
		actors[nr++] = s->actors[i];
	ret = find_set(a, actors, nr);
	free(actors);
	return ret;
}

static void warn_entry(struct attribution *a, size_t entry,
		       const struct object_id *oid)
{
	struct strbuf buf = STRBUF_INIT;

	if (a->warned[entry])
		return;
	a->warned[entry] = 1;
	provenance_journal_entry_format(&a->journal->entries[entry], &buf);
	warning(_("ignoring journal entry '%s': cannot read object %s"),
		buf.buf, oid_to_hex(oid));
	strbuf_release(&buf);
}

static int has_type(struct attribution *a, const struct object_id *oid,
		    enum object_type type)
{
	if (type == OBJ_BLOB && is_empty_blob_oid(oid, a->r->hash_algo))
		return 1;
	return odb_read_object_info(a->r->objects, oid, NULL) == type;
}

static int read_blob(struct attribution *a, const struct object_id *oid,
		     mmfile_t *mf)
{
	enum object_type type;
	size_t size;
	void *data;

	if (is_empty_blob_oid(oid, a->r->hash_algo)) {
		mf->ptr = xstrdup("");
		mf->size = 0;
		return 0;
	}
	data = odb_read_object(a->r->objects, oid, &type, &size);
	if (!data || type != OBJ_BLOB) {
		free(data);
		return error(_("unable to read blob '%s'"), oid_to_hex(oid));
	}
	mf->ptr = data;
	mf->size = size;
	return 0;
}

static int session_index(struct attribution *a, const char *id)
{
	struct session_info *s;

	for (size_t i = 0; i < a->sessions_nr; i++)
		if (!strcmp(a->sessions[i].id, id))
			return i;

	ALLOC_GROW(a->sessions, a->sessions_nr + 1, a->sessions_alloc);
	s = &a->sessions[a->sessions_nr];
	memset(s, 0, sizeof(*s));
	s->id = id;
	string_list_init_dup(&s->claims);
	strmap_init(&s->hunks);
	return a->sessions_nr++;
}

static void session_info_release(struct session_info *s)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;

	string_list_clear(&s->claims, 0);
	strmap_for_each_entry(&s->hunks, &iter, e) {
		struct hunks *h = e->value;

		free(h->ranges);
		free(h->entry);
	}
	strmap_clear(&s->hunks, 1);
	free(s->actor_id);
}

/* A run of lines on one side of a hunk, as it is read. */
struct hunk_run {
	struct strbuf text;
	/* '+' or '-', or 0 if there is no run */
	char side;
	unsigned long begin, nr;
};

/*
 * Add `run`, if there is one, of the patch of the journal entry
 * `entry` to the hunks that `s` claims in `path`, unless `path` is
 * empty, and start over.
 */
static void flush_run(struct attribution *a, struct session_info *s,
		      size_t entry, const char *path, struct hunk_run *run)
{
	struct provenance_range *range;
	struct hunks *h;

	if (run->side && *path) {
		h = strmap_get(&s->hunks, path);
		if (!h) {
			CALLOC_ARRAY(h, 1);
			strmap_put(&s->hunks, path, h);
		}
		ALLOC_GROW(h->ranges, h->nr + 1, h->alloc);
		ALLOC_GROW(h->entry, h->nr + 1, h->entry_alloc);
		range = &h->ranges[h->nr];
		memset(range, 0, sizeof(*range));
		range->side = run->side;
		range->begin = run->begin;
		range->end = run->begin + run->nr - 1;
		hash_object_file(a->r->hash_algo, run->text.buf, run->text.len,
				 OBJ_BLOB, &range->anchor);
		h->entry[h->nr++] = entry;
	}
	strbuf_reset(&run->text);
	run->side = 0;
}

/*
 * Parse the path of a "--- " or "+++ " line of a patch, the `len`
 * bytes at `p`, into `path`, without its `prefix`.  `path` is left
 * empty if the line names no such path, as for "/dev/null".
 */
static void patch_path(const char *p, size_t len, const char *prefix,
		       struct strbuf *path)
{
	struct strbuf buf = STRBUF_INIT;
	const char *rest;

	strbuf_reset(path);
	if (*p == '"') {
		struct strbuf quoted = STRBUF_INIT;

		strbuf_add(&quoted, p, len);
		if (unquote_c_style(&buf, quoted.buf, NULL))
			strbuf_reset(&buf);
		strbuf_release(&quoted);
	} else {
		const char *tab = memchr(p, '\t', len);

		strbuf_add(&buf, p, tab ? (size_t)(tab - p) : len);
	}
	if (skip_prefix(buf.buf, prefix, &rest))
		strbuf_addstr(path, rest);
	strbuf_release(&buf);
}

/*
 * Claim the hunks of a patch: the runs of lines that they add to the
 * path of a "+++ b/<path>" line and remove from the path of a "---
 * a/<path>" line.  apply_claims() locates them in the changes of the
 * commit.
 */
static void claim_patch(struct attribution *a, size_t entry,
			struct session_info *s)
{
	const struct object_id *oid = &a->journal->entries[entry].oid;
	struct strbuf old_path = STRBUF_INIT, new_path = STRBUF_INIT;
	struct hunk_run run = { .text = STRBUF_INIT };
	unsigned long old_line = 0, new_line = 0, old_left = 0, new_left = 0;
	enum object_type type;
	size_t size;
	char *buf = odb_read_object(a->r->objects, oid, &type, &size);
	const char *line, *end;
	char last = 0;

	if (!buf || type != OBJ_BLOB) {
		free(buf);
		warn_entry(a, entry, oid);
		return;
	}

	for (line = buf, end = buf + size; line < end; ) {
		const char *eol = memchr(line, '\n', end - line);
		size_t len = eol ? (size_t)(eol - line) : (size_t)(end - line);
		const char *p = line, *q;
		/* an empty line of a hunk is an empty context line */
		char c = len ? *line : ' ';

		line = eol ? eol + 1 : end;
		if (c == '\\') {
			/* the line before has no LF at the end of the file */
			if (last && last == run.side)
				strbuf_strip_suffix(&run.text, "\n");
			last = 0;
			continue;
		}
		if ((old_left || new_left) &&
		    (c == ' ' || c == '-' || c == '+')) {
			if (c != run.side)
				flush_run(a, s, entry, run.side == '-' ?
					  old_path.buf : new_path.buf, &run);
			if (c != ' ') {
				if (!run.side) {
					run.side = c;
					run.begin = c == '-' ? old_line : new_line;
					run.nr = 0;
				}
				strbuf_add(&run.text, p + 1, len - 1);
				strbuf_addch(&run.text, '\n');
				run.nr++;
			}
			if (c != '+' && old_left) {
				old_line++;
				old_left--;
			}
			if (c != '-' && new_left) {
				new_line++;
				new_left--;
			}
			last = c;
			continue;
		}

		flush_run(a, s, entry, run.side == '-' ?
			  old_path.buf : new_path.buf, &run);
		last = 0;
		old_left = new_left = 0;
		if (skip_prefix_mem(p, len, "--- ", &q, &len)) {
			patch_path(q, len, "a/", &old_path);
		} else if (skip_prefix_mem(p, len, "+++ ", &q, &len)) {
			patch_path(q, len, "b/", &new_path);
		} else if (skip_prefix_mem(p, len, "@@ -", &q, &len)) {
			char *e;

			old_line = strtoul(q, &e, 10);
			old_left = *e == ',' ? strtoul(e + 1, &e, 10) : 1;
			if (!skip_prefix(e, " +", &q)) {
				old_left = 0;
				continue;
			}
			new_line = strtoul(q, &e, 10);
			new_left = *e == ',' ? strtoul(e + 1, &e, 10) : 1;
			if (*e != ' ')
				old_left = new_left = 0;
		} else if (skip_prefix_mem(p, len, "diff ", &q, &len)) {
			strbuf_reset(&old_path);
			strbuf_reset(&new_path);
		}
	}
	flush_run(a, s, entry, run.side == '-' ?
		  old_path.buf : new_path.buf, &run);
	strbuf_release(&run.text);
	strbuf_release(&old_path);
	strbuf_release(&new_path);
	free(buf);
}

/*
 * Mark the entries of the journal taken on another commit than the one
 * HEAD points at as foreign, and the "begin" and "end" entries of an
 * interval during which HEAD moved as crossing.  An entry without a
 * head counts as taken on HEAD; the two entries of an interval of
 * which only one has a head are classified by that one.
 */
static void classify_entries(struct attribution *a)
{
	struct provenance_journal *j = a->journal;
	struct strintmap open;

	strintmap_init(&open, -1);
	for (size_t i = 0; i < j->nr; i++) {
		struct provenance_journal_entry *e = &j->entries[i];
		struct provenance_journal_entry *b;
		int begin;

		e->foreign = !is_null_oid(&e->head) &&
			     !oideq(&e->head, &a->head);
		e->crossing = e->used = 0;
		if (e->type == PROVENANCE_JOURNAL_BEGIN) {
			strintmap_set(&open, e->session, (int)i);
			continue;
		}
		if (e->type != PROVENANCE_JOURNAL_END)
			continue;
		begin = strintmap_get(&open, e->session);
		if (begin < 0)
			continue;
		strintmap_set(&open, e->session, -1);
		b = &j->entries[begin];
		if (!is_null_oid(&b->head) && !is_null_oid(&e->head) &&
		    !oideq(&b->head, &e->head))
			b->crossing = e->crossing = 1;
		else
			b->foreign = e->foreign = b->foreign || e->foreign;
	}
	strintmap_clear(&open);
}

static void collect_sessions(struct attribution *a, const char *session)
{
	const struct provenance_journal *j = a->journal;

	for (size_t i = 0; i < j->nr; i++) {
		const struct provenance_journal_entry *e = &j->entries[i];
		int idx = session_index(a, e->session);
		struct session_info *s = &a->sessions[idx];

		switch (e->type) {
		case PROVENANCE_JOURNAL_BEGIN:
			if (!s->started)
				s->started = e->time;
			if (!s->model && e->model)
				s->model = e->model;
			if (!s->tool && e->tool)
				s->tool = e->tool;
			break;
		case PROVENANCE_JOURNAL_END:
			if (e->crossing)
				break;
			s->ended = e->time;
			s->stamped = 1;
			break;
		case PROVENANCE_JOURNAL_EDIT:
			s->stamped = 1;
			break;
		case PROVENANCE_JOURNAL_REASON:
			s->summary = &e->oid;
			break;
		case PROVENANCE_JOURNAL_CLAIM_PATH:
			/* a path claimed on another commit says nothing */
			if (!e->foreign)
				string_list_append(&s->claims, e->path);
			break;
		case PROVENANCE_JOURNAL_CLAIM_PATCH:
			claim_patch(a, i, s);
			break;
		}
	}

	if (session) {
		int idx = session_index(a, session);
		struct session_info *s = &a->sessions[idx];

		if (!s->stamped) {
			warning(_("session '%s' has no stamps in the journal; "
				  "recording its changes as declared "
				  "(see 'git stamp claim')"), session);
			if (!s->claims.nr && !strmap_get_size(&s->hunks))
				s->claim_all = 1;
		}
	}

	for (size_t i = 0; i < a->sessions_nr; i++) {
		struct session_info *s = &a->sessions[i];

		string_list_sort(&s->claims);
		/* "human" and "-" have a meaning in records already */
		if (!strcmp(s->id, "human") || !strcmp(s->id, "-"))
			s->actor_id = xstrfmt("@%s", s->id);
		else
			s->actor_id = xstrdup(s->id);
	}
}

/* The content of one side of a filepair: empty unless a regular file. */
static void side_oid(struct attribution *a, const struct diff_filespec *spec,
		     struct object_id *oid)
{
	if (DIFF_FILE_VALID(spec) && S_ISREG(spec->mode))
		oidcpy(oid, &spec->oid);
	else
		oidcpy(oid, a->r->hash_algo->empty_blob);
}

/* Record the paths that differ between two snapshots in `changes`. */
static void collect_changes(struct attribution *a,
			    const struct object_id *pre,
			    const struct object_id *post,
			    struct strmap *changes)
{
	struct diff_options opt;

	repo_diff_setup(a->r, &opt);
	opt.flags.recursive = 1;
	opt.output_format = DIFF_FORMAT_NO_OUTPUT;
	opt.detect_rename = 0;
	diff_setup_done(&opt);
	diff_tree_oid(pre, post, "", &opt);

	for (int i = 0; i < diff_queued_diff.nr; i++) {
		const struct diff_filepair *p = diff_queued_diff.queue[i];
		struct change *c = strmap_get(changes, p->two->path);

		/* a type change may come as a deletion and an addition */
		if (!c) {
			CALLOC_ARRAY(c, 1);
			side_oid(a, p->one, &c->pre);
			strmap_put(changes, p->two->path, c);
		}
		side_oid(a, p->two, &c->post);
	}
	diff_flush(&opt);
}

static struct interval *add_interval(struct attribution *a, size_t begin,
				     size_t entry, int session)
{
	struct interval *iv;

	ALLOC_GROW(a->intervals, a->intervals_nr + 1, a->intervals_alloc);
	iv = &a->intervals[a->intervals_nr++];
	iv->entry = entry;
	iv->begin = begin;
	iv->session = session;
	iv->foreign = a->journal->entries[entry].foreign;
	strmap_init(&iv->changes);
	return iv;
}

/* The tree of the commit `oid`, or NULL if it cannot be read. */
static const struct object_id *head_tree(struct attribution *a,
					 const struct object_id *oid)
{
	struct commit *commit = lookup_commit_reference_gently(a->r, oid, 1);

	if (!commit || repo_parse_commit(a->r, commit))
		return NULL;
	return get_commit_tree_oid(commit);
}

/*
 * Turn the "end" and "edit" entries of the journal into intervals.  The
 * changes of an "end" entry are those since the "begin" of its session,
 * or, without one, since the last snapshot of any session, or, without
 * one, since the first parent `base`.  For a foreign "end" without a
 * "begin", they are those since the commit it was taken on.  The
 * entries of an interval during which HEAD moved are ignored, and only
 * the snapshots taken on HEAD are snapshots of any session.
 */
static void collect_intervals(struct attribution *a,
			      const struct object_id *base)
{
	const struct provenance_journal *j = a->journal;
	const struct object_id *last = base;
	ssize_t *open_begin;

	ALLOC_ARRAY(open_begin, a->sessions_nr);
	for (size_t i = 0; i < a->sessions_nr; i++)
		open_begin[i] = -1;

	for (size_t i = 0; i < j->nr; i++) {
		const struct provenance_journal_entry *e = &j->entries[i];
		int s = session_index(a, e->session);
		const struct object_id *pre;
		ssize_t begin;
		struct interval *iv;
		struct change *c;

		switch (e->type) {
		case PROVENANCE_JOURNAL_BEGIN:
			open_begin[s] = i;
			if (e->crossing)
				break;
			if (!has_type(a, &e->oid, OBJ_TREE))
				warn_entry(a, i, &e->oid);
			else if (!e->foreign)
				last = &e->oid;
			break;
		case PROVENANCE_JOURNAL_END:
			begin = open_begin[s];
			open_begin[s] = -1;
			if (e->crossing)
				break;
			if (begin >= 0)
				/* a "begin" is warned about iff it is unusable */
				pre = a->warned[begin] ? NULL :
				      &j->entries[begin].oid;
			else if (e->foreign)
				pre = head_tree(a, &e->head);
			else
				pre = last;
			if (!has_type(a, &e->oid, OBJ_TREE)) {
				warn_entry(a, i, &e->oid);
				break;
			}
			if (!e->foreign)
				last = &e->oid;
			if (!pre)
				break;
			iv = add_interval(a, begin >= 0 ? (size_t)begin : i,
					  i, s);
			collect_changes(a, pre, &e->oid, &iv->changes);
			break;
		case PROVENANCE_JOURNAL_EDIT:
			if (!has_type(a, &e->oid, OBJ_BLOB)) {
				warn_entry(a, i, &e->oid);
				break;
			}
			if (!has_type(a, &e->oid2, OBJ_BLOB)) {
				warn_entry(a, i, &e->oid2);
				break;
			}
			iv = add_interval(a, i, i, s);
			CALLOC_ARRAY(c, 1);
			oidcpy(&c->pre, &e->oid);
			oidcpy(&c->post, &e->oid2);
			strmap_put(&iv->changes, e->path, c);
			break;
		default:
			break;
		}
	}
	free(open_begin);
}

static size_t count_lines(const mmfile_t *mf)
{
	const char *p = mf->ptr, *end = mf->ptr + mf->size;
	size_t nr = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', end - p);

		p = eol ? eol + 1 : end;
		nr++;
	}
	return nr;
}

/* The current version of a file in the chain, and who owns its lines. */
struct file_state {
	struct object_id oid;
	mmfile_t content;
	struct line_owner *owners;
	size_t nr;
	/* per line of the first parent's version: who deleted it, or -1 */
	int *deleted;
	size_t base_nr;
};

static void file_state_release(struct file_state *fs)
{
	free(fs->content.ptr);
	free(fs->owners);
	free(fs->deleted);
}

struct step {
	struct attribution *a;
	int actor;
	/*
	 * If not NULL, who owns the lines that the step changes, per line
	 * of the new version, and the lines of the first parent that it
	 * deletes, per line of the first parent; `actor` is then unused.
	 */
	const int *seed;
	const int *pre_seed;
	/*
	 * If not NULL, per line of `seed` and `pre_seed`: the index of the
	 * source the owners come from, or -1.
	 */
	const int *from;
	const int *pre_from;
	const struct line_owner *old;
	struct line_owner *new;
	int *deleted;
	long pos_a, pos_b;
};

static void step_unchanged(struct step *st, long until_a)
{
	while (st->pos_a < until_a)
		st->new[st->pos_b++] = st->old[st->pos_a++];
}

/* Whether the set `set` stands for an unknown origin. */
static int is_unknown(struct attribution *a, int set)
{
	const struct owner_set *s = &a->sets[set];
	size_t last = s->nr - 1;

	/* only the other parents have a higher index */
	if (last && s->actors[last] == a->other)
		last--;
	return s->actors[last] == a->unknown;
}

/*
 * Note that a line of the commit takes the set `set` from the source
 * `from[i]`, if there is one; the lines it gives an unknown origin do
 * not count.
 */
static void contribute(struct attribution *a, const int *from, size_t i,
		       int set)
{
	if (from && from[i] >= 0 && !is_unknown(a, set))
		a->seeds[from[i]].contributed = 1;
}

static void step_delete(struct step *st, const struct line_owner *o)
{
	if (o->set != BASELINE)
		return;
	if (st->pre_seed) {
		st->deleted[o->base - 1] = st->pre_seed[o->base - 1];
		contribute(st->a, st->pre_from, o->base - 1,
			   st->pre_seed[o->base - 1]);
	} else {
		st->deleted[o->base - 1] = singleton(st->a, st->actor);
	}
}

/* The owner of the line `b` of the new version, which the step changes. */
static int step_owner(struct step *st, long b)
{
	if (!st->seed)
		return singleton(st->a, st->actor);
	contribute(st->a, st->from, b, st->seed[b]);
	return st->seed[b];
}

/* Return the set `set` with the actors of the set `other` added. */
static int set_union(struct attribution *a, int set, int other)
{
	for (size_t i = 0; i < a->sets[other].nr; i++)
		set = set_add(a, set, a->sets[other].actors[i]);
	return set;
}

static int step_hunk(long start_a, long count_a,
		     long start_b, long count_b, void *data)
{
	struct step *st = data;
	long paired = count_a < count_b ? count_a : count_b;

	step_unchanged(st, start_a);
	if (st->pos_b != start_b)
		BUG("provenance: unchanged lines out of step");

	for (long i = 0; i < paired; i++) {
		const struct line_owner *o = &st->old[start_a + i];
		struct line_owner *n = &st->new[start_b + i];

		if (o->set == BASELINE) {
			step_delete(st, o);
			n->set = step_owner(st, start_b + i);
		} else if (st->seed) {
			n->set = set_union(st->a, o->set, st->seed[start_b + i]);
			contribute(st->a, st->from, start_b + i,
				   st->seed[start_b + i]);
		} else {
			n->set = set_add(st->a, o->set, st->actor);
		}
	}
	for (long i = paired; i < count_a; i++)
		step_delete(st, &st->old[start_a + i]);
	for (long i = paired; i < count_b; i++)
		st->new[start_b + i].set = step_owner(st, start_b + i);

	st->pos_a = start_a + count_a;
	st->pos_b = start_b + count_b;
	return 0;
}

/* As in blame.c. */
static int diff_hunks(mmfile_t *file_a, mmfile_t *file_b,
		      xdl_emit_hunk_consume_func_t hunk_func, void *cb_data)
{
	xpparam_t xpp = {0};
	xdemitconf_t xecfg = {0};
	xdemitcb_t ecb = {NULL};

	xecfg.hunk_func = hunk_func;
	ecb.priv = cb_data;
	return xdi_diff(file_a, file_b, &xpp, &xecfg, &ecb);
}

/*
 * Move the file from its current version to `next`, whose content is
 * `content`, made by `actor`: lines the step leaves alone keep their
 * owners, lines it changes gain `actor`, and lines it adds belong to
 * `actor` alone.  With `seed` and `pre_seed` (see struct step), the
 * owners they give take the place of `actor`, and the sources named by
 * `from` and `pre_from` are marked as contributing.  Takes ownership
 * of `content`.
 */
static int step_seeded(struct attribution *a, struct file_state *fs,
		       mmfile_t content, const struct object_id *next,
		       int actor, const int *seed, const int *pre_seed,
		       const int *from, const int *pre_from)
{
	struct step st = {
		.a = a,
		.actor = actor,
		.seed = seed,
		.pre_seed = pre_seed,
		.from = from,
		.pre_from = pre_from,
		.old = fs->owners,
		.deleted = fs->deleted,
	};
	size_t nr;

	nr = count_lines(&content);
	CALLOC_ARRAY(st.new, nr);

	if (diff_hunks(&fs->content, &content, step_hunk, &st)) {
		free(content.ptr);
		free(st.new);
		return error(_("unable to diff '%s' and '%s'"),
			     oid_to_hex(&fs->oid), oid_to_hex(next));
	}
	step_unchanged(&st, fs->nr);
	if ((size_t)st.pos_b != nr)
		BUG("provenance: step produced %ld lines, expected %"PRIuMAX,
		    st.pos_b, (uintmax_t)nr);

	free(fs->content.ptr);
	free(fs->owners);
	fs->content = content;
	fs->owners = st.new;
	fs->nr = nr;
	oidcpy(&fs->oid, next);
	return 0;
}

static int step_to(struct attribution *a, struct file_state *fs,
		   mmfile_t content, const struct object_id *next, int actor)
{
	return step_seeded(a, fs, content, next, actor, NULL, NULL, NULL, NULL);
}

/* As step_to(), with the content of the blob `next`. */
static int step(struct attribution *a, struct file_state *fs,
		const struct object_id *next, int actor)
{
	mmfile_t content;

	if (read_blob(a, next, &content))
		return -1;
	return step_to(a, fs, content, next, actor);
}

/* Read the content of one side of a file: empty unless a regular file. */
static int read_side(struct attribution *a,
		     struct diff_filespec *spec, mmfile_t *mf)
{
	if (!DIFF_FILE_VALID(spec) || !S_ISREG(spec->mode)) {
		mf->ptr = xstrdup("");
		mf->size = 0;
		return 0;
	}
	if (diff_populate_filespec(a->r, spec, NULL))
		return error(_("unable to read blob '%s'"),
			     oid_to_hex(&spec->oid));
	mf->ptr = xmemdupz(spec->data, spec->size);
	mf->size = spec->size;
	return 0;
}

/* Read `path` of `tree` and its blob id: empty unless a regular file. */
static int read_path(struct attribution *a, const struct object_id *tree,
		     const char *path, struct object_id *oid, mmfile_t *mf)
{
	unsigned short mode;

	if (oideq(tree, a->r->hash_algo->empty_tree) ||
	    get_tree_entry(a->r, tree, path, oid, &mode) || !S_ISREG(mode))
		oidcpy(oid, a->r->hash_algo->empty_blob);
	return read_blob(a, oid, mf);
}

/*
 * The set of the actors `ids` of a range of the line record `rec`,
 * whose actors are imported as `actors` says, or -1 if it names none.
 */
static int record_set(struct attribution *a,
		      const struct provenance_record *rec, const int *actors,
		      const char *ids)
{
	int set = -1;

	while (*ids) {
		size_t len = strcspn(ids, ",");
		const struct provenance_actor *actor =
			provenance_record_actor(rec, ids, len);

		if (actor) {
			int i = actors[actor - rec->actors];

			if (i < 0)
				i = a->human;
			set = set < 0 ? singleton(a, i) : set_add(a, set, i);
		}
		ids += len;
		if (*ids)
			ids++;
	}
	return set;
}

/*
 * Per line of `content`, which is the version of `path` on `side` of
 * the line record of `seed`: the set of the range of the record that
 * contains the line, or -1.  The lines of an `UNKNOWN` range stay of
 * unknown origin.
 */
static int *seed_lines(struct attribution *a, const struct seed *seed,
		       const char *path, char side, const mmfile_t *content)
{
	const struct provenance_file *file =
		provenance_record_file(seed->src->lines, path);
	size_t nr = count_lines(content);
	int *sets;

	ALLOC_ARRAY(sets, nr);
	for (size_t i = 0; i < nr; i++)
		sets[i] = -1;
	for (size_t i = 0; file && i < file->ranges_nr; i++) {
		const struct provenance_range *range = &file->ranges[i];
		int set;

		if (range->side != side || !range->begin)
			continue;
		if (range->mark == PROVENANCE_MARK_UNKNOWN)
			set = singleton(a, a->unknown);
		else
			set = record_set(a, seed->src->lines, seed->actors,
					 range->actors);
		for (unsigned long l = range->begin; l <= range->end && l <= nr; l++)
			sets[l - 1] = set;
	}
	return sets;
}

struct unknown_lines {
	int *old_sets, *new_sets;
	int set;
};

static int unknown_hunk(long start_a, long count_a,
			long start_b, long count_b, void *data)
{
	struct unknown_lines *u = data;

	for (long i = 0; i < count_a; i++)
		u->old_sets[start_a + i] = u->set;
	for (long i = 0; i < count_b; i++)
		u->new_sets[start_b + i] = u->set;
	return 0;
}

/*
 * As seed_lines() for both versions of a path, `old` and `new`, of a
 * source without a line record: the lines that differ between them are
 * owned by `actor`, the unknown origin or the other parents of a merge.
 * Leaves `old_sets` and `new_sets` alone if the versions cannot be
 * compared.
 */
static void unknown_lines(struct attribution *a, mmfile_t *old, mmfile_t *new,
			  int actor, int **old_sets, int **new_sets)
{
	struct unknown_lines u = { .set = singleton(a, actor) };
	size_t old_nr = count_lines(old), new_nr = count_lines(new);

	ALLOC_ARRAY(u.old_sets, old_nr);
	for (size_t i = 0; i < old_nr; i++)
		u.old_sets[i] = -1;
	ALLOC_ARRAY(u.new_sets, new_nr);
	for (size_t i = 0; i < new_nr; i++)
		u.new_sets[i] = -1;
	if (diff_hunks(old, new, unknown_hunk, &u)) {
		free(u.old_sets);
		free(u.new_sets);
		return;
	}
	*old_sets = u.old_sets;
	*new_sets = u.new_sets;
}

/*
 * Give the lines of `path` whose set in `sets` (`nr` of them) is not
 * negative the owners that a blame of the parent of `seed`, a commit
 * being reverted, finds for them: the actors of the range that covers
 * the line in the record of the commit that wrote it.  A line whose
 * commit has no record, or whose record does not say who wrote it, is
 * of unknown origin.
 */
static void restore_owners(struct attribution *a, struct seed *seed,
			   const char *path, int *sets, size_t nr)
{
	const struct restored_file *rf = strmap_get(&seed->restored, path);

	for (size_t i = 0; i < nr; i++) {
		const struct restored_line *l;
		int set = -1;

		if (sets[i] < 0)
			continue;
		l = rf && i < rf->nr ? &rf->lines[i] : NULL;
		if (l && l->range)
			set = record_set(a, l->b->rec, l->b->actors,
					 l->range->actors);
		sets[i] = set < 0 ? singleton(a, a->unknown) : set;
	}
}

/* Whether the changes of `seed` include some to `path`. */
static int seed_has_path(struct seed *seed, const char *path)
{
	if (seed->src && seed->src->lines)
		return !!provenance_record_file(seed->src->lines, path);
	return strmap_contains(&seed->changes, path);
}

struct transplant {
	struct attribution *a;
	const int *from;
	int *to;
	char *kept;
	long pos_a, pos_b;
};

static void transplant_unchanged(struct transplant *t, long until_a)
{
	while (t->pos_a < until_a) {
		int set = t->from[t->pos_a++];

		t->kept[t->pos_b] = set >= 0;
		t->to[t->pos_b++] = set;
	}
}

static int transplant_hunk(long start_a, long count_a,
			   long start_b, long count_b, void *data)
{
	struct transplant *t = data;
	struct attribution *a = t->a;

	transplant_unchanged(t, start_a);
	if (t->pos_b != start_b)
		BUG("provenance: unchanged lines out of step");
	for (long i = 0; i < count_b; i++) {
		int set = i < count_a ? t->from[start_a + i] : -1;

		t->kept[start_b + i] = 0;
		t->to[start_b + i] = set < 0 ? -1 : set_add(a, set, a->fallback);
	}
	t->pos_a = start_a + count_a;
	t->pos_b = start_b + count_b;
	return 0;
}

/*
 * Who owns each line of `to`, if `from` is owned as `sets` says (-1
 * for a line nobody is known to own) and `to` was made from it: a
 * line kept keeps its owner, a line that replaces one gains the
 * fallback owner, and nobody is known to own any other line (-1).
 * `*kept` is set to say, per line of `to`, whether it is kept from a
 * line with an owner.  Returns NULL if the versions cannot be
 * compared.
 */
static int *transplant(struct attribution *a, mmfile_t *from,
		       const int *sets, mmfile_t *to, char **kept)
{
	struct transplant t = { .a = a, .from = sets };
	size_t nr = count_lines(to);

	ALLOC_ARRAY(t.to, nr);
	ALLOC_ARRAY(t.kept, nr);
	if (diff_hunks(from, to, transplant_hunk, &t)) {
		free(t.to);
		free(t.kept);
		return NULL;
	}
	transplant_unchanged(&t, count_lines(from));
	if ((size_t)t.pos_b != nr)
		BUG("provenance: transplant produced %ld lines, expected %"PRIuMAX,
		    t.pos_b, (uintmax_t)nr);
	*kept = t.kept;
	return t.to;
}

static int has_seed(struct attribution *a, const char *path)
{
	for (size_t i = 0; i < a->seeds_nr; i++)
		if (seed_has_path(&a->seeds[i], path))
			return 1;
	return 0;
}

/* The owners that the sources found in a version give to its lines. */
struct found_owners {
	/* per line: its set, or -1 if no source knows the line */
	int *sets;
	/* per line: the index of the source the set comes from, or -1 */
	int *from;
	/* per line: that source has the line as it is */
	char *kept;
	size_t nr;
};

static void found_owners_init(struct found_owners *fo, size_t nr)
{
	ALLOC_ARRAY(fo->sets, nr);
	ALLOC_ARRAY(fo->from, nr);
	CALLOC_ARRAY(fo->kept, nr);
	for (size_t i = 0; i < nr; i++)
		fo->sets[i] = fo->from[i] = -1;
	fo->nr = nr;
}

static void found_owners_release(struct found_owners *fo)
{
	free(fo->sets);
	free(fo->from);
	free(fo->kept);
}

/*
 * Merge into `fo` the owners that the source `src` gives to the lines
 * of `to`, as transplant() computes them from its version `from`,
 * owned as `sets` says.  A line keeps the set of an earlier source,
 * unless only the new one has the line as it is.  Returns -1 if the
 * versions cannot be compared.
 */
static int merge_owners(struct attribution *a, struct found_owners *fo,
			int src, mmfile_t *from, const int *sets, mmfile_t *to)
{
	char *kept;
	int *to_sets = transplant(a, from, sets, to, &kept);

	if (!to_sets)
		return -1;
	for (size_t i = 0; i < fo->nr; i++) {
		if (to_sets[i] < 0 ||
		    (fo->from[i] >= 0 && (fo->kept[i] || !kept[i])))
			continue;
		fo->sets[i] = to_sets[i];
		fo->from[i] = src;
		fo->kept[i] = kept[i];
	}
	free(to_sets);
	free(kept);
	return 0;
}

/*
 * Move the file, whose version in the first parent is `base`, to
 * `found`, whose blob id is `found_oid`, through the changes to `path`
 * of the sources `begin` to `end` - 1 and then of the other parents of
 * a merge, which are all found in `found`.  The changes of a source are
 * those from the pre-image of its record to the post-image, or the
 * reverse for a revert.  The lines and the deletions that the record
 * covers get its owners, those that a source without a record changes
 * are of unknown origin, and those in which another parent differs
 * from the first are the other parents'; a line that replaces one of
 * those gains the fallback owner, who also owns the lines that no
 * source knows.  For a source that is a revert, the lines that it
 * deleted, which the revert restores, get the owners that a blame
 * finds instead (see restore_owners()).  Takes ownership of `found`.
 */
static int step_found(struct attribution *a, const char *path,
		      struct file_state *fs, mmfile_t *base,
		      size_t begin, size_t end,
		      mmfile_t found, const struct object_id *found_oid)
{
	int fallback = singleton(a, a->fallback);
	size_t others = a->seeds_nr - a->sources_nr;
	struct found_owners owners, deleters;
	int ret = 0;

	found_owners_init(&owners, count_lines(&found));
	found_owners_init(&deleters, count_lines(base));
	for (size_t k = begin; !ret && k < end + others; k++) {
		/* the other parents come after the sources */
		size_t i = k < end ? k : a->sources_nr + (k - end);
		struct seed *seed = &a->seeds[i];
		int reversed = seed->src && seed->src->revert;
		const struct object_id *new_tree, *old_tree;
		mmfile_t new = { 0 }, old = { 0 };
		struct object_id new_oid, old_oid;
		int *new_sets = NULL, *old_sets = NULL;

		if (!seed_has_path(seed, path))
			continue;
		new_tree = reversed ? seed->pre_tree : seed->post_tree;
		old_tree = reversed ? seed->post_tree : seed->pre_tree;
		if (read_path(a, new_tree, path, &new_oid, &new) ||
		    read_path(a, old_tree, path, &old_oid, &old)) {
			ret = -1;
		} else {
			if (seed->other) {
				unknown_lines(a, &old, &new, a->other,
					      &old_sets, &new_sets);
			} else if (reversed) {
				/* first, the lines of `new` the source deleted */
				unknown_lines(a, &old, &new, a->unknown,
					      &old_sets, &new_sets);
				if (new_sets)
					restore_owners(a, seed, path, new_sets,
						       count_lines(&new));
				if (seed->src->lines) {
					free(old_sets);
					old_sets = seed_lines(a, seed, path,
							      '+', &old);
				}
			} else if (seed->src->lines) {
				new_sets = seed_lines(a, seed, path,
						      '+', &new);
				old_sets = seed_lines(a, seed, path,
						      '-', &old);
			} else {
				unknown_lines(a, &old, &new, a->unknown,
					      &old_sets, &new_sets);
			}
			if (!new_sets || !old_sets ||
			    merge_owners(a, &owners, i, &new, new_sets, &found) ||
			    merge_owners(a, &deleters, i, &old, old_sets, base))
				ret = error(_("unable to diff '%s' with its version in %s"),
					    path, oid_to_hex(&seed->commit->object.oid));
		}
		free(new.ptr);
		free(old.ptr);
		free(new_sets);
		free(old_sets);
	}

	if (ret) {
		free(found.ptr);
	} else {
		for (size_t i = 0; i < owners.nr; i++)
			if (owners.sets[i] < 0)
				owners.sets[i] = fallback;
		for (size_t i = 0; i < deleters.nr; i++)
			if (deleters.sets[i] < 0)
				deleters.sets[i] = fallback;
		ret = step_seeded(a, fs, found, found_oid, a->fallback,
				  owners.sets, deleters.sets,
				  owners.from, deleters.from);
	}
	found_owners_release(&owners);
	found_owners_release(&deleters);
	return ret;
}

/*
 * Move the file, which is at its version in the first parent, through
 * the changes of each source of a rewrite whose line record has a
 * block for it, or, for a source without a line record or another
 * parent of a merge, that changes it (see step_found()).  The changes
 * of the commit being amended, if it is a version the commit was made
 * from, are found in its own post-image; those of all other sources
 * together in `next`, the next version of the chain, or if it is NULL
 * in `two`, whose blob id is `post`.  The changes of the other parents
 * are found in both.
 */
static int seed_file(struct attribution *a, const char *path,
		     struct file_state *fs, const struct object_id *next,
		     struct diff_filespec *two, const struct object_id *post)
{
	mmfile_t base, found;
	struct object_id found_oid;
	size_t begin = 0;
	int ret = 0;

	if (read_blob(a, &fs->oid, &base))
		return -1;

	if ((a->flags & PROVENANCE_REWRITE_AMEND) && a->sources_nr > 1) {
		struct seed *seed = &a->seeds[0];

		begin = 1;
		if (seed_has_path(seed, path)) {
			ret = read_path(a, seed->post_tree, path,
					&found_oid, &found);
			if (!ret)
				ret = step_found(a, path, fs, &base, 0, 1,
						 found, &found_oid);
		}
	}

	for (size_t i = begin; !ret && i < a->seeds_nr; i++) {
		if (!seed_has_path(&a->seeds[i], path))
			continue;
		if (next) {
			ret = read_blob(a, next, &found);
			oidcpy(&found_oid, next);
		} else {
			ret = read_side(a, two, &found);
			oidcpy(&found_oid, post);
		}
		if (!ret)
			ret = step_found(a, path, fs, &base, begin,
					 a->sources_nr, found, &found_oid);
		break;
	}
	free(base.ptr);
	return ret;
}

/* The set `set` without the other parents, or BASELINE if that is empty. */
static int drop_other(struct attribution *a, int set)
{
	const struct owner_set *s;

	if (set < 0)
		return set;
	s = &a->sets[set];
	if (s->actors[s->nr - 1] != a->other)
		return set;
	if (s->nr == 1)
		return BASELINE;
	return find_set(a, s->actors, s->nr - 1);
}

/*
 * Leave out of the record of a merge what came from its other parents:
 * the lines of the file, which is at its version in the commit, that
 * the version of another parent has as they are, and the lines of its
 * version in the first parent, `base`, that the commit deletes and
 * another parent lacks.  The other lines keep their owners, without
 * the other parents.
 */
static int drop_others(struct attribution *a, const char *path,
		       struct file_state *fs, const struct object_id *base)
{
	mmfile_t pre;
	int ret = 0;

	if (a->sources_nr == a->seeds_nr)
		return 0;
	if (read_blob(a, base, &pre))
		return -1;
	for (size_t i = a->sources_nr; !ret && i < a->seeds_nr; i++) {
		struct seed *seed = &a->seeds[i];
		struct object_id oid;
		mmfile_t theirs;
		int *lacked = NULL, *added = NULL, *changed = NULL, *ours = NULL;

		if (!seed_has_path(seed, path))
			continue;
		if (read_path(a, seed->post_tree, path, &oid, &theirs)) {
			ret = -1;
			break;
		}
		unknown_lines(a, &pre, &theirs, a->other, &lacked, &added);
		unknown_lines(a, &theirs, &fs->content, a->other,
			      &changed, &ours);
		if (!lacked || !ours)
			ret = error(_("unable to diff '%s' with its version in %s"),
				    path, oid_to_hex(&seed->commit->object.oid));
		for (size_t j = 0; !ret && j < fs->base_nr; j++)
			if (lacked[j] >= 0)
				fs->deleted[j] = -1;
		for (size_t j = 0; !ret && j < fs->nr; j++)
			if (ours[j] < 0)
				fs->owners[j].set = BASELINE;
		free(theirs.ptr);
		free(lacked);
		free(added);
		free(changed);
		free(ours);
	}
	free(pre.ptr);

	for (size_t j = 0; j < fs->nr; j++)
		fs->owners[j].set = drop_other(a, fs->owners[j].set);
	for (size_t j = 0; j < fs->base_nr; j++)
		fs->deleted[j] = drop_other(a, fs->deleted[j]);
	return ret;
}

/*
 * Mark in `added` the lines of the file, at its version in the commit,
 * and in `deleted` those of its version `pre` in the first parent,
 * that the hunks `h` of the session `s` claim, locating the hunks of
 * each patch together.  A hunk that is not found claims nothing, with
 * a warning.  Returns -1 if `pre` cannot be read.
 */
static int locate_hunks(struct attribution *a, const struct session_info *s,
			const char *path, const struct hunks *h,
			const struct object_id *pre,
			const struct file_state *fs,
			char *added, char *deleted)
{
	struct provenance_range *ranges;
	mmfile_t old;
	char *found;
	int lost = 0;

	if (read_blob(a, pre, &old))
		return -1;
	DUP_ARRAY(ranges, h->ranges, h->nr);
	CALLOC_ARRAY(found, h->nr);
	for (size_t i = 0, j; i < h->nr; i = j) {
		for (j = i + 1; j < h->nr && h->entry[j] == h->entry[i]; j++)
			; /* find the end of the ranges of the patch */
		if (provenance_patch_locate_ranges(a->r, &old, &fs->content,
						   ranges + i, j - i,
						   found + i))
			memset(found + i, 0, j - i);
	}
	for (size_t i = 0; i < h->nr; i++) {
		const struct provenance_range *range = &ranges[i];

		if (!found[i])
			lost = 1;
		else
			memset((range->side == '+' ? added : deleted) +
			       range->begin - 1, 1,
			       range->end - range->begin + 1);
	}
	if (lost)
		warning(_("session '%s' claims hunks of '%s' that are not "
			  "among the changes; they are not claimed"),
			s->id, path);
	free(ranges);
	free(found);
	free(old.ptr);
	return 0;
}

/*
 * Give the human lines of claimed paths, and of the claimed hunks of
 * the patches, to the declared sessions.  `pre` is the version of the
 * file in the first parent.  Returns 0 on success, or -1 after
 * reporting an error.
 */
static int apply_claims(struct attribution *a, const char *path,
			const struct object_id *pre, struct file_state *fs)
{
	int human = singleton(a, a->human);

	for (size_t i = 0; i < a->sessions_nr; i++) {
		struct session_info *s = &a->sessions[i];
		const struct hunks *h = strmap_get(&s->hunks, path);
		char *added = NULL, *deleted = NULL;
		int whole, set;

		if (s->stamped)
			continue;
		whole = s->claim_all ||
			string_list_has_string(&s->claims, path);
		if (!whole && !h)
			continue;
		if (!whole) {
			CALLOC_ARRAY(added, fs->nr);
			CALLOC_ARRAY(deleted, fs->base_nr);
			if (locate_hunks(a, s, path, h, pre, fs,
					 added, deleted)) {
				free(added);
				free(deleted);
				return -1;
			}
		}
		set = singleton(a, i);
		for (size_t j = 0; j < fs->nr; j++)
			if (fs->owners[j].set == human && (whole || added[j]))
				fs->owners[j].set = set;
		for (size_t j = 0; j < fs->base_nr; j++)
			if (fs->deleted[j] == human && (whole || deleted[j]))
				fs->deleted[j] = set;
		free(added);
		free(deleted);
	}
	return 0;
}

/*
 * Whether the change `c` of the interval `iv` changes its path and its
 * blobs can be read, with a warning about the entry if they cannot.
 */
static int usable_change(struct attribution *a, const struct interval *iv,
			 const struct change *c)
{
	if (!c || oideq(&c->pre, &c->post))
		return 0;
	if (!has_type(a, &c->pre, OBJ_BLOB)) {
		warn_entry(a, iv->entry, &c->pre);
		return 0;
	}
	if (!has_type(a, &c->post, OBJ_BLOB)) {
		warn_entry(a, iv->entry, &c->post);
		return 0;
	}
	return 1;
}

/* The runs of lines that a change adds, as they are found. */
struct added_runs {
	struct attribution *a;
	/* the post-image, and the start of its line `line` (1-based) */
	const mmfile_t *post;
	const char *pos;
	unsigned long line;
	struct provenance_range *ranges;
	size_t nr, alloc;
};

/* Advance `ar->pos` to the start of the line `line` of the post-image. */
static void skip_to_line(struct added_runs *ar, unsigned long line)
{
	const char *end = ar->post->ptr + ar->post->size;

	while (ar->line < line && ar->pos < end) {
		const char *eol = memchr(ar->pos, '\n', end - ar->pos);

		ar->pos = eol ? eol + 1 : end;
		ar->line++;
	}
}

static int add_run(long start_a UNUSED, long count_a UNUSED,
		   long start_b, long count_b, void *data)
{
	struct added_runs *ar = data;
	struct provenance_range *range;
	const char *from;

	if (count_b <= 0)
		return 0;
	ALLOC_GROW(ar->ranges, ar->nr + 1, ar->alloc);
	range = &ar->ranges[ar->nr++];
	memset(range, 0, sizeof(*range));
	range->side = '+';
	range->begin = start_b + 1;
	range->end = start_b + count_b;
	skip_to_line(ar, range->begin);
	from = ar->pos;
	skip_to_line(ar, range->end + 1);
	hash_object_file(ar->a->r->hash_algo, from, ar->pos - from, OBJ_BLOB,
			 &range->anchor);
	return 0;
}

/*
 * Give the human lines that the commit adds to the file, at its
 * version in the commit, and that a foreign interval added, to the
 * session of the interval: the runs of lines that each change of the
 * interval to `path` adds are located by their content among the runs
 * of lines that the commit adds, as the hunks of "claim --patch" are.
 * `pre` is the version of the file in the first parent.  An interval
 * that gives a line to its session is marked as used.  Returns 0 on
 * success, or -1 after reporting an error.
 */
static int apply_foreign(struct attribution *a, const char *path,
			 const struct object_id *pre, struct file_state *fs)
{
	int human = singleton(a, a->human);
	mmfile_t old = { 0 };
	int ret = 0;

	for (size_t i = 0; !ret && i < a->intervals_nr; i++) {
		struct interval *iv = &a->intervals[i];
		const struct change *c = strmap_get(&iv->changes, path);
		mmfile_t before, after;
		struct added_runs ar = { .a = a, .line = 1 };
		char *found = NULL;
		int set = -1;

		if (!iv->foreign || !usable_change(a, iv, c))
			continue;
		if ((!old.ptr && read_blob(a, pre, &old)) ||
		    read_blob(a, &c->pre, &before)) {
			ret = -1;
			break;
		}
		if (read_blob(a, &c->post, &after)) {
			free(before.ptr);
			ret = -1;
			break;
		}
		ar.post = &after;
		ar.pos = after.ptr;
		if (diff_hunks(&before, &after, add_run, &ar))
			ret = error(_("unable to diff '%s' and '%s'"),
				    oid_to_hex(&c->pre), oid_to_hex(&c->post));
		CALLOC_ARRAY(found, ar.nr);
		if (!ret && ar.nr &&
		    provenance_patch_locate_ranges(a->r, &old, &fs->content,
						   ar.ranges, ar.nr, found))
			ret = error(_("unable to diff '%s'"), path);
		for (size_t k = 0; !ret && k < ar.nr; k++) {
			const struct provenance_range *range = &ar.ranges[k];

			if (!found[k])
				continue;
			for (unsigned long l = range->begin; l <= range->end; l++) {
				if (fs->owners[l - 1].set != human)
					continue;
				if (set < 0)
					set = singleton(a, iv->session);
				fs->owners[l - 1].set = set;
			}
		}
		if (set >= 0) {
			a->journal->entries[iv->begin].used = 1;
			a->journal->entries[iv->entry].used = 1;
		}
		free(found);
		free(ar.ranges);
		free(before.ptr);
		free(after.ptr);
	}
	free(old.ptr);
	return ret;
}

/*
 * Fill in the mark and the actors of a range owned by `set`, ranking
 * the actors by first use as they are seen.  A line of unknown origin
 * stays `UNKNOWN` whoever else touched it, as `MIXED` would claim to
 * know who wrote it; its other actors are neither listed nor attached.
 */
static void describe_set(struct attribution *a, int set, int added,
			 struct provenance_range *range)
{
	const struct owner_set *s = &a->sets[set];
	struct strbuf buf = STRBUF_INIT;
	int ai = 0, human = 0;
	int *order;

	if (is_unknown(a, set)) {
		range->actors = xstrdup("-");
		range->mark = PROVENANCE_MARK_UNKNOWN;
		return;
	}

	ALLOC_ARRAY(order, s->nr);
	for (size_t i = 0; i < s->nr; i++) {
		int actor = s->actors[i];
		size_t j = i;

		if (!a->rank[actor])
			a->rank[actor] = ++a->ranked;
		if (actor == a->human) {
			human = 1;
		} else {
			ai = 1;
			if (added)
				a->sessions[actor].attached = 1;
		}
		while (j && a->rank[order[j - 1]] > a->rank[actor]) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = actor;
	}

	for (size_t i = 0; i < s->nr; i++) {
		if (i)
			strbuf_addch(&buf, ',');
		strbuf_addstr(&buf, order[i] == a->human ? "human" :
			      a->sessions[order[i]].actor_id);
	}
	range->actors = strbuf_detach(&buf, NULL);
	range->mark = !human ? PROVENANCE_MARK_AI :
		      ai ? PROVENANCE_MARK_MIXED : PROVENANCE_MARK_HUMAN;
	free(order);
}

/*
 * Add a range for each run of consecutive lines of `content` with the
 * same owner set in `sets` (lines with a negative set are left out).
 * `whole` is the blob id of `content`.
 */
static void add_ranges(struct attribution *a, struct provenance_file *file,
		       char side, const mmfile_t *content,
		       const struct object_id *whole,
		       const int *sets, size_t nr)
{
	const char *p = content->ptr, *end = content->ptr + content->size;
	size_t *start;

	ALLOC_ARRAY(start, st_add(nr, 1));
	for (size_t i = 0; i < nr; i++) {
		const char *eol = memchr(p, '\n', end - p);

		start[i] = p - content->ptr;
		p = eol ? eol + 1 : end;
	}
	start[nr] = content->size;

	for (size_t i = 0, j; i < nr; i = j) {
		struct provenance_range *range;

		for (j = i + 1; j < nr && sets[j] == sets[i]; j++)
			; /* find the end of the run */
		if (sets[i] < 0)
			continue;

		ALLOC_GROW(file->ranges, file->ranges_nr + 1, file->ranges_alloc);
		range = &file->ranges[file->ranges_nr++];
		memset(range, 0, sizeof(*range));
		range->side = side;
		range->begin = i + 1;
		range->end = j;
		describe_set(a, sets[i], side == '+', range);
		if (!i && j == nr)
			oidcpy(&range->anchor, whole);
		else
			hash_object_file(a->r->hash_algo, content->ptr + start[i],
					 start[j] - start[i], OBJ_BLOB,
					 &range->anchor);
	}
	free(start);
}

static int add_file(struct attribution *a, const char *path,
		    const struct object_id *pre, const struct object_id *post,
		    struct file_state *fs, struct provenance_record *rec)
{
	struct provenance_file *file;
	int *sets;

	ALLOC_GROW(rec->files, rec->files_nr + 1, rec->files_alloc);
	file = &rec->files[rec->files_nr++];
	memset(file, 0, sizeof(*file));
	file->path = xstrdup(path);

	ALLOC_ARRAY(sets, fs->nr);
	for (size_t i = 0; i < fs->nr; i++)
		sets[i] = fs->owners[i].set;
	add_ranges(a, file, '+', &fs->content, post, sets, fs->nr);
	free(sets);

	for (size_t i = 0; i < fs->base_nr; i++) {
		mmfile_t content;

		if (fs->deleted[i] < 0)
			continue;
		if (read_blob(a, pre, &content))
			return -1;
		add_ranges(a, file, '-', &content, pre, fs->deleted,
			   fs->base_nr);
		free(content.ptr);
		break;
	}

	/* a merge took all its changes to the file from other parents */
	if (!file->ranges_nr) {
		free(file->path);
		rec->files_nr--;
	}
	return 0;
}

/*
 * Whether a merge takes `path`, whose version in the first parent is
 * `pre` and in the commit `post`, as another parent has it, or, if the
 * first parent does not have it, as another parent has some path, as
 * after a directory rename: all its lines then come from that parent,
 * and so do its deletions, and it gets no block.  This saves reading
 * the blobs, which a partial clone may not have.
 */
static int taken_from_other(struct attribution *a, const char *path,
			    const struct object_id *pre,
			    const struct object_id *post)
{
	if (is_empty_blob_oid(pre, a->r->hash_algo) &&
	    oidset_contains(&a->other_blobs, post))
		return 1;
	for (size_t i = a->sources_nr; i < a->seeds_nr; i++) {
		const struct change *c = strmap_get(&a->seeds[i].changes, path);

		if (c && oideq(&c->post, post))
			return 1;
	}
	return 0;
}

/*
 * Attribute the changes to `path` from `one` (its version in the first
 * parent) to `two`, and add its block to `rec`.  The content of `two`
 * is taken from its data if it has been populated, so that it need not
 * be in the object database.
 */
static int attribute_file(struct attribution *a, const char *path,
			  struct diff_filespec *one, struct diff_filespec *two,
			  struct provenance_record *rec)
{
	struct object_id pre, post;
	struct file_state fs = { 0 };
	struct {
		const struct change *change;
		int session;
	} *chain = NULL;
	size_t chain_nr = 0, chain_alloc = 0;
	int ret = 0;

	if ((DIFF_FILE_VALID(one) && !S_ISREG(one->mode)) ||
	    (DIFF_FILE_VALID(two) && !S_ISREG(two->mode)) ||
	    strchr(path, '\n'))
		return 0;
	side_oid(a, one, &pre);
	side_oid(a, two, &post);
	if (oideq(&pre, &post) || taken_from_other(a, path, &pre, &post) ||
	    (DIFF_FILE_VALID(one) && diff_filespec_is_binary(a->r, one)) ||
	    (DIFF_FILE_VALID(two) && diff_filespec_is_binary(a->r, two)))
		goto out;

	for (size_t i = 0; i < a->intervals_nr; i++) {
		struct interval *iv = &a->intervals[i];
		const struct change *c = strmap_get(&iv->changes, path);

		if (iv->foreign || !usable_change(a, iv, c))
			continue;
		ALLOC_GROW(chain, chain_nr + 1, chain_alloc);
		chain[chain_nr].change = c;
		chain[chain_nr].session = iv->session;
		chain_nr++;
	}

	if (!DIFF_FILE_VALID(one) && !chain_nr && !has_seed(a, path)) {
		/* a new file that no session touched: no need to diff */
		int fallback = singleton(a, a->fallback);

		/* the binary check above has usually read it already */
		if (diff_populate_filespec(a->r, two, NULL)) {
			ret = error(_("unable to read blob '%s'"),
				    oid_to_hex(&post));
			goto out;
		}
		fs.content.ptr = xmemdupz(two->data, two->size);
		fs.content.size = two->size;
		fs.nr = count_lines(&fs.content);
		ALLOC_ARRAY(fs.owners, fs.nr);
		for (size_t i = 0; i < fs.nr; i++)
			fs.owners[i].set = fallback;
	} else {
		if (read_blob(a, &pre, &fs.content)) {
			ret = -1;
			goto out;
		}
		oidcpy(&fs.oid, &pre);
		fs.nr = fs.base_nr = count_lines(&fs.content);
		ALLOC_ARRAY(fs.owners, fs.nr);
		ALLOC_ARRAY(fs.deleted, fs.base_nr);
		for (size_t i = 0; i < fs.nr; i++) {
			fs.owners[i].set = BASELINE;
			fs.owners[i].base = i + 1;
			fs.deleted[i] = -1;
		}

		if (has_seed(a, path))
			ret = seed_file(a, path, &fs,
					chain_nr ? &chain[0].change->pre : NULL,
					two, &post);
		for (size_t i = 0; !ret && i < chain_nr; i++) {
			const struct change *c = chain[i].change;

			if (!oideq(&fs.oid, &c->pre))
				ret = step(a, &fs, &c->pre, a->fallback);
			if (!ret)
				ret = step(a, &fs, &c->post, chain[i].session);
		}
		if (!ret && !oideq(&fs.oid, &post)) {
			mmfile_t content;

			ret = read_side(a, two, &content);
			if (!ret)
				ret = step_to(a, &fs, content, &post, a->fallback);
		}
		if (ret)
			goto out;
	}

	ret = drop_others(a, path, &fs, &pre);
	if (ret)
		goto out;
	ret = apply_foreign(a, path, &pre, &fs);
	if (ret)
		goto out;
	ret = apply_claims(a, path, &pre, &fs);
	if (ret)
		goto out;
	ret = add_file(a, path, &pre, &post, &fs, rec);

out:
	diff_free_filespec_data(one);
	diff_free_filespec_data(two);
	file_state_release(&fs);
	free(chain);
	return ret;
}

static void add_actors(struct attribution *a, const char *human,
		       struct provenance_record *rec)
{
	int ai = 0;

	for (int rank = 1; rank <= a->ranked; rank++) {
		struct provenance_actor *actor;
		int i = 0;

		while (a->rank[i] != rank)
			i++;
		ALLOC_GROW(rec->actors, rec->actors_nr + 1, rec->actors_alloc);
		actor = &rec->actors[rec->actors_nr++];
		memset(actor, 0, sizeof(*actor));

		if (i == a->human) {
			actor->id = xstrdup("human");
			actor->kind = PROVENANCE_ACTOR_HUMAN;
			actor->ident = xstrdup(human);
		} else {
			const struct session_info *s = &a->sessions[i];

			actor->id = xstrdup(s->actor_id);
			actor->kind = PROVENANCE_ACTOR_AI;
			actor->model = xstrdup(s->model ? s->model : "unknown");
			actor->tool = xstrdup(s->tool ? s->tool : "unknown");
			actor->session = xstrdup(s->id);
			actor->source = xstrdup(s->source ? s->source :
						s->stamped ? "observed" : "declared");
			ai = 1;
		}
	}

	/* declared if AI is involved and no session was observed */
	for (size_t i = 0; ai && i < a->sessions_nr; i++) {
		const struct session_info *s = &a->sessions[i];

		if (s->source ? !strcmp(s->source, "observed") : s->stamped)
			ai = 0;
	}
	if (a->flags & PROVENANCE_REWRITE_ADOPT) {
		rec->capture = PROVENANCE_CAPTURE_ADOPTED;
	} else if (ai) {
		rec->capture = PROVENANCE_CAPTURE_DECLARED;
	} else {
		rec->capture = PROVENANCE_CAPTURE_OBSERVED;
		rec->capture_tool = xstrfmt("git/%s", git_version_string);
	}
}

static void add_session(struct attribution *a, int i,
			struct provenance_reasoning *reasoning)
{
	const struct session_info *s = &a->sessions[i];
	struct provenance_session *out;
	size_t j;

	/* a session of a source may have stamps in the journal, too */
	for (j = 0; j < reasoning->sessions_nr; j++)
		if (!strcmp(reasoning->sessions[j].id, s->id))
			break;
	if (j == reasoning->sessions_nr) {
		ALLOC_GROW(reasoning->sessions, reasoning->sessions_nr + 1,
			   reasoning->sessions_alloc);
		out = &reasoning->sessions[reasoning->sessions_nr++];
		out->id = xstrdup(s->id);
		out->model = xstrdup(s->model ? s->model : "unknown");
		out->tool = xstrdup(s->tool ? s->tool : "unknown");
		out->started = s->started;
		out->ended = s->ended;
	}

	if (s->summary) {
		mmfile_t summary;

		if (!has_type(a, s->summary, OBJ_BLOB) ||
		    read_blob(a, s->summary, &summary)) {
			warning(_("cannot read the summary of session '%s'"),
				s->id);
			return;
		}
		if (summary.size) {
			if (reasoning->summary.len)
				strbuf_addch(&reasoning->summary, '\n');
			strbuf_add(&reasoning->summary, summary.ptr, summary.size);
			strbuf_complete_line(&reasoning->summary);
		}
		free(summary.ptr);
	}
}

/*
 * The sessions attached to the commit are those that own a line of
 * it, in order of first use, and the session named with "--session".
 */
static void add_sessions(struct attribution *a, const char *session,
			 struct provenance_reasoning *reasoning)
{
	for (int rank = 1; rank <= a->ranked; rank++) {
		int i = 0;

		while (a->rank[i] != rank)
			i++;
		if (i != a->human && a->sessions[i].attached &&
		    !a->sessions[i].imported)
			add_session(a, i, reasoning);
	}
	if (session) {
		int i = session_index(a, session);

		if (!a->sessions[i].attached)
			add_session(a, i, reasoning);
	}
}

/*
 * Mark the foreign "reason" and "claim --patch" entries of the
 * sessions attached to the commit as used, as their summary or their
 * hunks are.
 */
static void mark_used(struct attribution *a)
{
	struct provenance_journal *j = a->journal;

	for (size_t i = 0; i < j->nr; i++) {
		struct provenance_journal_entry *e = &j->entries[i];

		if (e->foreign &&
		    (e->type == PROVENANCE_JOURNAL_REASON ||
		     e->type == PROVENANCE_JOURNAL_CLAIM_PATCH) &&
		    a->sessions[session_index(a, e->session)].attached)
			e->used = 1;
	}
}

struct provenance_attribution {
	struct attribution a;
	/* the tree of the first parent, or the empty tree */
	struct object_id base;
};

/*
 * Declare the AI actors of the line record `rec`, and return the index
 * of each of its actors, -1 for a human.  An actor whose id is already
 * declared is that actor; the first declaration from a record gives
 * its metadata.
 */
static int *import_actors(struct attribution *a,
			  const struct provenance_record *rec)
{
	int *actors;

	ALLOC_ARRAY(actors, rec->actors_nr);
	for (size_t i = 0; i < rec->actors_nr; i++) {
		const struct provenance_actor *actor = &rec->actors[i];
		struct session_info *s;
		size_t j;

		actors[i] = -1;
		if (actor->kind != PROVENANCE_ACTOR_AI)
			continue;
		for (j = 0; j < a->sessions_nr; j++)
			if (!strcmp(a->sessions[j].actor_id, actor->id))
				break;
		if (j == a->sessions_nr) {
			ALLOC_GROW(a->sessions, a->sessions_nr + 1,
				   a->sessions_alloc);
			s = &a->sessions[a->sessions_nr++];
			memset(s, 0, sizeof(*s));
			s->id = actor->session;
			s->actor_id = xstrdup(actor->id);
			string_list_init_dup(&s->claims);
			strmap_init(&s->hunks);
			s->imported = 1;
		}
		s = &a->sessions[j];
		if (!s->source) {
			s->model = actor->model;
			s->tool = actor->tool;
			s->source = actor->source;
		}
		actors[i] = j;
	}
	return actors;
}

/* The record of `commit`, imported once, or NULL if it has none. */
static const struct blamed_record *blamed_record(struct attribution *a,
						 struct commit *commit)
{
	struct blamed_record *b = oidmap_get(&a->blamed, &commit->object.oid);

	if (!b) {
		CALLOC_ARRAY(b, 1);
		oidcpy(&b->entry.oid, &commit->object.oid);
		b->rec = provenance_cache_get(&a->cache, &commit->object.oid);
		if (b->rec)
			b->actors = import_actors(a, b->rec);
		oidmap_put(&a->blamed, b);
	}
	return b->rec ? b : NULL;
}

/*
 * Blame `path`, a regular file, in `parent`, and note in `restored`
 * who wrote each of its lines, importing the actors of the records of
 * the commits that the blame finds.  This is the blame of "git blame
 * <parent> -- <path>": renames are followed, but lines moved or copied
 * within or across files are not looked for.
 */
static void blame_path(struct attribution *a, struct commit *parent,
		       const char *path, struct strmap *restored)
{
	struct rev_info revs;
	struct blame_scoreboard sb;
	struct blame_origin *o;
	struct blame_entry *ent;
	struct restored_file *rf;

	repo_init_revisions(a->r, &revs, NULL);
	add_pending_object(&revs, &parent->object, "");
	init_scoreboard(&sb);
	sb.revs = &revs;
	sb.repo = a->r;
	sb.path = path;
	/* do not mark a root commit as a boundary */
	sb.show_root = 1;
	sb.xdl_opts = revs.diffopt.xdl_opts & XDF_INDENT_HEURISTIC;
	setup_scoreboard(&sb, &o);
	if (sb.num_lines) {
		o->suspects = blame_entry_prepend(NULL, 0, sb.num_lines, o);
		prio_queue_put(&sb.commits, o->commit);
	}
	blame_origin_decref(o);
	assign_blame(&sb, 0);

	rf = xcalloc(1, st_add(sizeof(*rf),
			       st_mult(sb.num_lines, sizeof(rf->lines[0]))));
	rf->nr = sb.num_lines;
	for (ent = sb.ent; ent; ) {
		struct blame_entry *next = ent->next;
		const struct blamed_record *b =
			blamed_record(a, ent->suspect->commit);
		const struct provenance_file *file = b ?
			provenance_record_file(b->rec, ent->suspect->path) :
			NULL;

		for (int i = 0; file && i < ent->num_lines; i++) {
			struct restored_line *l = &rf->lines[ent->lno + i];

			l->b = b;
			l->range = provenance_file_range(file, '+',
							 ent->s_lno + i + 1);
			if (l->range &&
			    l->range->mark == PROVENANCE_MARK_UNKNOWN)
				l->range = NULL;
		}
		blame_origin_decref(ent->suspect);
		free(ent);
		ent = next;
	}
	strmap_put(restored, path, rf);

	cleanup_scoreboard(&sb);
	release_revisions(&revs);
	clear_commit_marks(parent, ALL_REV_FLAGS);
}

/*
 * For a revert of `seed`: blame in its first parent each file that it
 * changes and that the parent has, so that the lines the revert
 * restores get the owners of the commits that wrote them.  See
 * restore_owners().
 */
static void blame_restored(struct attribution *a, struct seed *seed)
{
	struct commit *c = seed->src->commit;
	struct strmap changes = STRMAP_INIT;
	struct hashmap_iter iter;
	struct strmap_entry *e;

	if (!c->parents)
		return;
	collect_changes(a, seed->pre_tree, seed->post_tree, &changes);
	strmap_for_each_entry(&changes, &iter, e) {
		const struct change *ch = e->value;

		if (!is_empty_blob_oid(&ch->pre, a->r->hash_algo))
			blame_path(a, c->parents->item, e->key,
				   &seed->restored);
	}
	strmap_clear(&changes, 1);
}

/*
 * Add the sources, and then the other parents of a merge, `others`, to
 * the seeds; `base` is the tree of the first parent.
 */
static int add_seeds(struct attribution *a,
		     const struct provenance_source *sources, size_t nr,
		     const struct commit_list *others,
		     const struct object_id *base)
{
	CALLOC_ARRAY(a->seeds, st_add(nr, commit_list_count(others)));
	for (size_t i = 0; i < nr; i++) {
		struct seed *seed = &a->seeds[a->seeds_nr];
		struct commit *c = sources[i].commit;

		if (repo_parse_commit(a->r, c))
			return error(_("could not parse commit %s"),
				     oid_to_hex(&c->object.oid));
		seed->src = &sources[i];
		seed->commit = c;
		seed->post_tree = get_commit_tree_oid(c);
		seed->pre_tree = a->r->hash_algo->empty_tree;
		if (c->parents) {
			struct commit *parent = c->parents->item;

			if (repo_parse_commit(a->r, parent))
				return error(_("could not parse commit %s"),
					     oid_to_hex(&parent->object.oid));
			seed->pre_tree = get_commit_tree_oid(parent);
		}
		strmap_init(&seed->changes);
		strmap_init(&seed->restored);
		if (!seed->src->lines)
			collect_changes(a, seed->pre_tree, seed->post_tree,
					&seed->changes);
		else
			seed->actors = import_actors(a, seed->src->lines);
		a->seeds_nr++;
	}
	a->sources_nr = a->seeds_nr;

	for (; others; others = others->next) {
		struct seed *seed = &a->seeds[a->seeds_nr];
		struct commit *c = others->item;
		struct hashmap_iter iter;
		struct strmap_entry *e;

		if (repo_parse_commit(a->r, c))
			return error(_("could not parse commit %s"),
				     oid_to_hex(&c->object.oid));
		seed->commit = c;
		seed->other = 1;
		seed->post_tree = get_commit_tree_oid(c);
		seed->pre_tree = base;
		strmap_init(&seed->changes);
		strmap_init(&seed->restored);
		collect_changes(a, seed->pre_tree, seed->post_tree,
				&seed->changes);
		strmap_for_each_entry(&seed->changes, &iter, e) {
			const struct change *ch = e->value;

			if (!is_empty_blob_oid(&ch->post, a->r->hash_algo))
				oidset_insert(&a->other_blobs, &ch->post);
		}
		a->seeds_nr++;
	}
	return 0;
}

static int attribution_init(struct attribution *a, struct repository *r,
			    const struct object_id *base,
			    const struct commit_list *others,
			    struct provenance_journal *journal,
			    const char *session,
			    const struct provenance_source *sources,
			    size_t nr, unsigned flags)
{
	int ret;

	memset(a, 0, sizeof(*a));
	a->r = r;
	a->journal = journal;
	a->flags = flags;
	provenance_cache_init(&a->cache, r);
	oidmap_init(&a->blamed, 0);
	CALLOC_ARRAY(a->warned, journal->nr);
	provenance_journal_head(r, &a->head);
	classify_entries(a);
	collect_sessions(a, session);
	ret = add_seeds(a, sources, nr, others, base);
	/* the blame may declare actors, which come before the human */
	for (size_t i = 0; !ret && i < a->sources_nr; i++)
		if (a->seeds[i].src->revert)
			blame_restored(a, &a->seeds[i]);
	a->human = a->sessions_nr;
	a->unknown = a->human + 1;
	a->other = a->unknown + 1;
	a->fallback = (flags & PROVENANCE_REWRITE_ADOPT) ? a->unknown : a->human;
	CALLOC_ARRAY(a->rank, a->other + 1);
	if (!ret)
		collect_intervals(a, base);
	return ret;
}

static void free_blamed_record(void *p)
{
	struct blamed_record *b = p;

	free(b->actors);
	free(b);
}

static void attribution_release(struct attribution *a)
{
	for (size_t i = 0; i < a->seeds_nr; i++) {
		free(a->seeds[i].actors);
		strmap_clear(&a->seeds[i].changes, 1);
		strmap_clear(&a->seeds[i].restored, 1);
	}
	free(a->seeds);
	oidset_clear(&a->other_blobs);
	for (size_t i = 0; i < a->sessions_nr; i++)
		session_info_release(&a->sessions[i]);
	free(a->sessions);
	for (size_t i = 0; i < a->sets_nr; i++)
		free(a->sets[i].actors);
	free(a->sets);
	for (size_t i = 0; i < a->intervals_nr; i++)
		strmap_clear(&a->intervals[i].changes, 1);
	free(a->intervals);
	free(a->warned);
	free(a->rank);
	oidmap_clear_with_free(&a->blamed, free_blamed_record);
	provenance_cache_release(&a->cache);
}

struct provenance_attribution *
provenance_attribution_new(struct repository *r, const struct object_id *base,
			   const struct commit_list *others,
			   struct provenance_journal *journal)
{
	struct provenance_attribution *pa;

	CALLOC_ARRAY(pa, 1);
	oidcpy(&pa->base, base);
	if (attribution_init(&pa->a, r, &pa->base, others, journal, NULL,
			     NULL, 0, 0)) {
		provenance_attribution_free(pa);
		return NULL;
	}
	return pa;
}

int provenance_attribute_path(struct provenance_attribution *pa,
			      const char *path, const char *buf, size_t len,
			      unsigned mode, struct provenance_record *lines)
{
	struct repository *r = pa->a.r;
	struct diff_filespec *one = alloc_filespec(path);
	struct diff_filespec *two = alloc_filespec(path);
	struct object_id oid;
	unsigned short base_mode;
	int ret;

	if (!get_tree_entry(r, &pa->base, path, &oid, &base_mode))
		fill_filespec(one, &oid, 1, base_mode);
	if (buf) {
		/* hashed, not written: the content stays in memory */
		hash_object_file(r->hash_algo, buf, len, OBJ_BLOB, &oid);
		fill_filespec(two, &oid, 1, mode);
		two->data = (char *)buf;
		two->size = len;
	}
	ret = attribute_file(&pa->a, path, one, two, lines);
	free_filespec(one);
	free_filespec(two);
	return ret;
}

void provenance_attribution_free(struct provenance_attribution *pa)
{
	if (!pa)
		return;
	attribution_release(&pa->a);
	free(pa);
}

/* Add the sessions and the summary of `from` that `to` does not have. */
static void copy_reasoning(const struct provenance_reasoning *from,
			   struct provenance_reasoning *to)
{
	for (size_t i = 0; i < from->sessions_nr; i++) {
		const struct provenance_session *s = &from->sessions[i];
		struct provenance_session *out;
		size_t j;

		for (j = 0; j < to->sessions_nr; j++)
			if (!strcmp(to->sessions[j].id, s->id))
				break;
		if (j < to->sessions_nr)
			continue;
		ALLOC_GROW(to->sessions, to->sessions_nr + 1, to->sessions_alloc);
		out = &to->sessions[to->sessions_nr++];
		out->id = xstrdup(s->id);
		out->model = xstrdup_or_null(s->model);
		out->tool = xstrdup_or_null(s->tool);
		out->started = s->started;
		out->ended = s->ended;
	}
	if (from->summary.len) {
		if (to->summary.len)
			strbuf_addch(&to->summary, '\n');
		strbuf_addbuf(&to->summary, &from->summary);
		strbuf_complete_line(&to->summary);
	}
}

int provenance_attribute(struct repository *r,
			 const struct commit_list *parents,
			 const struct object_id *tree,
			 struct provenance_journal *journal,
			 const char *session,
			 const char *human,
			 struct provenance_record *lines,
			 struct provenance_reasoning *reasoning)
{
	return provenance_attribute_rewrite(r, parents, tree, NULL, 0, 0,
					    journal, session, human,
					    lines, reasoning);
}

int provenance_attribute_rewrite(struct repository *r,
				 const struct commit_list *parents,
				 const struct object_id *tree,
				 const struct provenance_source *sources,
				 size_t nr, unsigned flags,
				 struct provenance_journal *journal,
				 const char *session,
				 const char *human,
				 struct provenance_record *lines,
				 struct provenance_reasoning *reasoning)
{
	struct attribution a;
	const struct object_id *base = r->hash_algo->empty_tree;
	struct diff_options opt;
	int ret = 0;

	if ((flags & PROVENANCE_REWRITE_AMEND) && !nr)
		BUG("provenance: an amend without a source");
	if ((flags & PROVENANCE_REWRITE_ADOPT) && journal->nr)
		BUG("provenance: adopting with a journal");
	if (parents) {
		struct commit *parent = parents->item;

		if (repo_parse_commit(r, parent))
			return error(_("could not parse commit %s"),
				     oid_to_hex(&parent->object.oid));
		base = get_commit_tree_oid(parent);
	}

	if (attribution_init(&a, r, base, parents ? parents->next : NULL,
			     journal, session, sources, nr, flags)) {
		attribution_release(&a);
		return -1;
	}

	lines->version = 1;
	repo_diff_setup(r, &opt);
	opt.flags.recursive = 1;
	opt.output_format = DIFF_FORMAT_NO_OUTPUT;
	opt.detect_rename = 0;
	FREE_AND_NULL(opt.orderfile);
	diff_setup_done(&opt);
	diff_tree_oid(base, tree, "", &opt);
	diffcore_std(&opt);
	for (int i = 0; !ret && i < diff_queued_diff.nr; i++) {
		struct diff_filepair *p = diff_queued_diff.queue[i];

		ret = attribute_file(&a, p->two->path, p->one, p->two, lines);
	}
	diff_flush(&opt);

	if (!ret) {
		add_actors(&a, human, lines);
		/*
		 * A tentative source gives its reasoning only if the
		 * commit was found to be made from it.
		 */
		for (size_t i = 0; i < nr; i++)
			if (sources[i].reasoning && !sources[i].revert &&
			    (!sources[i].tentative || a.seeds[i].contributed))
				copy_reasoning(sources[i].reasoning, reasoning);
		add_sessions(&a, session, reasoning);
		mark_used(&a);
	}
	attribution_release(&a);
	return ret;
}
