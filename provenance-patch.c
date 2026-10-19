/*
 * The line record of a commit in a patch mail.
 */

#include "git-compat-util.h"
#include "provenance-patch.h"
#include "diffcore.h"
#include "provenance.h"
#include "strbuf.h"

#define BLOCK_HEADER "Provenance:"
#define BLOCK_INDENT "    "

/* Whether the patch in `q` shows `path`. */
static int shows_path(const struct diff_queue_struct *q, const char *path)
{
	for (int i = 0; i < q->nr; i++)
		if (!strcmp(q->queue[i]->one->path, path) ||
		    !strcmp(q->queue[i]->two->path, path))
			return 1;
	return 0;
}

int provenance_patch_format(struct repository *r,
			    const struct object_id *commit,
			    const struct diff_queue_struct *q,
			    struct strbuf *out)
{
	struct provenance_cache cache;
	const struct provenance_record *rec;
	int ret = 0;

	provenance_cache_init(&cache, r);
	rec = provenance_cache_get(&cache, commit);
	if (rec) {
		/* the same record, with the blocks the patch shows */
		struct provenance_record shown = *rec;
		struct strbuf buf = STRBUF_INIT;
		const char *p;

		ALLOC_ARRAY(shown.files, rec->files_nr);
		shown.files_nr = 0;
		for (size_t i = 0; i < rec->files_nr; i++)
			if (shows_path(q, rec->files[i].path))
				shown.files[shown.files_nr++] = rec->files[i];

		if (shown.files_nr) {
			provenance_record_format(&shown, &buf);
			strbuf_addstr(out, BLOCK_HEADER "\n");
			for (p = buf.buf; *p; ) {
				const char *eol = strchrnul(p, '\n');

				strbuf_addf(out, BLOCK_INDENT "%.*s\n",
					    (int)(eol - p), p);
				p = *eol ? eol + 1 : eol;
			}
			ret = 1;
		}
		free(shown.files);
		strbuf_release(&buf);
	}
	provenance_cache_release(&cache);
	return ret;
}
