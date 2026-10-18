#include "git-compat-util.h"
#include "provenance.h"
#include "commit.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "object.h"
#include "object-file.h"
#include "odb.h"
#include "parse.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"
#include "tag.h"
#include "tree-walk.h"

static const char *mark_names[] = {
	[PROVENANCE_MARK_AI] = "AI",
	[PROVENANCE_MARK_MIXED] = "MIXED",
	[PROVENANCE_MARK_HUMAN] = "HUMAN",
	[PROVENANCE_MARK_UNKNOWN] = "UNKNOWN",
};

static int parse_mark(const char *s, size_t len, enum provenance_mark *mark)
{
	for (size_t i = 0; i < ARRAY_SIZE(mark_names); i++) {
		if (strlen(mark_names[i]) == len &&
		    !memcmp(s, mark_names[i], len)) {
			*mark = i;
			return 0;
		}
	}
	return -1;
}

__attribute__((format (printf, 3, 4)))
static int parse_error(struct strbuf *err, int lineno, const char *fmt, ...)
{
	va_list ap;

	strbuf_addf(err, _("line %d: "), lineno);
	va_start(ap, fmt);
	strbuf_vaddf(err, fmt, ap);
	va_end(ap);
	return -1;
}

/*
 * Parse "<key>=<value>" at *p, where <value> contains no space.  The
 * field must be followed by a single space, or by the end of the line
 * if it is the last one.  Returns the value, or NULL if it is malformed.
 */
static char *parse_field(const char **p, const char *key, int last)
{
	const char *v, *e;

	if (!skip_prefix(*p, key, &v) || *v++ != '=')
		return NULL;
	e = strchrnul(v, ' ');
	if (e == v || (last ? *e != '\0' : *e != ' '))
		return NULL;
	*p = last ? e : e + 1;
	return xstrndup(v, e - v);
}

static int parse_actor(struct provenance_record *rec, const char *p,
		       int lineno, struct strbuf *err)
{
	struct provenance_actor *actor;
	const char *sp = strchr(p, ' ');

	if (!sp || sp == p)
		return parse_error(err, lineno, _("malformed actor line"));
	if (memchr(p, ',', sp - p) || (sp - p == 1 && *p == '-'))
		return parse_error(err, lineno, _("invalid actor id '%.*s'"),
				   (int)(sp - p), p);
	for (size_t i = 0; i < rec->actors_nr; i++)
		if (!xstrncmpz(rec->actors[i].id, p, sp - p))
			return parse_error(err, lineno,
					   _("duplicate actor id '%.*s'"),
					   (int)(sp - p), p);

	ALLOC_GROW(rec->actors, rec->actors_nr + 1, rec->actors_alloc);
	actor = &rec->actors[rec->actors_nr++];
	memset(actor, 0, sizeof(*actor));
	actor->id = xstrndup(p, sp - p);
	p = sp + 1;

	if (skip_prefix(p, "ai ", &p)) {
		actor->kind = PROVENANCE_ACTOR_AI;
		if (!(actor->model = parse_field(&p, "model", 0)) ||
		    !(actor->tool = parse_field(&p, "tool", 0)) ||
		    !(actor->session = parse_field(&p, "session", 0)) ||
		    !(actor->source = parse_field(&p, "source", 1)))
			return parse_error(err, lineno, _("malformed ai actor '%s'"),
					   actor->id);
		if (strcmp(actor->source, "observed") &&
		    strcmp(actor->source, "declared"))
			return parse_error(err, lineno, _("unknown actor source '%s'"),
					   actor->source);
	} else if (skip_prefix(p, "human ident=", &p) && *p) {
		actor->kind = PROVENANCE_ACTOR_HUMAN;
		actor->ident = xstrdup(p);
	} else {
		return parse_error(err, lineno, _("malformed actor '%s'"),
				   actor->id);
	}
	return 0;
}

/* "<tool>/<version>": opaque, non-empty on both sides, without spaces */
static int is_tool_token(const char *p)
{
	const char *slash = strchr(p, '/');

	return slash && slash != p && slash[1] && !strchr(p, ' ');
}

static int parse_lineno(const char **p, unsigned long *out)
{
	char *end;

	/* decimal, without leading zeros */
	if (!isdigit(**p) || (**p == '0' && isdigit((*p)[1])))
		return -1;
	errno = 0;
	*out = strtoul(*p, &end, 10);
	if (errno)
		return -1;
	*p = end;
	return 0;
}

static int parse_range(struct provenance_file *file, const char *line,
		       const struct git_hash_algo *algop,
		       int lineno, struct strbuf *err)
{
	struct provenance_range *range;
	const char *p = line + 2, *e;

	ALLOC_GROW(file->ranges, file->ranges_nr + 1, file->ranges_alloc);
	range = &file->ranges[file->ranges_nr++];
	memset(range, 0, sizeof(*range));
	range->side = line[0];

	if (parse_lineno(&p, &range->begin))
		return parse_error(err, lineno, _("malformed line range"));
	range->end = range->begin;
	if (*p == '-') {
		p++;
		if (parse_lineno(&p, &range->end))
			return parse_error(err, lineno, _("malformed line range"));
	}
	if (*p++ != ' ')
		return parse_error(err, lineno, _("malformed line range"));

	e = strchr(p, ' ');
	if (!e)
		return parse_error(err, lineno, _("missing mark"));
	if (parse_mark(p, e - p, &range->mark))
		return parse_error(err, lineno, _("unknown mark '%.*s'"),
				   (int)(e - p), p);
	p = e + 1;

	e = strchr(p, ' ');
	if (!e || e == p)
		return parse_error(err, lineno, _("missing actor list"));
	range->actors = xstrndup(p, e - p);
	p = e + 1;

	if (parse_oid_hex_algop(p, &range->anchor, &p, algop) || *p)
		return parse_error(err, lineno, _("malformed anchor"));
	return 0;
}

int provenance_record_parse(const char *buf, size_t len,
			    const struct git_hash_algo *algop,
			    struct provenance_record *rec,
			    struct strbuf *err)
{
	const char *end = buf + len;
	struct strbuf line = STRBUF_INIT;
	struct provenance_file *file = NULL;
	int lineno = 0, header = 0, ret = -1;

	if (memchr(buf, '\0', len)) {
		strbuf_addstr(err, _("record contains a NUL byte"));
		return -1;
	}

	while (buf < end) {
		const char *eol = memchr(buf, '\n', end - buf);
		size_t linelen = eol ? (size_t)(eol - buf) : (size_t)(end - buf);
		const char *p;

		strbuf_reset(&line);
		strbuf_add(&line, buf, linelen);
		buf += linelen + !!eol;
		lineno++;

		if (!line.len || line.buf[0] == '#')
			continue;

		switch (header) {
		case 0:
			if (!skip_prefix(line.buf, "version ", &p)) {
				parse_error(err, lineno, _("expected 'version' line"));
				goto out;
			}
			if (strcmp(p, "1")) {
				parse_error(err, lineno, _("unsupported version '%s'"), p);
				goto out;
			}
			rec->version = 1;
			header++;
			continue;
		case 1:
			if (!skip_prefix(line.buf, "commit ", &p) ||
			    parse_oid_hex_algop(p, &rec->commit, &p, algop) || *p) {
				parse_error(err, lineno, _("expected 'commit' line"));
				goto out;
			}
			header++;
			continue;
		case 2:
			if (!skip_prefix(line.buf, "capture ", &p)) {
				parse_error(err, lineno, _("expected 'capture' line"));
				goto out;
			}
			if (!strcmp(p, "declared")) {
				rec->capture = PROVENANCE_CAPTURE_DECLARED;
			} else if (!strcmp(p, "adopted")) {
				rec->capture = PROVENANCE_CAPTURE_ADOPTED;
			} else if (skip_prefix(p, "observed ", &p) &&
				   is_tool_token(p)) {
				rec->capture = PROVENANCE_CAPTURE_OBSERVED;
				rec->capture_tool = xstrdup(p);
			} else {
				parse_error(err, lineno, _("malformed 'capture' line"));
				goto out;
			}
			header++;
			continue;
		}

		if (skip_prefix(line.buf, "actor ", &p)) {
			if (file) {
				parse_error(err, lineno, _("'actor' line after 'file' line"));
				goto out;
			}
			if (parse_actor(rec, p, lineno, err))
				goto out;
		} else if (skip_prefix(line.buf, "file ", &p)) {
			if (!*p) {
				parse_error(err, lineno, _("empty path"));
				goto out;
			}
			ALLOC_GROW(rec->files, rec->files_nr + 1, rec->files_alloc);
			file = &rec->files[rec->files_nr++];
			memset(file, 0, sizeof(*file));
			file->path = xstrdup(p);
		} else if ((line.buf[0] == '+' || line.buf[0] == '-') &&
			   line.buf[1] == ' ') {
			if (!file) {
				parse_error(err, lineno, _("range before 'file' line"));
				goto out;
			}
			if (parse_range(file, line.buf, algop, lineno, err))
				goto out;
		} else {
			parse_error(err, lineno, _("unrecognized line"));
			goto out;
		}
	}

	if (header < 3) {
		strbuf_addstr(err, _("incomplete record header"));
		goto out;
	}
	ret = 0;
out:
	strbuf_release(&line);
	return ret;
}

void provenance_record_format(const struct provenance_record *rec,
			      struct strbuf *out)
{
	strbuf_addf(out, "version %d\n", rec->version);
	strbuf_addf(out, "commit %s\n", oid_to_hex(&rec->commit));
	switch (rec->capture) {
	case PROVENANCE_CAPTURE_OBSERVED:
		strbuf_addf(out, "capture observed %s\n", rec->capture_tool);
		break;
	case PROVENANCE_CAPTURE_DECLARED:
		strbuf_addstr(out, "capture declared\n");
		break;
	case PROVENANCE_CAPTURE_ADOPTED:
		strbuf_addstr(out, "capture adopted\n");
		break;
	}

	for (size_t i = 0; i < rec->actors_nr; i++) {
		strbuf_addstr(out, "actor ");
		provenance_actor_format(&rec->actors[i], out);
		strbuf_addch(out, '\n');
	}

	for (size_t i = 0; i < rec->files_nr; i++) {
		const struct provenance_file *f = &rec->files[i];

		strbuf_addf(out, "file %s\n", f->path);
		for (size_t j = 0; j < f->ranges_nr; j++) {
			const struct provenance_range *r = &f->ranges[j];

			strbuf_addf(out, "%c %lu", r->side, r->begin);
			if (r->end != r->begin)
				strbuf_addf(out, "-%lu", r->end);
			strbuf_addf(out, " %s %s %s\n", mark_names[r->mark],
				    r->actors, oid_to_hex(&r->anchor));
		}
	}
}

void provenance_actor_format(const struct provenance_actor *actor,
			     struct strbuf *out)
{
	if (actor->kind == PROVENANCE_ACTOR_AI)
		strbuf_addf(out, "%s ai model=%s tool=%s session=%s source=%s",
			    actor->id, actor->model, actor->tool,
			    actor->session, actor->source);
	else
		strbuf_addf(out, "%s human ident=%s", actor->id, actor->ident);
}

const char *provenance_mark_name(enum provenance_mark mark)
{
	return mark_names[mark];
}

char provenance_mark_char(enum provenance_mark mark)
{
	switch (mark) {
	case PROVENANCE_MARK_AI:
		return 'A';
	case PROVENANCE_MARK_MIXED:
		return 'M';
	case PROVENANCE_MARK_HUMAN:
		return 'H';
	default:
		return '?';
	}
}

const struct provenance_actor *
provenance_record_actor(const struct provenance_record *rec,
			const char *id, size_t len)
{
	for (size_t i = 0; i < rec->actors_nr; i++)
		if (!xstrncmpz(rec->actors[i].id, id, len))
			return &rec->actors[i];
	return NULL;
}

const struct provenance_file *
provenance_record_file(const struct provenance_record *rec, const char *path)
{
	for (size_t i = 0; i < rec->files_nr; i++)
		if (!strcmp(rec->files[i].path, path))
			return &rec->files[i];
	return NULL;
}

const struct provenance_range *
provenance_file_range(const struct provenance_file *file,
		      char side, unsigned long line)
{
	for (size_t i = 0; i < file->ranges_nr; i++) {
		const struct provenance_range *range = &file->ranges[i];

		if (range->side == side &&
		    range->begin <= line && line <= range->end)
			return range;
	}
	return NULL;
}

void provenance_record_release(struct provenance_record *rec)
{
	for (size_t i = 0; i < rec->actors_nr; i++) {
		struct provenance_actor *a = &rec->actors[i];

		free(a->id);
		free(a->model);
		free(a->tool);
		free(a->session);
		free(a->source);
		free(a->ident);
	}
	free(rec->actors);

	for (size_t i = 0; i < rec->files_nr; i++) {
		struct provenance_file *f = &rec->files[i];

		for (size_t j = 0; j < f->ranges_nr; j++)
			free(f->ranges[j].actors);
		free(f->ranges);
		free(f->path);
	}
	free(rec->files);
	free(rec->capture_tool);
	memset(rec, 0, sizeof(*rec));
}

/* Parse "<key>=<timestamp>" at *p, followed by a space or the end. */
static int parse_time_field(const char **p, const char *key,
			    timestamp_t *out)
{
	const char *v;
	char *end;

	if (!skip_prefix(*p, key, &v) || *v++ != '=' || !isdigit(*v))
		return -1;
	errno = 0;
	*out = parse_timestamp(v, &end, 10);
	if (errno || (*end && (*end != ' ' || !end[1])))
		return -1;
	*p = *end ? end + 1 : end;
	return 0;
}

static int parse_session(struct provenance_reasoning *rec, const char *p,
			 int lineno, struct strbuf *err)
{
	struct provenance_session *s;
	const char *sp = strchr(p, ' ');

	if (!sp || sp == p)
		return parse_error(err, lineno, _("malformed session line"));

	ALLOC_GROW(rec->sessions, rec->sessions_nr + 1, rec->sessions_alloc);
	s = &rec->sessions[rec->sessions_nr++];
	memset(s, 0, sizeof(*s));
	s->id = xstrndup(p, sp - p);
	p = sp + 1;

	if (!(s->model = parse_field(&p, "model", 0)) ||
	    !(s->tool = parse_field(&p, "tool", !strchr(p, ' '))) ||
	    (starts_with(p, "started=") &&
	     parse_time_field(&p, "started", &s->started)) ||
	    (starts_with(p, "ended=") &&
	     parse_time_field(&p, "ended", &s->ended)) ||
	    *p)
		return parse_error(err, lineno, _("malformed session '%s'"),
				   s->id);
	return 0;
}

int provenance_reasoning_parse(const char *buf, size_t len,
			       const struct git_hash_algo *algop,
			       struct provenance_reasoning *rec,
			       struct strbuf *err)
{
	const char *end = buf + len;
	struct strbuf line = STRBUF_INIT;
	int lineno = 0, header = 0, ret = -1;

	if (memchr(buf, '\0', len)) {
		strbuf_addstr(err, _("record contains a NUL byte"));
		return -1;
	}

	while (buf < end) {
		const char *eol = memchr(buf, '\n', end - buf);
		size_t linelen = eol ? (size_t)(eol - buf) : (size_t)(end - buf);
		const char *p;

		strbuf_reset(&line);
		strbuf_add(&line, buf, linelen);
		buf += linelen + !!eol;
		lineno++;

		/* the first blank line ends the header */
		if (!line.len)
			break;
		if (line.buf[0] == '#')
			continue;

		switch (header) {
		case 0:
			if (!skip_prefix(line.buf, "version ", &p)) {
				parse_error(err, lineno, _("expected 'version' line"));
				goto out;
			}
			if (strcmp(p, "1")) {
				parse_error(err, lineno, _("unsupported version '%s'"), p);
				goto out;
			}
			rec->version = 1;
			header++;
			continue;
		case 1:
			if (!skip_prefix(line.buf, "commit ", &p) ||
			    parse_oid_hex_algop(p, &rec->commit, &p, algop) || *p) {
				parse_error(err, lineno, _("expected 'commit' line"));
				goto out;
			}
			header++;
			continue;
		}

		if (!skip_prefix(line.buf, "session ", &p)) {
			parse_error(err, lineno, _("unrecognized line"));
			goto out;
		}
		if (parse_session(rec, p, lineno, err))
			goto out;
	}

	if (header < 2) {
		strbuf_addstr(err, _("incomplete record header"));
		goto out;
	}
	strbuf_add(&rec->summary, buf, end - buf);
	ret = 0;
out:
	strbuf_release(&line);
	return ret;
}

void provenance_reasoning_format(const struct provenance_reasoning *rec,
				 struct strbuf *out)
{
	strbuf_addf(out, "version %d\n", rec->version);
	strbuf_addf(out, "commit %s\n", oid_to_hex(&rec->commit));
	for (size_t i = 0; i < rec->sessions_nr; i++) {
		const struct provenance_session *s = &rec->sessions[i];

		strbuf_addf(out, "session %s model=%s tool=%s",
			    s->id, s->model, s->tool);
		if (s->started)
			strbuf_addf(out, " started=%"PRItime, s->started);
		if (s->ended)
			strbuf_addf(out, " ended=%"PRItime, s->ended);
		strbuf_addch(out, '\n');
	}
	strbuf_addch(out, '\n');
	strbuf_addbuf(out, &rec->summary);
}

void provenance_reasoning_release(struct provenance_reasoning *rec)
{
	for (size_t i = 0; i < rec->sessions_nr; i++) {
		free(rec->sessions[i].id);
		free(rec->sessions[i].model);
		free(rec->sessions[i].tool);
	}
	FREE_AND_NULL(rec->sessions);
	rec->sessions_nr = rec->sessions_alloc = 0;
	strbuf_release(&rec->summary);
}

int provenance_enabled(struct repository *r)
{
	int enabled;

	if (!repo_config_get_bool(r, "provenance.enabled", &enabled))
		return enabled;
	return git_env_bool("GIT_TEST_PROVENANCE", 1);
}

static int read_record(struct repository *r, const char *ref,
		       const struct object_id *commit, struct strbuf *out)
{
	struct object_id oid, sub, note_oid;
	char hex[GIT_MAX_HEXSZ + 1];
	size_t hexsz = r->hash_algo->hexsz;
	struct object *obj;
	unsigned short mode;
	enum object_type type;
	size_t size;
	void *data;
	int flags = 0, found = 0;

	oid_to_hex_r(hex, commit);
	if (!refs_resolve_ref_unsafe(get_main_ref_store(r), ref, 0,
				     &oid, &flags) ||
	    (flags & REF_ISBROKEN))
		return error(_("unable to read ref '%s'"), ref);
	if (is_null_oid(&oid))
		return 0;

	obj = deref_tag(r, parse_object(r, &oid), ref, 0);
	if (obj && obj->type == OBJ_COMMIT)
		oidcpy(&oid, get_commit_tree_oid((struct commit *)obj));
	else if (obj && obj->type == OBJ_TREE)
		oidcpy(&oid, &obj->oid);
	else
		return error(_("ref '%s' does not point to a tree"), ref);

	/*
	 * Look the commit up in the notes fan-out: the note is named by
	 * the rest of the hex object id, below zero or more levels of
	 * two-digit directories.
	 */
	for (size_t i = 0; i < hexsz; i += 2) {
		char dir[3] = { hex[i], hex[i + 1], '\0' };

		if (!get_tree_entry(r, &oid, hex + i, &note_oid, &mode)) {
			found = 1;
			break;
		}
		if (get_tree_entry(r, &oid, dir, &sub, &mode) ||
		    !S_ISDIR(mode))
			break;
		oidcpy(&oid, &sub);
	}
	if (!found)
		return 0;
	if (!S_ISREG(mode))
		return error(_("provenance record for '%s' in '%s' is not a blob"),
			     hex, ref);

	data = odb_read_object(r->objects, &note_oid, &type, &size);
	if (!data || type != OBJ_BLOB) {
		free(data);
		return error(_("unable to read provenance record '%s'"),
			     oid_to_hex(&note_oid));
	}
	strbuf_add(out, data, size);
	free(data);
	return 1;
}

int provenance_read_lines(struct repository *r,
			  const struct object_id *commit,
			  struct strbuf *out)
{
	return read_record(r, PROVENANCE_LINES_REF, commit, out);
}

int provenance_read_reasoning(struct repository *r,
			      const struct object_id *commit,
			      struct strbuf *out)
{
	return read_record(r, PROVENANCE_REASONING_REF, commit, out);
}

struct cached_record {
	struct oidmap_entry entry;
	/* the record is valid */
	int found;
	struct provenance_record rec;
};

void provenance_cache_init(struct provenance_cache *cache,
			   struct repository *r)
{
	cache->r = r;
	oidmap_init(&cache->records, 0);
	cache->have_ref = -1;
}

static void free_cached_record(void *p)
{
	struct cached_record *c = p;

	provenance_record_release(&c->rec);
	free(c);
}

void provenance_cache_release(struct provenance_cache *cache)
{
	oidmap_clear_with_free(&cache->records, free_cached_record);
	cache->have_ref = -1;
}

static void load_record(struct provenance_cache *cache,
			struct cached_record *c)
{
	const struct object_id *commit = &c->entry.oid;
	struct strbuf buf = STRBUF_INIT, err = STRBUF_INIT;

	if (provenance_read_lines(cache->r, commit, &buf) > 0) {
		if (provenance_record_parse(buf.buf, buf.len,
					    cache->r->hash_algo, &c->rec, &err))
			warning(_("ignoring malformed provenance record of %s: %s"),
				oid_to_hex(commit), err.buf);
		else if (!oideq(&c->rec.commit, commit))
			warning(_("ignoring provenance record of %s: it names commit %s"),
				oid_to_hex(commit), oid_to_hex(&c->rec.commit));
		else
			c->found = 1;
	}
	if (!c->found)
		provenance_record_release(&c->rec);
	strbuf_release(&buf);
	strbuf_release(&err);
}

const struct provenance_record *
provenance_cache_get(struct provenance_cache *cache,
		     const struct object_id *commit)
{
	struct cached_record *c;

	if (cache->have_ref < 0)
		cache->have_ref = refs_ref_exists(get_main_ref_store(cache->r),
						  PROVENANCE_LINES_REF);
	if (!cache->have_ref)
		return NULL;

	c = oidmap_get(&cache->records, commit);
	if (!c) {
		CALLOC_ARRAY(c, 1);
		oidcpy(&c->entry.oid, commit);
		oidmap_put(&cache->records, c);
		load_record(cache, c);
	}
	return c->found ? &c->rec : NULL;
}

__attribute__((format (printf, 4, 5)))
static int range_error(struct strbuf *err, const struct provenance_file *file,
		       const struct provenance_range *range,
		       const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	strbuf_vaddf(err, fmt, ap);
	va_end(ap);
	strbuf_addf(err, " ('%c %lu", range->side, range->begin);
	if (range->end != range->begin)
		strbuf_addf(err, "-%lu", range->end);
	strbuf_addf(err, "' in '%s')", file->path);
	return -1;
}

static int is_declared(const struct provenance_record *rec,
		       const char *id, size_t len)
{
	for (size_t i = 0; i < rec->actors_nr; i++)
		if (strlen(rec->actors[i].id) == len &&
		    !memcmp(rec->actors[i].id, id, len))
			return 1;
	return 0;
}

static int verify_actors(const struct provenance_record *rec,
			 const struct provenance_file *file,
			 const struct provenance_range *range,
			 struct strbuf *err)
{
	const char *p = range->actors;

	if (range->mark == PROVENANCE_MARK_UNKNOWN) {
		if (strcmp(range->actors, "-"))
			return range_error(err, file, range,
					   _("UNKNOWN mark requires actor list '-'"));
		return 0;
	}

	for (;;) {
		const char *e = strchrnul(p, ',');

		if (!is_declared(rec, p, e - p))
			return range_error(err, file, range,
					   _("undeclared actor '%.*s'"),
					   (int)(e - p), p);
		if (!*e)
			return 0;
		p = e + 1;
	}
}

/* The content of one side of a file, with the offset of each line. */
struct file_lines {
	int loaded;
	struct strbuf buf;
	/* line N starts at start[N - 1]; start[nr] is the end of the buffer */
	size_t *start;
	size_t nr, alloc;
};

#define FILE_LINES_INIT { .buf = STRBUF_INIT }

static void file_lines_release(struct file_lines *f)
{
	strbuf_release(&f->buf);
	free(f->start);
}

static int load_lines(struct repository *r, const struct object_id *tree_oid,
		      int parent, const struct provenance_file *file,
		      const struct provenance_range *range,
		      struct file_lines *f, struct strbuf *err)
{
	struct object_id oid;
	unsigned short mode;
	enum object_type type;
	size_t size, pos = 0;
	void *data;

	if (get_tree_entry(r, tree_oid, file->path, &oid, &mode))
		return range_error(err, file, range, parent ?
				   _("path not found in the first parent") :
				   _("path not found in the commit"));
	if (!S_ISREG(mode))
		return range_error(err, file, range, parent ?
				   _("path is not a regular file in the first parent") :
				   _("path is not a regular file in the commit"));
	data = odb_read_object(r->objects, &oid, &type, &size);
	if (!data || type != OBJ_BLOB) {
		free(data);
		return range_error(err, file, range, _("unable to read blob '%s'"),
				   oid_to_hex(&oid));
	}
	strbuf_attach(&f->buf, data, size, size + 1);

	while (pos < f->buf.len) {
		const char *eol = memchr(f->buf.buf + pos, '\n', f->buf.len - pos);

		ALLOC_GROW(f->start, f->nr + 1, f->alloc);
		f->start[f->nr++] = pos;
		pos = eol ? (size_t)(eol - f->buf.buf) + 1 : f->buf.len;
	}
	ALLOC_GROW(f->start, f->nr + 1, f->alloc);
	f->start[f->nr] = f->buf.len;
	f->loaded = 1;
	return 0;
}

static int verify_content(struct repository *r,
			  const struct provenance_file *file,
			  const struct provenance_range *range,
			  const struct file_lines *f, struct strbuf *err)
{
	struct object_id oid;
	size_t from, to;

	if (range->begin < 1 || range->end < range->begin)
		return range_error(err, file, range, _("invalid line range"));
	if (range->end > f->nr)
		return range_error(err, file, range,
				   _("range past end of file (%lu lines)"),
				   (unsigned long)f->nr);

	from = f->start[range->begin - 1];
	to = f->start[range->end];
	hash_object_file(r->hash_algo, f->buf.buf + from, to - from,
			 OBJ_BLOB, &oid);
	if (!oideq(&oid, &range->anchor))
		return range_error(err, file, range, _("anchor mismatch"));
	return 0;
}

int provenance_verify(struct repository *r, struct commit *commit,
		      const struct provenance_record *rec,
		      struct strbuf *err)
{
	const struct object_id *post_tree, *pre_tree = NULL;
	int ret = 0;

	if (!oideq(&rec->commit, &commit->object.oid)) {
		strbuf_addf(err, _("record names commit '%s'"),
			    oid_to_hex(&rec->commit));
		return -1;
	}
	if (repo_parse_commit(r, commit)) {
		strbuf_addf(err, _("unable to parse commit '%s'"),
			    oid_to_hex(&commit->object.oid));
		return -1;
	}
	post_tree = get_commit_tree_oid(commit);
	if (commit->parents) {
		struct commit *parent = commit->parents->item;

		if (repo_parse_commit(r, parent)) {
			strbuf_addf(err, _("unable to parse commit '%s'"),
				    oid_to_hex(&parent->object.oid));
			return -1;
		}
		pre_tree = get_commit_tree_oid(parent);
	}

	for (size_t i = 0; !ret && i < rec->files_nr; i++) {
		const struct provenance_file *file = &rec->files[i];
		struct file_lines post = FILE_LINES_INIT, pre = FILE_LINES_INIT;

		for (size_t j = 0; !ret && j < file->ranges_nr; j++) {
			const struct provenance_range *range = &file->ranges[j];
			int parent = range->side == '-';
			struct file_lines *f = parent ? &pre : &post;

			ret = verify_actors(rec, file, range, err);
			if (ret)
				break;
			if (parent && !pre_tree) {
				ret = range_error(err, file, range,
						  _("'-' range in a root commit"));
				break;
			}
			if (!f->loaded) {
				ret = load_lines(r, parent ? pre_tree : post_tree,
						 parent, file, range, f, err);
				if (ret)
					break;
			}
			ret = verify_content(r, file, range, f, err);
		}
		file_lines_release(&post);
		file_lines_release(&pre);
	}
	return ret;
}
