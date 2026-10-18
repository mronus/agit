#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test transferring provenance with fetch, push and clone'

GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Append a line to <file> in <repo> and commit it, with the session
# <session> if one is given, so that the commit has a line record and,
# with a session, a reasoning record.
#
# Usage: commit_in <repo> <file> [<session>]
commit_in () {
	echo "$2" >>"$1/$2" &&
	git -C "$1" add "$2" &&
	git -C "$1" commit -q -m "$2" ${3:+--session "$3"}
}

# Print the provenance refs of <repo>, one per line.
#
# Usage: provenance_refs <repo>
provenance_refs () {
	git -C "$1" for-each-ref --format="%(refname)" refs/provenance/
}

# Attach <file> as the line record of <commit> in <repo>, as t3315
# does: "git notes" only writes below refs/notes/, so write the note
# through a scratch notes ref.
#
# Usage: replace_record <repo> <commit> <file>
replace_record () {
	git -C "$1" update-ref refs/notes/scratch refs/provenance/lines &&
	git -C "$1" notes --ref=scratch add -f --no-stripspace -F "$PWD/$3" "$2" &&
	git -C "$1" update-ref refs/provenance/lines refs/notes/scratch &&
	git -C "$1" update-ref -d refs/notes/scratch
}

test_expect_success 'setup' '
	git config --global advice.provenanceSession false &&
	git init -b main src &&
	commit_in src one s1 &&
	git init --bare -b main remote.git &&
	git -C src remote add origin ../remote.git &&
	git -C src push -u origin main &&
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/reasoning
	EOF
	provenance_refs remote.git >actual &&
	test_cmp expect actual
'

test_expect_success 'push sends the provenance refs with the branch' '
	git -C src rev-parse refs/provenance/lines refs/provenance/reasoning >expect &&
	git -C remote.git rev-parse refs/provenance/lines refs/provenance/reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'clone fetches the provenance refs and creates the local refs' '
	git clone remote.git c1 &&
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/reasoning
	refs/provenance/remotes/origin/lines
	refs/provenance/remotes/origin/reasoning
	EOF
	provenance_refs c1 >actual &&
	test_cmp expect actual &&
	git -C remote.git rev-parse refs/provenance/lines refs/provenance/reasoning >expect &&
	git -C c1 rev-parse refs/provenance/lines refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C c1 rev-parse refs/provenance/remotes/origin/lines \
		refs/provenance/remotes/origin/reasoning >actual &&
	test_cmp expect actual &&
	git -C src provenance show main >expect &&
	git -C c1 provenance show main >actual &&
	test_cmp expect actual &&
	git -C c1 provenance show --reasoning main >actual &&
	test_grep "^session s1" actual
'

test_expect_success 'clone writes no refspecs to the configuration' '
	echo "+refs/heads/*:refs/remotes/origin/*" >expect &&
	git -C c1 config get --all remote.origin.fetch >actual &&
	test_cmp expect actual &&
	test_must_fail git -C c1 config get remote.origin.push
'

test_expect_success 'push to a URL does not send provenance' '
	git init --bare -b main plain.git &&
	git -C src push ../plain.git main 2>err &&
	test_grep ! refs/provenance err &&
	provenance_refs plain.git >actual &&
	test_must_be_empty actual
'

test_expect_success 'clone of a remote without provenance refs' '
	git clone plain.git plain &&
	provenance_refs plain >actual &&
	test_must_be_empty actual &&
	git -C plain fetch &&
	provenance_refs plain >actual &&
	test_must_be_empty actual
'

test_expect_success 'clone --mirror has the refs directly' '
	git clone --mirror remote.git mirror.git &&
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/reasoning
	EOF
	provenance_refs mirror.git >actual &&
	test_cmp expect actual &&
	git -C mirror.git fetch &&
	provenance_refs mirror.git >actual &&
	test_cmp expect actual
'

test_expect_success 'fetch brings new records and fast-forwards the local refs' '
	commit_in src two s2 &&
	git -C src push 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines" err &&
	git -C c1 fetch 2>err &&
	test_grep "refs/provenance/lines *-> refs/provenance/remotes/origin/lines" err &&
	test_grep ! "fast-forwarded" err &&
	git -C remote.git rev-parse refs/provenance/lines refs/provenance/reasoning >expect &&
	git -C c1 rev-parse refs/provenance/lines refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C src provenance show main >expect &&
	git -C c1 provenance show origin/main >actual &&
	test_cmp expect actual
'

test_expect_success 'fetch does not write the provenance refs to FETCH_HEAD' '
	test_grep ! refs/provenance c1/.git/FETCH_HEAD
'

test_expect_success 'fetch -v reports the merge' '
	commit_in src three &&
	git -C src push &&
	git -C c1 fetch -v 2>err &&
	test_grep "^fast-forwarded .refs/provenance/lines. to .refs/provenance/remotes/origin/lines.$" err &&
	test_grep ! "refs/provenance/reasoning.* to " err
'

test_expect_success 'fetch does nothing when the local refs are ahead' '
	git -C c1 checkout -q -b local origin/main &&
	commit_in c1 local &&
	git -C c1 rev-parse refs/provenance/lines >expect &&
	git -C c1 update-ref -d refs/provenance/remotes/origin/lines &&
	git -C c1 fetch -v 2>err &&
	test_grep ! "provenance/lines. to" err &&
	git -C c1 rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual &&
	git -C c1 rev-parse refs/provenance/remotes/origin/lines >actual &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	test_cmp expect actual
'

test_expect_success 'fetch with an explicit refspec fetches provenance' '
	commit_in src four &&
	git -C src push &&
	git clone remote.git c2 &&
	commit_in src five &&
	git -C src push &&
	git -C c2 fetch origin main &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C c2 rev-parse refs/provenance/remotes/origin/lines >actual &&
	test_cmp expect actual &&
	git -C c2 rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'pull merges provenance' '
	commit_in src six &&
	git -C src push &&
	git -C c2 pull &&
	git -C src provenance show main >expect &&
	git -C c2 provenance show main >actual &&
	test_cmp expect actual
'

test_expect_success 'fetch --all merges the provenance of each remote' '
	git -C c2 remote add second ../remote.git &&
	git -C c2 fetch --all &&
	git -C c2 rev-parse refs/provenance/remotes/origin/lines >expect &&
	git -C c2 rev-parse refs/provenance/remotes/second/lines >actual &&
	test_cmp expect actual &&
	git -C c2 remote remove second &&
	git -C c2 for-each-ref --format="%(refname)" \
		refs/provenance/remotes/second/ >refs &&
	while read ref
	do
		git -C c2 update-ref -d "$ref" || return 1
	done <refs
'

test_expect_success 'push retries after merging when the remote moved' '
	git clone remote.git a &&
	git clone remote.git b &&
	commit_in a from-a &&
	git -C a push &&
	git -C b checkout -q -b topic &&
	commit_in b from-b &&
	git -C b push origin topic 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines (fetch first)" err &&
	test_grep "Merging the provenance refs of .origin. and pushing them again" err &&
	test_grep ! "^error" err &&
	git -C b rev-parse refs/provenance/lines >expect &&
	git -C remote.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual &&
	git -C remote.git rev-list --merges refs/provenance/lines >merges &&
	test_line_count = 1 merges &&
	git -C a provenance show main >expect &&
	git -C remote.git provenance show main >actual &&
	test_cmp expect actual &&
	git -C b provenance show topic >expect &&
	git -C remote.git provenance show topic >actual &&
	test_cmp expect actual &&
	git -C a fetch &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C a rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual &&
	git -C a provenance show origin/topic
'

test_expect_success 'merge keeps the local record of a commit both sides changed' '
	git clone remote.git c3 &&
	commit=$(git -C c3 rev-parse main) &&
	echo theirs >theirs &&
	replace_record a $commit theirs &&
	git -C a push origin refs/provenance/lines &&
	echo ours >ours &&
	replace_record c3 $commit ours &&
	git -C c3 fetch 2>err &&
	test_grep "warning: keeping the local provenance record of $commit in .refs/provenance/lines." err &&
	grep warning err >warnings &&
	test_line_count = 1 warnings &&
	git -C c3 provenance show $commit >actual &&
	test_cmp ours actual &&
	git -C c3 rev-list --merges refs/provenance/lines >merges &&
	test_line_count = 2 merges &&
	git -C c3 rev-parse refs/provenance/lines^2 >actual &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	test_cmp expect actual
'

test_expect_success 'merge takes a record that only the remote changed' '
	commit=$(git -C c3 rev-parse main~1) &&
	echo changed >changed &&
	replace_record a $commit changed &&
	git -C a push origin refs/provenance/lines &&
	git -C c3 fetch 2>err &&
	test_grep ! warning err &&
	git -C c3 provenance show $commit >actual &&
	test_cmp changed actual
'

test_expect_success 'provenance.reasoning.fetch=never' '
	git clone -c provenance.reasoning.fetch=never remote.git no-reasoning &&
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/remotes/origin/lines
	EOF
	provenance_refs no-reasoning >actual &&
	test_cmp expect actual &&
	git -C no-reasoning fetch &&
	provenance_refs no-reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'provenance.reasoning.push=never' '
	git -C remote.git rev-parse refs/provenance/reasoning >expect &&
	git -C a checkout -q main &&
	git -C a pull -q &&
	commit_in a with-session s3 &&
	git -C a -c provenance.reasoning.push=never push &&
	git -C remote.git rev-parse refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C a rev-parse refs/provenance/lines >expect &&
	git -C remote.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual &&
	git -C a push &&
	git -C a rev-parse refs/provenance/reasoning >expect &&
	git -C remote.git rev-parse refs/provenance/reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'ondemand is rejected' '
	test_must_fail git -C a -c provenance.reasoning.fetch=ondemand fetch 2>err &&
	test_grep "provenance.reasoning.fetch=ondemand is not supported yet" err &&
	test_must_fail git -C a -c provenance.reasoning.push=ondemand push 2>err &&
	test_grep "provenance.reasoning.push=ondemand is not supported yet" err &&
	test_must_fail git -C a -c provenance.reasoning.fetch=sometimes fetch 2>err &&
	test_grep "invalid value for .provenance.reasoning.fetch.: .sometimes." err
'

test_expect_success 'provenance.enabled=false transfers nothing' '
	git clone -c provenance.enabled=false remote.git disabled &&
	provenance_refs disabled >actual &&
	test_must_be_empty actual &&
	commit_in a disabled-a &&
	git -C remote.git for-each-ref refs/provenance/ >expect &&
	git -C a -c provenance.enabled=false push 2>err &&
	test_grep ! refs/provenance err &&
	git -C remote.git for-each-ref refs/provenance/ >actual &&
	test_cmp expect actual &&
	git -C a push &&
	git -C c3 for-each-ref refs/provenance/ >expect &&
	git -C c3 -c provenance.enabled=false fetch 2>err &&
	test_grep ! refs/provenance err &&
	git -C c3 for-each-ref refs/provenance/ >actual &&
	test_cmp expect actual
'

test_expect_success 'GIT_TEST_PROVENANCE=false transfers nothing' '
	GIT_TEST_PROVENANCE=false git clone remote.git test-off 2>err &&
	printf "Cloning into \047test-off\047...\ndone.\n" >expect &&
	test_cmp expect err &&
	provenance_refs test-off >actual &&
	test_must_be_empty actual &&
	commit_in a test-off-a &&
	git -C remote.git for-each-ref refs/provenance/ >expect &&
	GIT_TEST_PROVENANCE=false git -C a push 2>err &&
	test_grep ! refs/provenance err &&
	git -C remote.git for-each-ref refs/provenance/ >actual &&
	test_cmp expect actual &&
	git -C a push &&
	GIT_TEST_PROVENANCE=false git -C test-off fetch 2>err &&
	test_grep ! refs/provenance err &&
	provenance_refs test-off >actual &&
	test_must_be_empty actual
'

test_expect_success 'fetch --prune removes stale tracking refs' '
	git clone --bare remote.git prune.git &&
	git clone prune.git prune &&
	git -C prune.git update-ref -d refs/provenance/reasoning &&
	git -C prune fetch --prune 2>err &&
	test_grep "\[deleted\].*refs/provenance/remotes/origin/reasoning" err &&
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/reasoning
	refs/provenance/remotes/origin/lines
	EOF
	provenance_refs prune >actual &&
	test_cmp expect actual
'

test_expect_success 'fetch --dry-run and push --dry-run change no refs' '
	commit_in a dry-run &&
	git -C a push &&
	git -C c3 for-each-ref >expect &&
	git -C c3 fetch --dry-run &&
	git -C c3 for-each-ref >actual &&
	test_cmp expect actual &&
	git -C c3 pull -q &&
	commit_in c3 dry-run-push &&
	git -C remote.git for-each-ref >expect &&
	git -C c3 push --dry-run origin main 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines" err &&
	git -C remote.git for-each-ref >actual &&
	test_cmp expect actual
'

test_expect_success 'push --tags and push --delete do not push provenance' '
	git -C c3 push origin HEAD:refs/heads/doomed 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines" err &&
	commit_in c3 after-doomed &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C c3 tag v1 &&
	git -C c3 push --tags 2>err &&
	test_grep ! refs/provenance err &&
	git -C c3 push --delete origin doomed 2>err &&
	test_grep ! refs/provenance err &&
	git -C remote.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'push --all pushes provenance' '
	commit_in c3 all &&
	git -C c3 push --all 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines" err &&
	git -C c3 rev-parse refs/provenance/lines >expect &&
	git -C remote.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'push without local provenance refs' '
	git clone -c provenance.enabled=false remote.git none &&
	git -C none -c provenance.enabled=false commit -q --allow-empty -m none &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C none push 2>err &&
	test_grep ! refs/provenance err &&
	git -C remote.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'push warns if a hook refuses the retry' '
	git clone --bare remote.git hook.git &&
	git clone hook.git h1 &&
	git clone hook.git h2 &&
	commit_in h1 h1 &&
	git -C h1 push &&
	test_hook -C hook.git pre-receive <<-\EOF &&
	while read old new ref
	do
		case "$ref" in
		refs/provenance/*)
			echo >&2 "no provenance here"
			exit 1 ;;
		esac
	done
	EOF
	git -C h2 checkout -q -b hooked &&
	commit_in h2 h2 &&
	git -C h2 push origin hooked 2>err &&
	test_grep "Merging the provenance refs of .origin. and pushing them again" err &&
	test_grep "warning: the remote .origin. refused the provenance refs: refs/provenance/lines (pre-receive hook declined)$" err &&
	test_grep ! "^error" err &&
	git -C hook.git rev-parse --verify hooked &&
	git -C h1 rev-parse refs/provenance/lines >expect &&
	git -C hook.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'shallow clone gets and merges records' '
	git -C a pull -q &&
	commit_in a before-shallow &&
	git -C a push &&
	git clone --depth 1 "file://$(pwd)/remote.git" shallow &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C shallow rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual &&
	git -C a provenance show main >expect &&
	git -C shallow provenance show main >actual &&
	test_cmp expect actual &&
	commit_in shallow shallow &&
	commit_in a after-shallow &&
	git -C a push &&
	git -C shallow fetch 2>err &&
	git -C shallow rev-list --merges refs/provenance/lines -1 >merges &&
	test_line_count = 1 merges &&
	git -C shallow rev-parse refs/provenance/lines^2 >actual &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	test_cmp expect actual &&
	git -C a provenance show main >expect &&
	git -C shallow provenance show origin/main >actual &&
	test_cmp expect actual
'

test_expect_success 'remote.<name>.provenance=false: fetch takes nothing' '
	git clone remote.git optout &&
	git -C optout config set remote.origin.provenance false &&
	git -C optout for-each-ref refs/provenance/ >expect &&
	commit_in a optout-a &&
	git -C a push &&
	git -C optout fetch 2>err &&
	test_grep ! refs/provenance err &&
	git -C optout pull -q 2>err &&
	test_grep ! refs/provenance err &&
	git -C optout remote update 2>err &&
	test_grep ! refs/provenance err &&
	git -C optout for-each-ref refs/provenance/ >actual &&
	test_cmp expect actual
'

test_expect_success 'remote.<name>.provenance=false: explicit fetch refspecs' '
	git -C optout rev-parse refs/provenance/lines >expect.local &&
	git -C optout fetch origin \
		refs/provenance/lines:refs/provenance/remotes/origin/lines &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C optout rev-parse refs/provenance/remotes/origin/lines >actual &&
	test_cmp expect actual &&
	git -C optout rev-parse refs/provenance/lines >actual &&
	test_cmp expect.local actual
'

test_expect_success 'remote.<name>.provenance=false: push sends nothing' '
	commit_in optout optout-push &&
	git -C remote.git for-each-ref refs/provenance/ >expect &&
	git -C optout push 2>err &&
	test_grep ! refs/provenance err &&
	git -C remote.git for-each-ref refs/provenance/ >actual &&
	test_cmp expect actual &&
	git -C optout push origin \
		refs/provenance/lines:refs/provenance/optout 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/optout" err &&
	git -C optout rev-parse refs/provenance/lines >expect &&
	git -C remote.git rev-parse refs/provenance/optout >actual &&
	test_cmp expect actual &&
	git -C remote.git update-ref -d refs/provenance/optout
'

test_expect_success 'remote.<name>.provenance=false: clone takes nothing' '
	git clone -c remote.origin.provenance=false remote.git optout-clone &&
	provenance_refs optout-clone >actual &&
	test_must_be_empty actual &&
	test_cmp_config -C optout-clone false remote.origin.provenance &&
	git -C optout-clone fetch &&
	provenance_refs optout-clone >actual &&
	test_must_be_empty actual
'

test_expect_success 'push to refs/for/<branch> does not push provenance' '
	git init --bare -b main review.git &&
	git -C optout remote add review ../review.git &&
	git -C optout push review HEAD:refs/for/main 2>err &&
	test_grep ! refs/provenance err &&
	provenance_refs review.git >actual &&
	test_must_be_empty actual &&
	git -C optout push review HEAD:main 2>err &&
	test_grep "refs/provenance/lines -> refs/provenance/lines" err
'

test_expect_success 'push succeeds with a warning if a hook refuses provenance' '
	git clone --bare remote.git refuse.git &&
	git clone refuse.git r1 &&
	test_hook --setup -C refuse.git update <<-\EOF &&
	case "$1" in
	refs/provenance/*)
		echo >&2 "no provenance here"
		exit 1 ;;
	esac
	EOF
	commit_in r1 refused s4 &&
	git -C r1 push 2>err &&
	test_grep "! \[remote rejected\] *refs/provenance/lines -> refs/provenance/lines (hook declined)" err &&
	test_grep "warning: the remote .origin. refused the provenance refs: refs/provenance/lines (hook declined), refs/provenance/reasoning (hook declined)$" err &&
	test_grep "hint: .origin., set .remote.origin.provenance. to false" err &&
	test_grep ! "^error" err &&
	git -C r1 rev-parse main >expect &&
	git -C refuse.git rev-parse main >actual &&
	test_cmp expect actual &&
	git -C remote.git rev-parse refs/provenance/lines >expect &&
	git -C refuse.git rev-parse refs/provenance/lines >actual &&
	test_cmp expect actual
'

test_expect_success 'push fails if a hook refuses an explicit provenance ref' '
	commit_in r1 refused-explicit &&
	test_must_fail git -C r1 push origin main \
		refs/provenance/lines:refs/provenance/lines 2>err &&
	test_grep "error: failed to push some refs" err &&
	test_grep ! "warning: the remote" err &&
	git -C r1 rev-parse main >expect &&
	git -C refuse.git rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'push --atomic fails if a hook refuses provenance' '
	commit_in r1 refused-atomic &&
	git -C refuse.git rev-parse main >expect &&
	test_must_fail git -C r1 push --atomic 2>err &&
	test_grep "error: failed to push some refs" err &&
	test_grep ! "warning: the remote" err &&
	git -C refuse.git rev-parse main >actual &&
	test_cmp expect actual
'

test_done
