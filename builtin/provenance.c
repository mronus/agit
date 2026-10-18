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
#include "provenance-adopt.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "string-list.h"
#include "write-or-die.h"

#define PROVENANCE_SHOW_USAGE \
	N_("git provenance show [--reasoning] <commit>")

#define PROVENANCE_VERIFY_USAGE \
	N_("git provenance verify [<commit>...]")

#define PROVENANCE_ADOPT_USAGE \
	N_("git provenance adopt [--from <rev>]... [-f | --force] <revision-range>...")

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

static int cmd_provenance_adopt(int argc, const char **argv, const char *prefix,
				struct repository *repo)
{
	const char * const adopt_usage[] = {
		PROVENANCE_ADOPT_USAGE,
		NULL,
	};
	struct string_list from_args = STRING_LIST_INIT_NODUP;
	int force = 0;
	struct option options[] = {
		OPT_STRING_LIST(0, "from", &from_args, N_("rev"),
				N_("only take records from commits that <rev> can reach")),
		OPT__FORCE(&force, N_("replace records that were adopted"), 0),
		OPT_END(),
	};
	static const char *words[] = {
		[PROVENANCE_ADOPT_ADOPTED] = "adopted",
		[PROVENANCE_ADOPT_NO_SOURCE] = "no source",
		[PROVENANCE_ADOPT_HAS_RECORD] = "has record",
		[PROVENANCE_ADOPT_MERGE] = "merge",
	};
	struct provenance_adopt pa;
	struct rev_info revs;
	struct commit **targets = NULL, **from = NULL;
	size_t targets_nr = 0, targets_alloc = 0;
	struct commit *commit;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, adopt_usage,
			     PARSE_OPT_KEEP_ARGV0 | PARSE_OPT_KEEP_UNKNOWN_OPT);
	if (argc < 2)
		usage_with_options(adopt_usage, options);

	/* a single commit is that commit, as for "git cherry-pick" */
	repo_init_revisions(repo, &revs, prefix);
	revs.no_walk = 1;
	revs.topo_order = 1;
	revs.reverse = 1;
	argc = setup_revisions(argc, argv, &revs, NULL);
	if (argc > 1)
		die(_("unrecognized argument: %s"), argv[1]);
	if (prepare_revision_walk(&revs))
		die(_("revision walk setup failed"));
	while ((commit = get_revision(&revs))) {
		ALLOC_GROW(targets, targets_nr + 1, targets_alloc);
		targets[targets_nr++] = commit;
	}
	release_revisions(&revs);

	ALLOC_ARRAY(from, from_args.nr);
	for (size_t i = 0; i < from_args.nr; i++)
		from[i] = lookup_commit_arg(repo, from_args.items[i].string);

	provenance_adopt_init(&pa, repo, targets, targets_nr,
			      from, from_args.nr);
	for (size_t i = 0; i < targets_nr; i++) {
		enum provenance_adopt_result result;

		if (provenance_adopt_commit(&pa, targets[i], force, &result))
			ret = 1;
		else
			printf("%s %s\n", oid_to_hex(&targets[i]->object.oid),
			       words[result]);
		fflush(stdout);
	}

	provenance_adopt_release(&pa);
	string_list_clear(&from_args, 0);
	free(targets);
	free(from);
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
		PROVENANCE_ADOPT_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("show", &fn, cmd_provenance_show),
		OPT_SUBCOMMAND("verify", &fn, cmd_provenance_verify),
		OPT_SUBCOMMAND("adopt", &fn, cmd_provenance_adopt),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, provenance_usage, 0);

	repo_config(repo, git_default_config, NULL);

	return fn(argc, argv, prefix, repo);
}
