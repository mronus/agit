/*
 * "git stamp" builtin command
 */

#include "git-compat-util.h"

#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "object-file.h"
#include "odb.h"
#include "parse-options.h"
#include "pathspec.h"
#include "provenance-journal.h"
#include "read-cache-ll.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"

#define STAMP_BEGIN_USAGE \
	N_("git stamp begin --session <id> [--model <model>] [--tool <tool>] [--] [<pathspec>...]")

#define STAMP_END_USAGE \
	N_("git stamp end --session <id> [--] [<pathspec>...]")

#define STAMP_EDIT_USAGE \
	N_("git stamp edit --session <id> --path <path> --pre <file> [--post <file>]")

#define STAMP_REASON_USAGE \
	N_("git stamp reason --session <id> --summary <file> [--transcript <file>]")

#define STAMP_CLAIM_USAGE \
	N_("git stamp claim --session <id> (--patch <file> | [--] <path>...)")

#define STAMP_LIST_USAGE \
	N_("git stamp list [--session <id>]")

#define OPT_SESSION(v) \
	OPT_STRING(0, "session", (v), N_("id"), N_("the agent session"))

static void check_session(const char *session,
			  const char * const *usagestr,
			  const struct option *options)
{
	if (!session) {
		error(_("option '%s' is required"), "--session");
		usage_with_options(usagestr, options);
	}
	if (!provenance_session_is_valid(session))
		die(_("invalid session id '%s'"), session);
}

static int append(struct repository *repo,
		  struct provenance_journal_entry *e,
		  const char *session)
{
	e->time = time(NULL);
	e->session = (char *)session;
	provenance_journal_head(repo, &e->head);
	return provenance_journal_append(repo, e) ? 1 : 0;
}

/*
 * Write a snapshot of the paths matching `argv` (the whole working
 * tree if there are none) into e->oid.
 */
static void snapshot(struct repository *repo, const char *prefix,
		     const char **argv, struct provenance_journal_entry *e)
{
	struct pathspec pathspec;

	parse_pathspec(&pathspec, 0,
		       PATHSPEC_PREFER_FULL | PATHSPEC_SYMLINK_LEADING_PATH,
		       prefix, argv);
	if (provenance_journal_snapshot(repo, &pathspec, &e->oid))
		die(_("unable to take a snapshot of the working tree"));
	clear_pathspec(&pathspec);
}

/*
 * Hash the contents of `file` as a blob, converting it as "git add"
 * would convert the working tree file at `path`.
 */
static void hash_file(struct repository *repo, const char *file,
		      const char *path, struct object_id *oid)
{
	struct stat st;
	int fd = xopen(file, O_RDONLY);

	if (fstat(fd, &st) < 0)
		die_errno(_("unable to stat '%s'"), file);
	if (index_fd(repo->index, oid, fd, &st, OBJ_BLOB, path,
		     INDEX_WRITE_OBJECT))
		die(_("unable to hash '%s'"), file);
}

/*
 * Store the contents of `file`, or of the standard input if it is "-"
 * and `allow_stdin` is set, as a blob.
 */
static void store_file(struct repository *repo, const char *file,
		       int allow_stdin, struct object_id *oid)
{
	struct strbuf buf = STRBUF_INIT;

	if (allow_stdin && !strcmp(file, "-")) {
		if (strbuf_read(&buf, 0, 0) < 0)
			die_errno(_("unable to read from standard input"));
	} else if (strbuf_read_file(&buf, file, 0) < 0) {
		die_errno(_("unable to read '%s'"), file);
	}
	if (odb_write_object(repo->objects, buf.buf, buf.len, OBJ_BLOB, oid))
		die(_("unable to write '%s' to the object database"), file);
	strbuf_release(&buf);
}

/* Turn a path given on the command line into one relative to the top. */
static char *full_path(struct repository *repo, const char *prefix,
		       const char *path)
{
	return prefix_path(repo, prefix, prefix ? strlen(prefix) : 0, path);
}

static int cmd_stamp_begin(int argc, const char **argv, const char *prefix,
			   struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_BEGIN_USAGE,
		NULL,
	};
	const char *session = NULL, *model = NULL, *tool = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_STRING(0, "model", &model, N_("model"),
			   N_("the model of the agent")),
		OPT_STRING(0, "tool", &tool, N_("tool"),
			   N_("the tool running the agent")),
		OPT_END(),
	};
	struct provenance_journal_entry e = {
		.type = PROVENANCE_JOURNAL_BEGIN,
	};

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	check_session(session, usagestr, options);

	e.model = (char *)model;
	e.tool = (char *)tool;
	snapshot(repo, prefix, argv, &e);
	return append(repo, &e, session);
}

static int cmd_stamp_end(int argc, const char **argv, const char *prefix,
			 struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_END_USAGE,
		NULL,
	};
	const char *session = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_END(),
	};
	struct provenance_journal_entry e = {
		.type = PROVENANCE_JOURNAL_END,
	};

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	check_session(session, usagestr, options);

	snapshot(repo, prefix, argv, &e);
	return append(repo, &e, session);
}

static int cmd_stamp_edit(int argc, const char **argv, const char *prefix,
			  struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_EDIT_USAGE,
		NULL,
	};
	const char *session = NULL, *path = NULL;
	char *pre = NULL, *post = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_STRING(0, "path", &path, N_("path"),
			   N_("the path of the edited file")),
		OPT_FILENAME(0, "pre", &pre,
			     N_("file with the content before the edit")),
		OPT_FILENAME(0, "post", &post,
			     N_("file with the content after the edit")),
		OPT_END(),
	};
	struct provenance_journal_entry e = {
		.type = PROVENANCE_JOURNAL_EDIT,
	};
	int ret;

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	check_session(session, usagestr, options);
	if (argc || !path || !pre)
		usage_with_options(usagestr, options);

	e.path = full_path(repo, prefix, path);
	if (strchr(e.path, '\n'))
		die(_("path '%s' contains a newline"), path);

	if (repo_read_index(repo) < 0)
		die(_("index file corrupt"));
	hash_file(repo, pre, e.path, &e.oid);
	if (post) {
		hash_file(repo, post, e.path, &e.oid2);
	} else {
		struct stat st;

		if (lstat(e.path, &st))
			die_errno(_("unable to stat '%s'"), e.path);
		if (index_path(repo->index, &e.oid2, e.path, &st,
			       INDEX_WRITE_OBJECT))
			die(_("unable to hash '%s'"), e.path);
	}

	ret = append(repo, &e, session);
	free(e.path);
	free(pre);
	free(post);
	return ret;
}

static int cmd_stamp_reason(int argc, const char **argv, const char *prefix,
			    struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_REASON_USAGE,
		NULL,
	};
	const char *session = NULL;
	char *summary = NULL, *transcript = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_FILENAME(0, "summary", &summary,
			     N_("file with the summary, or '-' for the standard input")),
		OPT_FILENAME(0, "transcript", &transcript,
			     N_("file with the transcript of the session")),
		OPT_END(),
	};
	struct provenance_journal_entry e = {
		.type = PROVENANCE_JOURNAL_REASON,
	};
	int ret;

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	check_session(session, usagestr, options);
	if (argc || !summary)
		usage_with_options(usagestr, options);

	store_file(repo, summary, 1, &e.oid);
	if (transcript)
		store_file(repo, transcript, 0, &e.oid2);
	ret = append(repo, &e, session);
	free(summary);
	free(transcript);
	return ret;
}

static int cmd_stamp_claim(int argc, const char **argv, const char *prefix,
			   struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_CLAIM_USAGE,
		NULL,
	};
	const char *session = NULL;
	char *patch = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_FILENAME(0, "patch", &patch,
			     N_("file with the patch to claim")),
		OPT_END(),
	};
	char **paths;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	check_session(session, usagestr, options);
	if (!patch == !argc)
		usage_with_options(usagestr, options);

	if (patch) {
		struct provenance_journal_entry e = {
			.type = PROVENANCE_JOURNAL_CLAIM_PATCH,
		};

		store_file(repo, patch, 0, &e.oid);
		ret = append(repo, &e, session);
		free(patch);
		return ret;
	}

	/* check all paths before recording any of them */
	CALLOC_ARRAY(paths, argc);
	for (int i = 0; i < argc; i++) {
		paths[i] = full_path(repo, prefix, argv[i]);
		if (strchr(paths[i], '\n'))
			die(_("path '%s' contains a newline"), argv[i]);
	}
	for (int i = 0; i < argc && !ret; i++) {
		struct provenance_journal_entry e = {
			.type = PROVENANCE_JOURNAL_CLAIM_PATH,
			.path = paths[i],
		};

		ret = append(repo, &e, session);
	}
	for (int i = 0; i < argc; i++)
		free(paths[i]);
	free(paths);
	return ret;
}

static int cmd_stamp_list(int argc, const char **argv, const char *prefix,
			  struct repository *repo)
{
	const char * const usagestr[] = {
		STAMP_LIST_USAGE,
		NULL,
	};
	const char *session = NULL;
	struct option options[] = {
		OPT_SESSION(&session),
		OPT_END(),
	};
	struct provenance_journal j = PROVENANCE_JOURNAL_INIT;
	struct strbuf buf = STRBUF_INIT;

	argc = parse_options(argc, argv, prefix, options, usagestr, 0);
	if (argc)
		usage_with_options(usagestr, options);

	if (provenance_journal_read(repo, &j))
		return 1;
	for (size_t i = 0; i < j.nr; i++) {
		if (session && strcmp(session, j.entries[i].session))
			continue;
		strbuf_reset(&buf);
		provenance_journal_entry_format(&j.entries[i], &buf);
		puts(buf.buf);
	}
	strbuf_release(&buf);
	provenance_journal_release(&j);
	return 0;
}

int cmd_stamp(int argc,
	      const char **argv,
	      const char *prefix,
	      struct repository *repo)
{
	const char * const stamp_usage[] = {
		STAMP_BEGIN_USAGE,
		STAMP_END_USAGE,
		STAMP_EDIT_USAGE,
		STAMP_REASON_USAGE,
		STAMP_CLAIM_USAGE,
		STAMP_LIST_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("begin", &fn, cmd_stamp_begin),
		OPT_SUBCOMMAND("end", &fn, cmd_stamp_end),
		OPT_SUBCOMMAND("edit", &fn, cmd_stamp_edit),
		OPT_SUBCOMMAND("reason", &fn, cmd_stamp_reason),
		OPT_SUBCOMMAND("claim", &fn, cmd_stamp_claim),
		OPT_SUBCOMMAND("list", &fn, cmd_stamp_list),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, stamp_usage, 0);

	repo_config(repo, git_default_config, NULL);

	return fn(argc, argv, prefix, repo);
}
