#include "git-compat-util.h"
#include "provenance-journal.h"
#include "cache-tree.h"
#include "dir.h"
#include "gettext.h"
#include "hex.h"
#include "lockfile.h"
#include "odb.h"
#include "odb/transaction.h"
#include "path.h"
#include "pathspec.h"
#include "preload-index.h"
#include "read-cache.h"
#include "repository.h"
#include "sparse-index.h"
#include "strbuf.h"
#include "string-list.h"
#include "symlinks.h"
#include "write-or-die.h"

/*
 * How long to wait for another process that is appending to the
 * journal, e.g. a parallel agent.
 */
#define JOURNAL_LOCK_TIMEOUT_MS 1000

static const char *type_names[] = {
	[PROVENANCE_JOURNAL_BEGIN] = "begin",
	[PROVENANCE_JOURNAL_END] = "end",
	[PROVENANCE_JOURNAL_EDIT] = "edit",
	[PROVENANCE_JOURNAL_REASON] = "reason",
	[PROVENANCE_JOURNAL_CLAIM_PATH] = "claim",
	[PROVENANCE_JOURNAL_CLAIM_PATCH] = "claim",
};

int provenance_session_is_valid(const char *session)
{
	if (!*session)
		return 0;
	for (const char *p = session; *p; p++)
		if (!isalnum(*p) && !strchr("._:-", *p))
			return 0;
	return 1;
}

/* A model or tool name: non-empty, without spaces or LF. */
static int is_token(const char *s)
{
	return *s && !strpbrk(s, " \n");
}

static int is_path(const char *s)
{
	return *s && !strchr(s, '\n');
}

void provenance_journal_entry_format(const struct provenance_journal_entry *e,
				     struct strbuf *out)
{
	strbuf_addf(out, "%s %"PRItime" %s ", type_names[e->type], e->time,
		    e->session);

	switch (e->type) {
	case PROVENANCE_JOURNAL_BEGIN:
		strbuf_add_oid_hex(out, &e->oid);
		if (e->model)
			strbuf_addf(out, " model=%s", e->model);
		if (e->tool)
			strbuf_addf(out, " tool=%s", e->tool);
		break;
	case PROVENANCE_JOURNAL_END:
		strbuf_add_oid_hex(out, &e->oid);
		break;
	case PROVENANCE_JOURNAL_EDIT:
		strbuf_add_oid_hex(out, &e->oid);
		strbuf_addch(out, ' ');
		strbuf_add_oid_hex(out, &e->oid2);
		strbuf_addch(out, ' ');
		strbuf_addstr(out, e->path);
		break;
	case PROVENANCE_JOURNAL_REASON:
		strbuf_add_oid_hex(out, &e->oid);
		if (!is_null_oid(&e->oid2)) {
			strbuf_addch(out, ' ');
			strbuf_add_oid_hex(out, &e->oid2);
		}
		break;
	case PROVENANCE_JOURNAL_CLAIM_PATH:
		strbuf_addstr(out, "path ");
		strbuf_addstr(out, e->path);
		break;
	case PROVENANCE_JOURNAL_CLAIM_PATCH:
		strbuf_addstr(out, "patch ");
		strbuf_add_oid_hex(out, &e->oid);
		break;
	}
}

static int check_entry(const struct provenance_journal_entry *e)
{
	if (!e->session || !provenance_session_is_valid(e->session))
		return error(_("invalid session id '%s'"),
			     e->session ? e->session : "");
	if (e->model && !is_token(e->model))
		return error(_("invalid model name '%s'"), e->model);
	if (e->tool && !is_token(e->tool))
		return error(_("invalid tool name '%s'"), e->tool);
	if ((e->type == PROVENANCE_JOURNAL_EDIT ||
	     e->type == PROVENANCE_JOURNAL_CLAIM_PATH) &&
	    (!e->path || !is_path(e->path)))
		return error(_("invalid path '%s'"), e->path ? e->path : "");
	return 0;
}

/* Take the journal's lock, creating its directory if needed. */
static int lock_journal(struct repository *r, struct lock_file *lk,
			char *path)
{
	if (safe_create_leading_directories(r, path))
		return error_errno(_("unable to create directory for '%s'"),
				   path);
	if (repo_hold_lock_file_for_update_timeout(r, lk, path,
						   LOCK_REPORT_ON_ERROR,
						   JOURNAL_LOCK_TIMEOUT_MS) < 0)
		return -1;
	return 0;
}

/*
 * Append `line` and a LF to the file `path`, by writing the result to
 * the lock file `lk` of `path` and committing it.
 *
 * The file is rewritten through the lock file rather than appended to
 * in place, so that a reader never sees a partially written line.  The
 * files only hold what was recorded since the last commit, so they
 * stay small.
 */
static int append_line(struct lock_file *lk, const char *path,
		       const struct strbuf *line)
{
	struct strbuf buf = STRBUF_INIT;
	int ret = -1;

	if (strbuf_read_file(&buf, path, 0) < 0 && errno != ENOENT) {
		error_errno(_("unable to read '%s'"), path);
		goto out;
	}
	if (buf.len && buf.buf[buf.len - 1] != '\n')
		strbuf_addch(&buf, '\n');
	strbuf_addbuf(&buf, line);
	strbuf_addch(&buf, '\n');

	if (write_in_full(get_lock_file_fd(lk), buf.buf, buf.len) < 0) {
		error_errno(_("unable to write '%s'"), get_lock_file_path(lk));
		goto out;
	}
	if (commit_lock_file(lk)) {
		error_errno(_("unable to write '%s'"), path);
		goto out;
	}
	ret = 0;

out:
	strbuf_release(&buf);
	return ret;
}

int provenance_journal_append(struct repository *r,
			      const struct provenance_journal_entry *e)
{
	struct lock_file lk = LOCK_INIT;
	struct strbuf line = STRBUF_INIT;
	char *path = repo_git_path(r, "provenance/journal");
	int ret = -1;

	if (check_entry(e) || lock_journal(r, &lk, path))
		goto out;
	provenance_journal_entry_format(e, &line);
	ret = append_line(&lk, path, &line);

out:
	rollback_lock_file(&lk);
	strbuf_release(&line);
	free(path);
	return ret;
}

int provenance_picks_append(struct repository *r,
			    const struct object_id *oid, int revert)
{
	struct lock_file lk = LOCK_INIT, picks_lk = LOCK_INIT;
	struct strbuf line = STRBUF_INIT;
	char *journal = repo_git_path(r, "provenance/journal");
	char *path = repo_git_path(r, "provenance/picks");
	int ret = -1;

	if (lock_journal(r, &lk, journal) ||
	    repo_hold_lock_file_for_update(r, &picks_lk, path,
					   LOCK_REPORT_ON_ERROR) < 0)
		goto out;
	strbuf_addf(&line, "%s %s", revert ? "revert" : "pick",
		    oid_to_hex(oid));
	ret = append_line(&picks_lk, path, &line);

out:
	rollback_lock_file(&picks_lk);
	rollback_lock_file(&lk);
	strbuf_release(&line);
	free(journal);
	free(path);
	return ret;
}

int provenance_journal_clear(struct repository *r)
{
	struct lock_file lk = LOCK_INIT;
	char *path = repo_git_path(r, "provenance/journal");
	char *picks = repo_git_path(r, "provenance/picks");
	int ret = 0;

	if (!file_exists(path) && !file_exists(picks))
		goto out;
	if (repo_hold_lock_file_for_update_timeout(r, &lk, path, 0,
						   JOURNAL_LOCK_TIMEOUT_MS) < 0) {
		ret = -1;
		goto out;
	}
	if ((unlink(path) && errno != ENOENT) ||
	    (unlink(picks) && errno != ENOENT)) {
		int saved_errno = errno;

		ret = -1;
		rollback_lock_file(&lk);
		errno = saved_errno;
		goto out;
	}
	rollback_lock_file(&lk);

out:
	free(path);
	free(picks);
	return ret;
}

int provenance_picks_read(struct repository *r,
			  struct provenance_picks *picks)
{
	struct strbuf buf = STRBUF_INIT;
	char *path = repo_git_path(r, "provenance/picks");
	char *line, *next;
	int lineno = 0, ret = 0;

	if (strbuf_read_file(&buf, path, 0) < 0) {
		if (errno != ENOENT)
			ret = error_errno(_("unable to read '%s'"), path);
		goto out;
	}

	for (line = buf.buf; line < buf.buf + buf.len; line = next) {
		struct object_id oid;
		const char *p, *end;
		int revert = 0;

		next = strchrnul(line, '\n');
		if (*next)
			*next++ = '\0';
		lineno++;
		if (skip_prefix(line, "revert ", &p))
			revert = 1;
		else if (!skip_prefix(line, "pick ", &p))
			p = line;
		if (parse_oid_hex_algop(p, &oid, &end, r->hash_algo) ||
		    *end) {
			warning(_("ignoring malformed line %d in '%s'"),
				lineno, path);
			continue;
		}
		ALLOC_GROW(picks->items, picks->nr + 1, picks->alloc);
		oidcpy(&picks->items[picks->nr].oid, &oid);
		picks->items[picks->nr].revert = revert;
		picks->nr++;
	}

out:
	strbuf_release(&buf);
	free(path);
	return ret;
}

void provenance_picks_release(struct provenance_picks *picks)
{
	FREE_AND_NULL(picks->items);
	picks->nr = picks->alloc = 0;
}

/*
 * Parse an object id that is followed by a space or by the end of the
 * line, and advance *p past it.
 */
static int parse_oid_field(const char **p, const struct git_hash_algo *algop,
			   struct object_id *oid)
{
	const char *end;

	if (parse_oid_hex_algop(*p, oid, &end, algop) ||
	    (*end && *end != ' '))
		return -1;
	*p = end;
	return 0;
}

/* Parse a value that is followed by a space or by the end of the line. */
static char *parse_value(const char **p)
{
	const char *e = strchrnul(*p, ' ');
	char *v;

	if (e == *p)
		return NULL;
	v = xstrndup(*p, e - *p);
	*p = e;
	return v;
}

static int parse_entry(const char *line, const struct git_hash_algo *algop,
		       struct provenance_journal_entry *e)
{
	const char *p = line, *sp;
	char *end;

	if (skip_prefix(p, "begin ", &p))
		e->type = PROVENANCE_JOURNAL_BEGIN;
	else if (skip_prefix(p, "end ", &p))
		e->type = PROVENANCE_JOURNAL_END;
	else if (skip_prefix(p, "edit ", &p))
		e->type = PROVENANCE_JOURNAL_EDIT;
	else if (skip_prefix(p, "reason ", &p))
		e->type = PROVENANCE_JOURNAL_REASON;
	else if (skip_prefix(p, "claim ", &p))
		e->type = PROVENANCE_JOURNAL_CLAIM_PATH;
	else
		return -1;

	if (!isdigit(*p))
		return -1;
	errno = 0;
	e->time = parse_timestamp(p, &end, 10);
	if (errno || *end != ' ')
		return -1;
	p = end + 1;

	sp = strchr(p, ' ');
	if (!sp)
		return -1;
	e->session = xstrndup(p, sp - p);
	if (!provenance_session_is_valid(e->session))
		return -1;
	p = sp + 1;

	switch (e->type) {
	case PROVENANCE_JOURNAL_BEGIN:
		if (parse_oid_field(&p, algop, &e->oid))
			return -1;
		if (skip_prefix(p, " model=", &p) &&
		    !(e->model = parse_value(&p)))
			return -1;
		if (skip_prefix(p, " tool=", &p) &&
		    !(e->tool = parse_value(&p)))
			return -1;
		break;
	case PROVENANCE_JOURNAL_END:
		if (parse_oid_field(&p, algop, &e->oid))
			return -1;
		break;
	case PROVENANCE_JOURNAL_EDIT:
		if (parse_oid_field(&p, algop, &e->oid) || *p != ' ')
			return -1;
		p++;
		if (parse_oid_field(&p, algop, &e->oid2) || *p != ' ')
			return -1;
		p++;
		if (!is_path(p))
			return -1;
		e->path = xstrdup(p);
		p += strlen(p);
		break;
	case PROVENANCE_JOURNAL_REASON:
		if (parse_oid_field(&p, algop, &e->oid))
			return -1;
		if (*p == ' ') {
			p++;
			if (parse_oid_field(&p, algop, &e->oid2))
				return -1;
		}
		break;
	case PROVENANCE_JOURNAL_CLAIM_PATH:
	case PROVENANCE_JOURNAL_CLAIM_PATCH:
		if (skip_prefix(p, "path ", &p) && is_path(p)) {
			e->path = xstrdup(p);
			p += strlen(p);
		} else if (skip_prefix(p, "patch ", &p)) {
			e->type = PROVENANCE_JOURNAL_CLAIM_PATCH;
			if (parse_oid_field(&p, algop, &e->oid))
				return -1;
		} else {
			return -1;
		}
		break;
	}
	return *p ? -1 : 0;
}

static void entry_release(struct provenance_journal_entry *e)
{
	free(e->session);
	free(e->model);
	free(e->tool);
	free(e->path);
}

int provenance_journal_read(struct repository *r,
			    struct provenance_journal *j)
{
	struct strbuf buf = STRBUF_INIT;
	char *path = repo_git_path(r, "provenance/journal");
	char *line, *next;
	int lineno = 0, ret = 0;

	if (strbuf_read_file(&buf, path, 0) < 0) {
		if (errno != ENOENT)
			ret = error_errno(_("unable to read '%s'"), path);
		goto out;
	}

	for (line = buf.buf; line < buf.buf + buf.len; line = next) {
		struct provenance_journal_entry *e;

		next = strchrnul(line, '\n');
		if (*next)
			*next++ = '\0';
		lineno++;

		ALLOC_GROW(j->entries, j->nr + 1, j->alloc);
		e = &j->entries[j->nr];
		memset(e, 0, sizeof(*e));
		if (parse_entry(line, r->hash_algo, e)) {
			warning(_("ignoring malformed line %d in '%s'"),
				lineno, path);
			entry_release(e);
			continue;
		}
		j->nr++;
	}

out:
	strbuf_release(&buf);
	free(path);
	return ret;
}

void provenance_journal_release(struct provenance_journal *j)
{
	for (size_t i = 0; i < j->nr; i++)
		entry_release(&j->entries[i]);
	FREE_AND_NULL(j->entries);
	j->nr = j->alloc = 0;
}

/*
 * Update the entries of `istate` that match `pathspec` from the
 * working tree, the way "git add -A" does.
 */
static int update_from_worktree(struct index_state *istate,
				const struct pathspec *pathspec)
{
	struct string_list paths = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	struct dir_struct dir = DIR_INIT;
	int ret = -1;

	/*
	 * Collect the tracked paths that may have changed first, as
	 * updating an entry can move the others around.  Entries that
	 * preload_index() found unchanged are skipped; submodules and
	 * entries outside a sparse checkout are kept as they are.
	 */
	for (unsigned int i = 0; i < istate->cache_nr; i++) {
		const struct cache_entry *ce = istate->cache[i];

		if (ce_uptodate(ce) || ce_skip_worktree(ce) ||
		    S_ISGITLINK(ce->ce_mode) ||
		    !ce_path_match(istate, ce, pathspec, NULL))
			continue;
		/* the stages of an unmerged path are adjacent */
		if (paths.nr &&
		    !strcmp(paths.items[paths.nr - 1].string, ce->name))
			continue;
		string_list_append(&paths, ce->name);
	}

	for_each_string_list_item(item, &paths) {
		const char *path = item->string;
		struct stat st;

		if (has_symlink_leading_path(path, strlen(path))) {
			/* as if deleted, like "git add -A" */
			remove_file_from_index(istate, path);
		} else if (lstat(path, &st)) {
			if (!is_missing_file_error(errno)) {
				error_errno(_("unable to stat '%s'"), path);
				goto out;
			}
			remove_file_from_index(istate, path);
		} else if (S_ISDIR(st.st_mode)) {
			/* its files are picked up as untracked below */
			remove_file_from_index(istate, path);
		} else if (add_to_index(istate, path, &st, 0)) {
			goto out;
		}
	}

	setup_standard_excludes(&dir);
	fill_directory(&dir, istate, pathspec);
	for (int i = 0; i < dir.nr; i++) {
		const char *path = dir.entries[i]->name;
		struct stat st;

		/*
		 * Skip nested repositories, which "git add" would add as
		 * submodules, and paths outside a sparse checkout.
		 */
		if (ends_with(path, "/") ||
		    !path_in_sparse_checkout(path, istate))
			continue;
		if (lstat(path, &st)) {
			if (is_missing_file_error(errno))
				continue;
			error_errno(_("unable to stat '%s'"), path);
			goto out;
		}
		if (add_to_index(istate, path, &st, 0))
			goto out;
	}
	ret = 0;

out:
	dir_clear(&dir);
	string_list_clear(&paths, 0);
	return ret;
}

int provenance_journal_snapshot(struct repository *r,
				const struct pathspec *pathspec,
				struct object_id *tree)
{
	struct index_state istate = INDEX_STATE_INIT(r);
	struct odb_transaction *transaction = NULL;
	int ret;

	if (read_index_from(&istate, r->index_file, repo_get_git_dir(r)) < 0) {
		ret = error(_("index file corrupt"));
		goto out;
	}
	ensure_full_index(&istate);
	preload_index(&istate, pathspec, 0);

	if (!r->objects->transaction)
		odb_transaction_begin_or_die(r->objects, &transaction, 0);
	ret = update_from_worktree(&istate, pathspec);
	odb_transaction_commit_and_finalize_or_die(transaction);
	if (ret)
		goto out;

	if (cache_tree_update(&istate, WRITE_TREE_SILENT)) {
		ret = error(_("unable to write the working tree as a tree; "
			      "are there unmerged paths?"));
		goto out;
	}
	oidcpy(tree, &istate.cache_tree->oid);

out:
	release_index(&istate);
	return ret;
}
