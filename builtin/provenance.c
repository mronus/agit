/*
 * "git provenance" builtin command
 */

#include "git-compat-util.h"

#include "abspath.h"
#include "builtin.h"
#include "commit.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "object-name.h"
#include "oid-array.h"
#include "pager.h"
#include "parse-options.h"
#include "provenance.h"
#include "provenance-adopt.h"
#include "provenance-remap.h"
#include "provenance-stat.h"
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

#define PROVENANCE_PRUNE_USAGE \
	N_("git provenance prune [--dry-run]")

#define PROVENANCE_STAT_USAGE \
	N_("git provenance stat [--porcelain] [--no-cache] [<revision-range>...]")

#define PROVENANCE_REMAP_USAGE \
	N_("git provenance remap [--dry-run] [<file>]")

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

static const char *stat_sides[] = { "added", "removed" };

static void print_stat_porcelain(const struct provenance_stat *stat)
{
	static const char *keys[] = {
		[PROVENANCE_MARK_AI] = "ai",
		[PROVENANCE_MARK_MIXED] = "mixed",
		[PROVENANCE_MARK_HUMAN] = "human",
		[PROVENANCE_MARK_UNKNOWN] = "unknown",
	};
	const uintmax_t *sides[] = { stat->added, stat->removed };

	printf("commits %"PRIuMAX"\n", stat->commits);
	printf("recorded %"PRIuMAX"\n", stat->recorded);
	for (size_t i = 0; i < ARRAY_SIZE(sides); i++) {
		uintmax_t total = 0;

		for (size_t m = 0; m < ARRAY_SIZE(keys); m++)
			total += sides[i][m];
		printf("%s %"PRIuMAX"\n", stat_sides[i], total);
		for (size_t m = 0; m < ARRAY_SIZE(keys); m++)
			printf("%s.%s %"PRIuMAX"\n", stat_sides[i], keys[m],
			       sides[i][m]);
	}
	printf("share %"PRIuMAX"\n", provenance_stat_share(stat));
}

static uintmax_t percent(uintmax_t n, uintmax_t total)
{
	return total ? (n * 100 + total / 2) / total : 0;
}

static int max_width(uintmax_t a, uintmax_t b)
{
	return decimal_width(a > b ? a : b);
}

static void print_stat(const struct provenance_stat *stat)
{
	const uintmax_t *sides[] = { stat->added, stat->removed };
	uintmax_t total[2] = { 0 };
	uintmax_t share = provenance_stat_share(stat);
	size_t marks = ARRAY_SIZE(stat->added);
	/* the share has a '%' after its digits */
	int width = max_width(stat->commits, share * 10);

	for (size_t i = 0; i < ARRAY_SIZE(sides); i++) {
		for (size_t m = 0; m < marks; m++)
			total[i] += sides[i][m];
		if (decimal_width(total[i]) > width)
			width = decimal_width(total[i]);
	}

	printf("%-7s  %*"PRIuMAX"  (%"PRIuMAX" recorded)\n",
	       "commits", width, stat->commits, stat->recorded);
	for (size_t i = 0; i < ARRAY_SIZE(sides); i++) {
		printf("%-7s  %*"PRIuMAX, stat_sides[i], width, total[i]);
		for (size_t m = 0; m < marks; m++) {
			uintmax_t pct = percent(sides[i][m], total[i]);
			int pct_width = max_width(percent(stat->added[m], total[0]),
						  percent(stat->removed[m], total[1]));

			printf("  %s %*"PRIuMAX" (%"PRIuMAX"%%)",
			       provenance_mark_name(m),
			       max_width(stat->added[m], stat->removed[m]),
			       sides[i][m], pct);
			if (m + 1 < marks)
				printf("%*s", pct_width - decimal_width(pct), "");
		}
		putchar('\n');
	}
	printf("%-7s  %*"PRIuMAX"%%\n", "share", width - 1, share);
}

static int cmd_provenance_stat(int argc, const char **argv, const char *prefix,
			       struct repository *repo)
{
	const char * const stat_usage[] = {
		PROVENANCE_STAT_USAGE,
		NULL,
	};
	int porcelain = 0, use_cache = 1;
	struct option options[] = {
		OPT_BOOL(0, "porcelain", &porcelain,
			 N_("print one 'key value' line per count")),
		OPT_BOOL(0, "cache", &use_cache,
			 N_("use the cache of the line record totals")),
		OPT_END(),
	};
	struct setup_revision_opt opt = { .def = "HEAD" };
	struct provenance_stat stat;
	struct rev_info revs;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, stat_usage,
			     PARSE_OPT_KEEP_ARGV0 | PARSE_OPT_KEEP_UNKNOWN_OPT);

	/* a single commit is its history, as for "git log" */
	repo_init_revisions(repo, &revs, prefix);
	argc = setup_revisions(argc, argv, &revs, &opt);
	if (argc > 1)
		die(_("unrecognized argument: %s"), argv[1]);
	if (revs.prune_data.nr)
		die(_("pathspec arguments not supported"));

	if (provenance_stat_walk(repo, &revs, use_cache, &stat))
		ret = 1;
	else if (porcelain)
		print_stat_porcelain(&stat);
	else
		print_stat(&stat);

	release_revisions(&revs);
	return ret;
}

/*
 * Read the "<old-oid> <new-oid>" lines of `in` into `pairs`, ignoring
 * blank lines and "#" comments.
 */
static int read_pairs(struct repository *repo, FILE *in,
		      struct provenance_remap **pairs, size_t *nr)
{
	struct strbuf line = STRBUF_INIT;
	uintmax_t lineno = 0;
	size_t alloc = 0;
	int ret = 0;

	while (strbuf_getline(&line, in) != EOF) {
		struct provenance_remap *p;
		const char *end;

		lineno++;
		strbuf_trim(&line);
		if (!line.len || line.buf[0] == '#')
			continue;
		ALLOC_GROW(*pairs, *nr + 1, alloc);
		p = &(*pairs)[*nr];
		if (parse_oid_hex_algop(line.buf, &p->old_oid, &end,
					repo->hash_algo) ||
		    !isspace(*end) ||
		    parse_oid_hex_algop(end + strspn(end, " \t"), &p->new_oid,
					&end, repo->hash_algo) ||
		    *end) {
			ret = error(_("line %"PRIuMAX": expected '<old-oid> <new-oid>': %s"),
				    lineno, line.buf);
			break;
		}
		(*nr)++;
	}

	strbuf_release(&line);
	return ret;
}

static int cmd_provenance_remap(int argc, const char **argv, const char *prefix,
				struct repository *repo)
{
	const char * const remap_usage[] = {
		PROVENANCE_REMAP_USAGE,
		NULL,
	};
	int dry_run = 0;
	struct option options[] = {
		OPT__DRY_RUN(&dry_run, N_("show what would be moved")),
		OPT_END(),
	};
	static const char *words[] = {
		[PROVENANCE_REMAP_MOVED] = "moved",
		[PROVENANCE_REMAP_REATTACHED] = "re-attached",
		[PROVENANCE_REMAP_NOT_COMMIT] = "skipped: not a commit",
		[PROVENANCE_REMAP_HAS_RECORD] = "skipped: has a line record",
		[PROVENANCE_REMAP_BAD_RECORD] = "skipped: unparsable record",
	};
	struct provenance_remap *pairs = NULL;
	size_t nr = 0;
	FILE *in = stdin;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, remap_usage, 0);
	if (argc > 1)
		usage_with_options(remap_usage, options);

	if (argc) {
		char *path = prefix_filename(prefix, argv[0]);

		in = xfopen(path, "r");
		free(path);
	}
	if (read_pairs(repo, in, &pairs, &nr) ||
	    provenance_remap(repo, pairs, nr, dry_run))
		ret = 1;
	if (in != stdin)
		fclose(in);

	for (size_t i = 0; !ret && i < nr; i++) {
		if (pairs[i].result == PROVENANCE_REMAP_NONE)
			continue;
		printf("%s ", oid_to_hex(&pairs[i].old_oid));
		printf("%s %s\n", oid_to_hex(&pairs[i].new_oid),
		       words[pairs[i].result]);
	}

	free(pairs);
	return ret;
}

static int print_pruned(const struct object_id *oid, void *data UNUSED)
{
	printf("%s pruned\n", oid_to_hex(oid));
	return 0;
}

static int cmd_provenance_prune(int argc, const char **argv, const char *prefix,
				struct repository *repo)
{
	const char * const prune_usage[] = {
		PROVENANCE_PRUNE_USAGE,
		NULL,
	};
	int dry_run = 0;
	struct option options[] = {
		OPT__DRY_RUN(&dry_run, N_("show what would be pruned")),
		OPT_END(),
	};
	struct oid_array pruned = OID_ARRAY_INIT;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, prune_usage, 0);
	if (argc)
		usage_with_options(prune_usage, options);

	if (provenance_prune(repo, dry_run, &pruned))
		ret = 1;
	else
		oid_array_for_each_unique(&pruned, print_pruned, NULL);

	oid_array_clear(&pruned);
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
		PROVENANCE_REMAP_USAGE,
		PROVENANCE_PRUNE_USAGE,
		PROVENANCE_STAT_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("show", &fn, cmd_provenance_show),
		OPT_SUBCOMMAND("verify", &fn, cmd_provenance_verify),
		OPT_SUBCOMMAND("adopt", &fn, cmd_provenance_adopt),
		OPT_SUBCOMMAND("remap", &fn, cmd_provenance_remap),
		OPT_SUBCOMMAND("prune", &fn, cmd_provenance_prune),
		OPT_SUBCOMMAND("stat", &fn, cmd_provenance_stat),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, provenance_usage, 0);

	repo_config(repo, git_default_config, NULL);

	return fn(argc, argv, prefix, repo);
}
