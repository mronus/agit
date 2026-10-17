/*
 * The attribution walk: which actor wrote each line that a commit changes.
 */

#include "git-compat-util.h"
#include "provenance-attribute.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "provenance.h"
#include "provenance-journal.h"
#include "quote.h"
#include "repository.h"
#include "strbuf.h"
#include "string-list.h"
#include "strmap.h"
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
 * in it, and actor sessions_nr is the human making the commit.  Sets
 * are interned, so that two lines have the same owners exactly if they
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
	/* the session has "end" or "edit" entries */
	unsigned stamped : 1;
	/* every human line is the session's ("--session" without stamps) */
	unsigned claim_all : 1;
	/* the session owns a line of the commit */
	unsigned attached : 1;
};

/* A path changed by an interval. */
struct change {
	struct object_id pre;
	struct object_id post;
};

/* The changes that one "end" or "edit" entry attributes to a session. */
struct interval {
	size_t entry;
	int session;
	/* path -> struct change */
	struct strmap changes;
};

struct attribution {
	struct repository *r;
	const struct provenance_journal *journal;

	struct session_info *sessions;
	size_t sessions_nr, sessions_alloc;
	/* the actor index of the human */
	int human;

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
	return a->sessions_nr++;
}

/*
 * Claim the paths of a patch: those named by its "+++ b/<path>" and
 * "--- a/<path>" lines.  This is an approximation: the whole of each
 * path is claimed, not only the hunks of the patch.
 */
static void claim_patch(struct attribution *a, size_t entry,
			struct session_info *s)
{
	const struct object_id *oid = &a->journal->entries[entry].oid;
	struct strbuf path = STRBUF_INIT;
	enum object_type type;
	size_t size;
	char *buf = odb_read_object(a->r->objects, oid, &type, &size);
	const char *line, *end;

	if (!buf || type != OBJ_BLOB) {
		free(buf);
		warn_entry(a, entry, oid);
		return;
	}

	for (line = buf, end = buf + size; line < end; ) {
		const char *eol = memchr(line, '\n', end - line);
		size_t len = eol ? (size_t)(eol - line) : (size_t)(end - line);
		const char *p, *rest, *prefix = NULL;

		if (skip_prefix_mem(line, len, "+++ ", &p, &len))
			prefix = "b/";
		else if (skip_prefix_mem(line, len, "--- ", &p, &len))
			prefix = "a/";
		line = eol ? eol + 1 : end;
		if (!prefix)
			continue;

		strbuf_reset(&path);
		if (*p == '"') {
			struct strbuf quoted = STRBUF_INIT;

			strbuf_add(&quoted, p, len);
			if (unquote_c_style(&path, quoted.buf, NULL))
				strbuf_reset(&path);
			strbuf_release(&quoted);
		} else {
			const char *tab = memchr(p, '\t', len);

			strbuf_add(&path, p, tab ? (size_t)(tab - p) : len);
		}
		if (skip_prefix(path.buf, prefix, &rest) && *rest)
			string_list_append(&s->claims, rest);
	}
	strbuf_release(&path);
	free(buf);
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
			if (!s->claims.nr)
				s->claim_all = 1;
		}
	}

	a->human = a->sessions_nr;
	CALLOC_ARRAY(a->rank, a->sessions_nr + 1);
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

static struct interval *add_interval(struct attribution *a, size_t entry,
				     int session)
{
	struct interval *iv;

	ALLOC_GROW(a->intervals, a->intervals_nr + 1, a->intervals_alloc);
	iv = &a->intervals[a->intervals_nr++];
	iv->entry = entry;
	iv->session = session;
	strmap_init(&iv->changes);
	return iv;
}

/*
 * Turn the "end" and "edit" entries of the journal into intervals.  The
 * changes of an "end" entry are those since the "begin" of its session,
 * or, without one, since the last snapshot of any session, or, without
 * one, since the first parent `base`.
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
		struct interval *iv;
		struct change *c;

		switch (e->type) {
		case PROVENANCE_JOURNAL_BEGIN:
			open_begin[s] = i;
			if (has_type(a, &e->oid, OBJ_TREE))
				last = &e->oid;
			else
				warn_entry(a, i, &e->oid);
			break;
		case PROVENANCE_JOURNAL_END:
			pre = last;
			if (open_begin[s] >= 0) {
				pre = &j->entries[open_begin[s]].oid;
				/* a "begin" is warned about iff it is unusable */
				if (a->warned[open_begin[s]])
					pre = NULL;
				open_begin[s] = -1;
			}
			if (!has_type(a, &e->oid, OBJ_TREE)) {
				warn_entry(a, i, &e->oid);
				break;
			}
			last = &e->oid;
			if (!pre)
				break;
			iv = add_interval(a, i, s);
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
			iv = add_interval(a, i, s);
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

static void step_delete(struct step *st, const struct line_owner *o)
{
	if (o->set == BASELINE)
		st->deleted[o->base - 1] = singleton(st->a, st->actor);
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
			n->set = singleton(st->a, st->actor);
		} else {
			n->set = set_add(st->a, o->set, st->actor);
		}
	}
	for (long i = paired; i < count_a; i++)
		step_delete(st, &st->old[start_a + i]);
	for (long i = paired; i < count_b; i++)
		st->new[start_b + i].set = singleton(st->a, st->actor);

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
 * Move the file from its current version to `next`, made by `actor`:
 * lines the step leaves alone keep their owners, lines it changes gain
 * `actor`, and lines it adds belong to `actor` alone.
 */
static int step(struct attribution *a, struct file_state *fs,
		const struct object_id *next, int actor)
{
	struct step st = {
		.a = a,
		.actor = actor,
		.old = fs->owners,
		.deleted = fs->deleted,
	};
	mmfile_t content;
	size_t nr;

	if (read_blob(a, next, &content))
		return -1;
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

/* Give the human lines of claimed paths to the declared sessions. */
static void apply_claims(struct attribution *a, const char *path,
			 struct file_state *fs)
{
	int human = singleton(a, a->human);

	for (size_t i = 0; i < a->sessions_nr; i++) {
		const struct session_info *s = &a->sessions[i];
		int set;

		if (s->stamped ||
		    (!s->claim_all && !string_list_has_string(&s->claims, path)))
			continue;
		set = singleton(a, i);
		for (size_t j = 0; j < fs->nr; j++)
			if (fs->owners[j].set == human)
				fs->owners[j].set = set;
		for (size_t j = 0; j < fs->base_nr; j++)
			if (fs->deleted[j] == human)
				fs->deleted[j] = set;
	}
}

/*
 * Fill in the mark and the actors of a range owned by `set`, ranking
 * the actors by first use as they are seen.
 */
static void describe_set(struct attribution *a, int set, int added,
			 struct provenance_range *range)
{
	const struct owner_set *s = &a->sets[set];
	struct strbuf buf = STRBUF_INIT;
	int ai = 0, human = 0;
	int *order;

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
	return 0;
}

static int attribute_file(struct attribution *a,
			  struct diff_filepair *p,
			  struct provenance_record *rec)
{
	const char *path = p->two->path;
	struct object_id pre, post;
	struct file_state fs = { 0 };
	struct {
		const struct change *change;
		int session;
	} *chain = NULL;
	size_t chain_nr = 0, chain_alloc = 0;
	int ret = 0;

	if ((DIFF_FILE_VALID(p->one) && !S_ISREG(p->one->mode)) ||
	    (DIFF_FILE_VALID(p->two) && !S_ISREG(p->two->mode)) ||
	    strchr(path, '\n'))
		return 0;
	side_oid(a, p->one, &pre);
	side_oid(a, p->two, &post);
	if (oideq(&pre, &post) ||
	    (DIFF_FILE_VALID(p->one) && diff_filespec_is_binary(a->r, p->one)) ||
	    (DIFF_FILE_VALID(p->two) && diff_filespec_is_binary(a->r, p->two)))
		goto out;

	for (size_t i = 0; i < a->intervals_nr; i++) {
		struct interval *iv = &a->intervals[i];
		const struct change *c = strmap_get(&iv->changes, path);

		if (!c || oideq(&c->pre, &c->post))
			continue;
		if (!has_type(a, &c->pre, OBJ_BLOB)) {
			warn_entry(a, iv->entry, &c->pre);
			continue;
		}
		if (!has_type(a, &c->post, OBJ_BLOB)) {
			warn_entry(a, iv->entry, &c->post);
			continue;
		}
		ALLOC_GROW(chain, chain_nr + 1, chain_alloc);
		chain[chain_nr].change = c;
		chain[chain_nr].session = iv->session;
		chain_nr++;
	}

	if (!DIFF_FILE_VALID(p->one) && !chain_nr) {
		/* a new file that no session touched: no need to diff */
		int human = singleton(a, a->human);

		/* the binary check above has usually read it already */
		if (diff_populate_filespec(a->r, p->two, NULL)) {
			ret = error(_("unable to read blob '%s'"),
				    oid_to_hex(&post));
			goto out;
		}
		fs.content.ptr = xmemdupz(p->two->data, p->two->size);
		fs.content.size = p->two->size;
		fs.nr = count_lines(&fs.content);
		ALLOC_ARRAY(fs.owners, fs.nr);
		for (size_t i = 0; i < fs.nr; i++)
			fs.owners[i].set = human;
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

		for (size_t i = 0; !ret && i < chain_nr; i++) {
			const struct change *c = chain[i].change;

			if (!oideq(&fs.oid, &c->pre))
				ret = step(a, &fs, &c->pre, a->human);
			if (!ret)
				ret = step(a, &fs, &c->post, chain[i].session);
		}
		if (!ret && !oideq(&fs.oid, &post))
			ret = step(a, &fs, &post, a->human);
		if (ret)
			goto out;
	}

	apply_claims(a, path, &fs);
	ret = add_file(a, path, &pre, &post, &fs, rec);

out:
	diff_free_filespec_data(p->one);
	diff_free_filespec_data(p->two);
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
			actor->source = xstrdup(s->stamped ? "observed" : "declared");
			ai = 1;
		}
	}

	/* declared if AI is involved and no session was observed */
	for (size_t i = 0; ai && i < a->sessions_nr; i++)
		if (a->sessions[i].stamped)
			ai = 0;
	if (ai) {
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

	ALLOC_GROW(reasoning->sessions, reasoning->sessions_nr + 1,
		   reasoning->sessions_alloc);
	out = &reasoning->sessions[reasoning->sessions_nr++];
	out->id = xstrdup(s->id);
	out->model = xstrdup(s->model ? s->model : "unknown");
	out->tool = xstrdup(s->tool ? s->tool : "unknown");
	out->started = s->started;
	out->ended = s->ended;

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
		if (i != a->human && a->sessions[i].attached)
			add_session(a, i, reasoning);
	}
	if (session) {
		int i = session_index(a, session);

		if (!a->sessions[i].attached)
			add_session(a, i, reasoning);
	}
}

static void attribution_release(struct attribution *a)
{
	for (size_t i = 0; i < a->sessions_nr; i++) {
		string_list_clear(&a->sessions[i].claims, 0);
		free(a->sessions[i].actor_id);
	}
	free(a->sessions);
	for (size_t i = 0; i < a->sets_nr; i++)
		free(a->sets[i].actors);
	free(a->sets);
	for (size_t i = 0; i < a->intervals_nr; i++)
		strmap_clear(&a->intervals[i].changes, 1);
	free(a->intervals);
	free(a->warned);
	free(a->rank);
}

int provenance_attribute(struct repository *r,
			 struct commit *parent,
			 const struct object_id *tree,
			 const struct provenance_journal *journal,
			 const char *session,
			 const char *human,
			 struct provenance_record *lines,
			 struct provenance_reasoning *reasoning)
{
	struct attribution a = {
		.r = r,
		.journal = journal,
	};
	const struct object_id *base = r->hash_algo->empty_tree;
	struct diff_options opt;
	int ret = 0;

	if (parent) {
		if (repo_parse_commit(r, parent))
			return error(_("could not parse commit %s"),
				     oid_to_hex(&parent->object.oid));
		base = get_commit_tree_oid(parent);
	}

	CALLOC_ARRAY(a.warned, journal->nr);
	collect_sessions(&a, session);
	collect_intervals(&a, base);

	lines->version = 1;
	repo_diff_setup(r, &opt);
	opt.flags.recursive = 1;
	opt.output_format = DIFF_FORMAT_NO_OUTPUT;
	opt.detect_rename = 0;
	FREE_AND_NULL(opt.orderfile);
	diff_setup_done(&opt);
	diff_tree_oid(base, tree, "", &opt);
	diffcore_std(&opt);
	for (int i = 0; !ret && i < diff_queued_diff.nr; i++)
		ret = attribute_file(&a, diff_queued_diff.queue[i], lines);
	diff_flush(&opt);

	if (!ret) {
		add_actors(&a, human, lines);
		add_sessions(&a, session, reasoning);
	}
	attribution_release(&a);
	return ret;
}
