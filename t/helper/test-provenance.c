#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "provenance.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"

/*
 * "test-tool provenance format" parses a line record from stdin and
 * prints it as formatted by provenance_record_format().
 */
int cmd__provenance(int argc, const char **argv)
{
	struct provenance_record rec = PROVENANCE_RECORD_INIT;
	struct strbuf in = STRBUF_INIT, out = STRBUF_INIT, err = STRBUF_INIT;

	if (argc != 2 || strcmp(argv[1], "format"))
		die("usage: test-tool provenance format");

	setup_git_directory(the_repository);

	if (strbuf_read(&in, 0, 0) < 0)
		die_errno("could not read stdin");
	if (provenance_record_parse(in.buf, in.len, the_repository->hash_algo,
				    &rec, &err))
		die("%s", err.buf);
	provenance_record_format(&rec, &out);
	fwrite(out.buf, 1, out.len, stdout);

	provenance_record_release(&rec);
	strbuf_release(&in);
	strbuf_release(&out);
	strbuf_release(&err);
	return 0;
}
