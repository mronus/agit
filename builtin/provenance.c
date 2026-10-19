/*
 * "git provenance" builtin command
 */

#include "git-compat-util.h"

#include "advice.h"
#include "builtin.h"
#include "commit.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "object-name.h"
#include "oid-array.h"
#include "parse-options.h"
#include "provenance.h"
#include "provenance-adopt.h"
#include "provenance-purge.h"
#include "quote.h"
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

#define PROVENANCE_PURGE_USAGE \
	N_("git provenance purge [--dry-run] <revision-range>..."), \
	N_("git provenance purge [--dry-run] --session <id>... [<revision-range>...]"), \
	N_("git provenance purge [--dry-run] [-E | -F] [-i] --redact <pattern>\n" \
	   "                     [<revision-range>...]")

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

static int cmd_provenance_purge(int argc, const char **argv, const char *prefix,
				struct repository *repo)
{
	const char * const purge_usage[] = {
		PROVENANCE_PURGE_USAGE,
		NULL,
	};
	enum { PATTERN_BASIC, PATTERN_EXTENDED, PATTERN_FIXED };
	struct string_list sessions = STRING_LIST_INIT_NODUP;
	const char *redact = NULL;
	int dry_run = 0, pattern_type = PATTERN_BASIC, icase = 0;
	struct option options[] = {
		OPT__DRY_RUN(&dry_run, N_("show what would be purged")),
		OPT_STRING_LIST(0, "session", &sessions, N_("id"),
				N_("drop the session <id> from the records")),
		OPT_STRING(0, "redact", &redact, N_("pattern"),
			   N_("replace the matches of <pattern> in the summaries")),
		OPT_SET_INT('E', "extended-regexp", &pattern_type,
			    N_("<pattern> is an extended regular expression"),
			    PATTERN_EXTENDED),
		OPT_SET_INT('F', "fixed-strings", &pattern_type,
			    N_("<pattern> is a fixed string"), PATTERN_FIXED),
		OPT_BOOL('i', "regexp-ignore-case", &icase,
			 N_("match <pattern> ignoring case")),
		OPT_END(),
	};
	static const char *words[] = {
		[PROVENANCE_PURGE_REMOVED] = "removed",
		[PROVENANCE_PURGE_REDACTED] = "redacted",
		[PROVENANCE_PURGE_SESSION_DROPPED] = "session dropped",
	};
	struct provenance_purge_opts opts = { 0 };
	struct provenance_purged *purged = NULL;
	struct oid_array commits = OID_ARRAY_INIT;
	size_t purged_nr = 0;
	regex_t pattern;
	int ranges, ret = 0;

	argc = parse_options(argc, argv, prefix, options, purge_usage,
			     PARSE_OPT_KEEP_ARGV0 | PARSE_OPT_KEEP_UNKNOWN_OPT);
	ranges = argc > 1;
	die_for_incompatible_opt2(sessions.nr > 0, "--session",
				  !!redact, "--redact");
	if (!redact && pattern_type != PATTERN_BASIC)
		die(_("the option '%s' requires '%s'"),
		    pattern_type == PATTERN_FIXED ? "-F" : "-E", "--redact");
	if (!redact && icase)
		die(_("the option '%s' requires '%s'"), "-i", "--redact");
	if (!ranges && !sessions.nr && !redact)
		usage_with_options(purge_usage, options);

	opts.dry_run = dry_run;
	if (sessions.nr)
		opts.sessions = &sessions;
	if (redact) {
		struct strbuf quoted = STRBUF_INIT;
		int flags = REG_NEWLINE;

		if (pattern_type == PATTERN_FIXED) {
			basic_regex_quote_buf(&quoted, redact);
			redact = quoted.buf;
		} else if (pattern_type == PATTERN_EXTENDED) {
			flags |= REG_EXTENDED;
		}
		if (icase)
			flags |= REG_ICASE;
		if (regcomp(&pattern, redact, flags))
			die(_("invalid regular expression: %s"), redact);
		strbuf_release(&quoted);
		opts.pattern = &pattern;
	}

	if (ranges) {
		struct rev_info revs;
		struct commit *commit;

		/* a single commit is that commit, as for "adopt" */
		repo_init_revisions(repo, &revs, prefix);
		revs.no_walk = 1;
		revs.topo_order = 1;
		revs.reverse = 1;
		argc = setup_revisions(argc, argv, &revs, NULL);
		if (argc > 1)
			die(_("unrecognized argument: %s"), argv[1]);
		if (prepare_revision_walk(&revs))
			die(_("revision walk setup failed"));
		while ((commit = get_revision(&revs)))
			oid_array_append(&commits, &commit->object.oid);
		release_revisions(&revs);
	}

	if (provenance_purge(repo, &opts, ranges ? &commits : NULL,
			     &purged, &purged_nr))
		ret = 1;
	for (size_t i = 0; i < purged_nr; i++) {
		printf("%s %s", oid_to_hex(&purged[i].commit),
		       words[purged[i].result]);
		if (purged[i].result == PROVENANCE_PURGE_REDACTED)
			printf(" %"PRIuMAX, (uintmax_t)purged[i].matches);
		putchar('\n');
	}
	if (!ret && !purged_nr)
		fprintf_ln(stderr, _("nothing to purge"));
	else if (!ret && !dry_run)
		advise_if_enabled(ADVICE_PROVENANCE_PURGE,
				  _("To replace the reasoning records of a remote with these, use\n"
				    "'git push --force <remote> refs/provenance/reasoning'. Then,\n"
				    "to drop the purged records from this repository, use\n"
				    "'git reflog expire --expire=now refs/provenance/reasoning'\n"
				    "and 'git gc --prune=now'."));

	if (opts.pattern)
		regfree(&pattern);
	string_list_clear(&sessions, 0);
	oid_array_clear(&commits);
	free(purged);
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
		PROVENANCE_PURGE_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("show", &fn, cmd_provenance_show),
		OPT_SUBCOMMAND("verify", &fn, cmd_provenance_verify),
		OPT_SUBCOMMAND("adopt", &fn, cmd_provenance_adopt),
		OPT_SUBCOMMAND("purge", &fn, cmd_provenance_purge),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, provenance_usage, 0);

	repo_config(repo, git_default_config, NULL);

	return fn(argc, argv, prefix, repo);
}
