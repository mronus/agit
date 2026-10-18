#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "provenance.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"

/*
 * "test-tool provenance format" parses a line record from stdin and
 * prints it as formatted by provenance_record_format();
 * "test-tool provenance format-reasoning" does the same for a
 * reasoning record.
 */
int cmd__provenance(int argc, const char **argv)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct provenance_reasoning reasoning = PROVENANCE_REASONING_INIT;
	struct strbuf in = STRBUF_INIT, out = STRBUF_INIT, err = STRBUF_INIT;
	int is_reasoning;

	if (argc != 2 ||
	    (strcmp(argv[1], "format") && strcmp(argv[1], "format-reasoning")))
		die("usage: test-tool provenance (format | format-reasoning)");
	is_reasoning = !strcmp(argv[1], "format-reasoning");

	setup_git_directory(the_repository);

	if (strbuf_read(&in, 0, 0) < 0)
		die_errno("could not read stdin");
	if (is_reasoning) {
		if (provenance_reasoning_parse(in.buf, in.len,
					       the_repository->hash_algo,
					       &reasoning, &err))
			die("%s", err.buf);
		provenance_reasoning_format(&reasoning, &out);
	} else {
		if (provenance_record_parse(in.buf, in.len,
					    the_repository->hash_algo,
					    &rec, &err))
			die("%s", err.buf);
		provenance_record_format(&rec, &out);
	}
	fwrite(out.buf, 1, out.len, stdout);

	provenance_record_release(&rec);
	provenance_reasoning_release(&reasoning);
	strbuf_release(&in);
	strbuf_release(&out);
	strbuf_release(&err);
	return 0;
}
