/*
 * "git provenance" builtin command
 */

#include "git-compat-util.h"

#include "builtin.h"
#include "commit.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "object-name.h"
#include "parse-options.h"
#include "provenance.h"
#include "repository.h"
#include "strbuf.h"
#include "write-or-die.h"

#define PROVENANCE_SHOW_USAGE \
	N_("git provenance show [--reasoning] <commit>")

#define PROVENANCE_VERIFY_USAGE \
	N_("git provenance verify [<commit>...]")

static struct commit *lookup_commit_arg(struct repository *repo,
					const char *arg)
{
	struct object_id oid;
	struct commit *commit;

	if (repo_get_oid_committish(repo, arg, &oid))
		die(_("not a valid commit: '%s'"), arg);
	commit = lookup_commit_reference(repo, &oid);
	if (!commit)
		die(_("not a valid commit: '%s'"), arg);
	return commit;
}

static int cmd_provenance_show(int argc, const char **argv, const char *prefix,
			       struct repository *repo)
{
	const char * const show_usage[] = {
		PROVENANCE_SHOW_USAGE,
		NULL,
	};
	int reasoning = 0;
	struct option options[] = {
		OPT_BOOL(0, "reasoning", &reasoning,
			 N_("show the reasoning record instead of the line record")),
		OPT_END(),
	};
	struct strbuf buf = STRBUF_INIT;
	struct commit *commit;
	int ret;

	argc = parse_options(argc, argv, prefix, options, show_usage, 0);
	if (argc != 1)
		usage_with_options(show_usage, options);

	commit = lookup_commit_arg(repo, argv[0]);
	if (reasoning)
		ret = provenance_read_reasoning(repo, &commit->object.oid, &buf);
	else
		ret = provenance_read_lines(repo, &commit->object.oid, &buf);

	if (!ret)
		error(_("no provenance record for '%s'"),
		      oid_to_hex(&commit->object.oid));
	else if (ret > 0)
		write_or_die(1, buf.buf, buf.len);

	strbuf_release(&buf);
	return ret > 0 ? 0 : 1;
}

static int verify_one(struct repository *repo, struct commit *commit)
{
	const char *hex = oid_to_hex(&commit->object.oid);
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;
	int ret;

	ret = provenance_read_lines(repo, &commit->object.oid, &buf);
	if (ret < 0)
		goto out;
	if (!ret) {
		printf("%s no record\n", hex);
		goto out;
	}

	if (provenance_record_parse(buf.buf, buf.len, repo->hash_algo,
				    &rec, &err) ||
	    provenance_verify(repo, commit, &rec, &err)) {
		ret = error(_("provenance of '%s' does not verify: %s"),
			    hex, err.buf);
		goto out;
	}
	printf("%s ok\n", hex);
	ret = 0;

out:
	provenance_record_release(&rec);
	strbuf_release(&buf);
	strbuf_release(&err);
	return ret;
}

static int cmd_provenance_verify(int argc, const char **argv, const char *prefix,
				 struct repository *repo)
{
	const char * const verify_usage[] = {
		PROVENANCE_VERIFY_USAGE,
		NULL,
	};
	struct option options[] = {
		OPT_END(),
	};
	const char *head[] = { "HEAD" };
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, verify_usage, 0);
	if (!argc) {
		argc = 1;
		argv = head;
	}

	for (int i = 0; i < argc; i++) {
		struct commit *commit = lookup_commit_arg(repo, argv[i]);

		if (verify_one(repo, commit) < 0)
			ret = 1;
		fflush(stdout);
	}
	return ret;
}

int cmd_provenance(int argc,
		   const char **argv,
		   const char *prefix,
		   struct repository *repo)
{
	const char * const provenance_usage[] = {
		PROVENANCE_SHOW_USAGE,
		PROVENANCE_VERIFY_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("show", &fn, cmd_provenance_show),
		OPT_SUBCOMMAND("verify", &fn, cmd_provenance_verify),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, provenance_usage, 0);

	repo_config(repo, git_default_config, NULL);

	return fn(argc, argv, prefix, repo);
}
