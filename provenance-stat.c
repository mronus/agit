/*
 * Summing the line records of a revision walk.
 */

#include "git-compat-util.h"
#include "provenance-stat.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "lockfile.h"
#include "notes.h"
#include "odb.h"
#include "path.h"
#include "refs.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "tag.h"

void provenance_stat_add_record(struct provenance_stat *stat,
				const struct provenance_record *rec)
{
	for (size_t i = 0; i < rec->files_nr; i++) {
		const struct provenance_file *file = &rec->files[i];

		for (size_t j = 0; j < file->ranges_nr; j++) {
			const struct provenance_range *range = &file->ranges[j];
			uintmax_t *lines = range->side == '+' ?
				stat->added : stat->removed;

			if (range->end < range->begin)
				continue;
			lines[range->mark] += range->end - range->begin + 1;
		}
	}
}

uintmax_t provenance_stat_share(const struct provenance_stat *stat)
{
	uintmax_t total = 0, ai;

	for (size_t i = 0; i < ARRAY_SIZE(stat->added); i++)
		total += stat->added[i];
	ai = stat->added[PROVENANCE_MARK_AI] +
	     stat->added[PROVENANCE_MARK_MIXED];
	return total ? (ai * 100 + total / 2) / total : 0;
}

/*
 * Load the notes tree of refs/provenance/lines into `t`.  Returns 1 if
 * it was loaded, 0 if the ref does not exist, and -1, after reporting
 * an error, if it cannot be read or does not point to a tree.
 */
static int load_notes(struct repository *r, struct notes_tree *t)
{
	struct object_id oid;
	struct object *obj;
	int flags = 0;

	if (!refs_resolve_ref_unsafe(get_main_ref_store(r),
				     PROVENANCE_LINES_REF, 0, &oid, &flags) ||
	    (flags & REF_ISBROKEN))
		return error(_("unable to read ref '%s'"), PROVENANCE_LINES_REF);
	if (is_null_oid(&oid))
		return 0;

	obj = deref_tag(r, parse_object(r, &oid), PROVENANCE_LINES_REF, 0);
	if (!obj || (obj->type != OBJ_COMMIT && obj->type != OBJ_TREE))
		return error(_("ref '%s' does not point to a tree"),
			     PROVENANCE_LINES_REF);

	init_notes(t, PROVENANCE_LINES_REF, NULL, 0);
	return 1;
}

/*
 * Set `lines` to the lines of the line record of `commit` stored in
 * `blob`.  Returns 0 on success, and -1, after a warning, if the record
 * cannot be read or parsed.
 */
static int record_lines(struct repository *r, const struct object_id *commit,
			const struct object_id *blob,
			struct provenance_stat *lines)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct strbuf err = STRBUF_INIT;
	enum object_type type;
	size_t size;
	void *data;
	int ret = -1;

	memset(lines, 0, sizeof(*lines));
	data = odb_read_object(r->objects, blob, &type, &size);
	if (!data || type != OBJ_BLOB)
		warning(_("ignoring unreadable provenance record of %s"),
			oid_to_hex(commit));
	else if (provenance_record_parse(data, size, r->hash_algo,
					 &rec, &err))
		warning(_("ignoring malformed provenance record of %s: %s"),
			oid_to_hex(commit), err.buf);
	else
		ret = 0;
	if (!ret)
		provenance_stat_add_record(lines, &rec);

	provenance_record_release(&rec);
	strbuf_release(&err);
	free(data);
	return ret;
}

static void add_lines(struct provenance_stat *stat,
		      const struct provenance_stat *lines)
{
	for (size_t i = 0; i < ARRAY_SIZE(stat->added); i++) {
		stat->added[i] += lines->added[i];
		stat->removed[i] += lines->removed[i];
	}
}

/*
 * The cache of the lines of line records, keyed by the object id of
 * the blob of the record.  See the "Statistics cache" section of
 * Documentation/technical/git-provenance-format.adoc.
 */
struct cache_entry {
	struct oidmap_entry entry;
	struct provenance_stat lines;
};

struct stat_cache {
	char *path;
	struct oidmap entries;
	/* the entries added during this run, in the order they were added */
	struct cache_entry **added;
	size_t added_nr, added_alloc;
};

#define CACHE_HEADER "version 1\n"

static int parse_count(const char **p, const char *eol, uintmax_t *n)
{
	char *end;

	if (*p >= eol || **p != ' ' || !isdigit((*p)[1]))
		return -1;
	errno = 0;
	*n = strtoumax(*p + 1, &end, 10);
	if (errno || end > eol)
		return -1;
	*p = end;
	return 0;
}

/*
 * Parse the cache file in `buf`, and add its entries to `entries`, or
 * only check it if `entries` is NULL.  Returns 0 if the whole file
 * parses, and -1 otherwise.
 */
static int parse_cache(const struct git_hash_algo *algop,
		       const char *buf, size_t len, struct oidmap *entries)
{
	const char *p = buf, *end = buf + len;

	if (!skip_prefix_mem(buf, len, CACHE_HEADER, &p, &len))
		return -1;
	while (p < end) {
		const char *eol = memchr(p, '\n', end - p);
		struct cache_entry *e;
		int ok = 1;

		if (!eol)
			return -1;
		CALLOC_ARRAY(e, 1);
		if (parse_oid_hex_algop(p, &e->entry.oid, &p, algop))
			ok = 0;
		for (size_t i = 0; ok && i < ARRAY_SIZE(e->lines.added); i++)
			ok = !parse_count(&p, eol, &e->lines.added[i]);
		for (size_t i = 0; ok && i < ARRAY_SIZE(e->lines.removed); i++)
			ok = !parse_count(&p, eol, &e->lines.removed[i]);
		if (!ok || p != eol) {
			free(e);
			return -1;
		}
		if (entries)
			free(oidmap_put(entries, e));
		else
			free(e);
		p = eol + 1;
	}
	return 0;
}

static void cache_load(struct repository *r, struct stat_cache *cache)
{
	struct strbuf buf = STRBUF_INIT;

	cache->path = xstrfmt("%s/info/provenance-stat",
			      repo_get_object_directory(r));
	oidmap_init(&cache->entries, 0);
	if (strbuf_read_file(&buf, cache->path, 0) < 0) {
		if (errno != ENOENT)
			warning_errno(_("unable to read '%s'"), cache->path);
	} else if (parse_cache(r->hash_algo, buf.buf, buf.len,
			       &cache->entries)) {
		warning(_("ignoring malformed provenance stat cache '%s'"),
			cache->path);
		oidmap_clear(&cache->entries, 1);
		oidmap_init(&cache->entries, 0);
	}
	strbuf_release(&buf);
}

static void cache_add(struct stat_cache *cache, const struct object_id *blob,
		      const struct provenance_stat *lines)
{
	struct cache_entry *e;

	CALLOC_ARRAY(e, 1);
	oidcpy(&e->entry.oid, blob);
	e->lines = *lines;
	oidmap_put(&cache->entries, e);
	ALLOC_GROW(cache->added, cache->added_nr + 1, cache->added_alloc);
	cache->added[cache->added_nr++] = e;
}

/*
 * Append the entries added during this run to the cache file, or
 * write it anew if it is missing or does not parse.  The cache is only
 * an aid, so a failure is a warning.
 */
static void cache_write(struct repository *r, struct stat_cache *cache)
{
	struct lock_file lk = LOCK_INIT;
	struct strbuf buf = STRBUF_INIT;

	if (!cache->added_nr)
		return;
	if (safe_create_leading_directories_const(r, cache->path) ||
	    repo_hold_lock_file_for_update(r, &lk, cache->path, 0) < 0) {
		warning_errno(_("unable to lock '%s'"), cache->path);
		return;
	}

	/* another run may have written the file since it was loaded */
	if (strbuf_read_file(&buf, cache->path, 0) < 0 ||
	    parse_cache(r->hash_algo, buf.buf, buf.len, NULL)) {
		strbuf_reset(&buf);
		strbuf_addstr(&buf, CACHE_HEADER);
	}
	for (size_t i = 0; i < cache->added_nr; i++) {
		const struct cache_entry *e = cache->added[i];

		strbuf_add_oid_hex(&buf, &e->entry.oid);
		for (size_t m = 0; m < ARRAY_SIZE(e->lines.added); m++)
			strbuf_addf(&buf, " %"PRIuMAX, e->lines.added[m]);
		for (size_t m = 0; m < ARRAY_SIZE(e->lines.removed); m++)
			strbuf_addf(&buf, " %"PRIuMAX, e->lines.removed[m]);
		strbuf_addch(&buf, '\n');
	}

	if (write_in_full(get_lock_file_fd(&lk), buf.buf, buf.len) < 0) {
		warning_errno(_("unable to write '%s'"), cache->path);
		rollback_lock_file(&lk);
	} else if (commit_lock_file(&lk)) {
		warning_errno(_("unable to write '%s'"), cache->path);
	}
	strbuf_release(&buf);
}

static void cache_release(struct stat_cache *cache)
{
	oidmap_clear(&cache->entries, 1);
	free(cache->added);
	free(cache->path);
}

int provenance_stat_walk(struct repository *r, struct rev_info *revs,
			 int use_cache, struct provenance_stat *stat)
{
	struct notes_tree t = { 0 };
	struct stat_cache cache = { 0 };
	struct commit *commit;
	int have_notes, ret = 0;

	memset(stat, 0, sizeof(*stat));
	have_notes = load_notes(r, &t);
	if (have_notes < 0)
		return -1;
	if (prepare_revision_walk(revs)) {
		ret = error(_("revision walk setup failed"));
		goto out;
	}
	if (have_notes && use_cache)
		cache_load(r, &cache);

	while ((commit = get_revision(revs))) {
		const struct object_id *blob;
		struct cache_entry *e = NULL;
		struct provenance_stat lines;

		stat->commits++;
		if (!have_notes)
			continue;
		blob = get_note(&t, &commit->object.oid);
		if (!blob)
			continue;
		if (use_cache)
			e = oidmap_get(&cache.entries, blob);
		if (e) {
			lines = e->lines;
		} else {
			if (record_lines(r, &commit->object.oid, blob, &lines))
				continue;
			if (use_cache)
				cache_add(&cache, blob, &lines);
		}
		stat->recorded++;
		add_lines(stat, &lines);
	}

	if (have_notes && use_cache) {
		cache_write(r, &cache);
		cache_release(&cache);
	}
out:
	if (have_notes)
		free_notes(&t);
	return ret;
}
