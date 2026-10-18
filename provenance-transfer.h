#ifndef PROVENANCE_TRANSFER_H
#define PROVENANCE_TRANSFER_H

struct ref;
struct refspec;
struct remote;
struct repository;

/*
 * The provenance refs of a remote are fetched into tracking refs, as
 * branches are fetched into remote-tracking branches:
 * refs/provenance/<kind> of the remote "origin" lands in
 * refs/provenance/remotes/origin/<kind>, and is then merged into the
 * local refs/provenance/<kind>.  See the "Transfer" section of
 * Documentation/technical/git-provenance.adoc.
 */
#define PROVENANCE_TRACKING_PREFIX "refs/provenance/remotes/"

/*
 * Append to `out` the refspecs that fetch the provenance refs of
 * `remote` into its tracking refs, if provenance is enabled in `r`,
 * `remote` is a configured remote and "remote.<name>.provenance" is
 * not false.  The reasoning ref is fetched
 * only if "provenance.reasoning.fetch" is "always".  A provenance ref
 * that is already mapped by `cmdline` (the refspecs given on the
 * command line, if any) or by the configured refspecs of `remote`,
 * as a mirror refspec maps it, is left out.  The refspecs name the
 * refs exactly: a caller must allow them to be missing on the remote.
 */
void provenance_fetch_refspecs(struct repository *r, struct remote *remote,
			       struct refspec *cmdline, struct refspec *out);

/*
 * Merge each tracking ref of the remote `remote_name` that the fetch
 * described by `ref_map` updated into the local provenance ref, as
 * the "Transfer" section of the design describes: create it,
 * fast-forward it, or make a merge commit whose tree is the union of
 * both sides, keeping the local record (with a warning) for a commit
 * that both sides changed.  With `verbose` > 0, report each merged
 * ref on stderr.  Nothing is merged if "remote.<name>.provenance" of
 * `remote_name` is false.  Returns 0 on success and -1, after
 * reporting an error, if a ref could not be merged.
 */
int provenance_merge_fetched(struct repository *r, const char *remote_name,
			     const struct ref *ref_map, int verbose);

/*
 * Append to `out` the refspecs that push the local provenance refs
 * along with the refs that `rs` and the transport flags `flags` push
 * to `remote`.  They are added only if provenance is enabled, `remote`
 * is a configured remote whose "remote.<name>.provenance" is not
 * false, the push updates at least one ref under refs/heads/ on the
 * remote and is neither a mirror nor a pruning push, and the local
 * provenance ref exists.  The reasoning ref is pushed only if
 * "provenance.reasoning.push" is "always".  A provenance ref that `rs`
 * already names is left out.
 */
void provenance_push_refspecs(struct repository *r, struct remote *remote,
			      const struct refspec *rs, int flags,
			      struct refspec *out);

/*
 * Return 1 if the push whose result is `refs` (as transport_push()
 * leaves it) had a provenance ref rejected because the remote ref
 * moved, and 0 otherwise.  If `other_errors` is not NULL, set it to 1
 * if any other ref failed for another reason.
 */
int provenance_push_rejected(const struct ref *refs, int *other_errors);

/*
 * Fetch the provenance refs that the push whose result is `refs`
 * failed to update from the remote `remote_name` and merge them into
 * the local provenance refs, by running "git fetch".  The cached refs
 * and objects of `r` are dropped afterwards, so that `r` sees the
 * merged refs.  Returns 0 on success and non-zero if the fetch failed.
 */
int provenance_fetch_rejected(struct repository *r, const char *remote_name,
			      const struct ref *refs);

#endif /* PROVENANCE_TRANSFER_H */
