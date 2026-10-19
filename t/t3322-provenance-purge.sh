#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test purging reasoning records with git provenance purge'

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

# Attach <file> as the reasoning record of <commit> in <repo>: "git
# notes" only writes below refs/notes/, so write the note through a
# scratch notes ref.
#
# Usage: set_reasoning <repo> <commit> <file>
set_reasoning () {
	git -C "$1" update-ref refs/notes/scratch refs/provenance/reasoning &&
	git -C "$1" notes --ref=scratch add -f --no-stripspace -F "$PWD/$3" "$2" &&
	git -C "$1" update-ref refs/provenance/reasoning refs/notes/scratch &&
	git -C "$1" update-ref -d refs/notes/scratch
}

# Point refs/provenance/reasoning of repo back at the records of the
# setup.
reset_reasoning () {
	git -C repo update-ref refs/provenance/reasoning $(cat orig)
}

# Check that the reasoning records of the commits <name>... of repo are
# those of the setup.
#
# Usage: check_kept <name>...
check_kept () {
	for name in "$@"
	do
		git -C repo provenance show --reasoning $(cat $name.oid) >actual &&
		test_cmp $name.rec actual || return 1
	done
}

test_expect_success 'setup' '
	git config --global advice.provenanceSession false &&
	git config --global advice.provenancePurge false &&
	git init -b main repo &&
	git -C repo config set core.logAllRefUpdates always &&
	commit_in repo c0 &&
	commit_in repo c1 s1 &&
	commit_in repo c2 s1 &&
	commit_in repo c3 s2 &&
	commit_in repo c4 s3 &&
	for name in c0 c1 c2 c3 c4
	do
		git -C repo rev-parse main~$((4 - ${name#c})) >$name.oid || return 1
	done &&
	cat >c1.rec <<-EOF &&
	version 1
	commit $(cat c1.oid)
	session s1 model=m1 tool=t1

	Called the API with the key KEY[0].x and AKIA.KEY.
	Nothing else.
	EOF
	cat >c2.rec <<-EOF &&
	version 1
	commit $(cat c2.oid)
	session s1 model=m1 tool=t1
	session s2 model=m2 tool=t2 started=1 ended=2

	A summary of s1 and s2.
	EOF
	cat >c3.rec <<-EOF &&
	version 1
	commit $(cat c3.oid)
	session s2 model=m2 tool=t2

	The secret token is here.
	A SECRET in capitals, and Token.
	EOF
	cat >c4.rec <<-EOF &&
	version 1
	commit $(cat c4.oid)
	session s3 model=m3 tool=t3

	Plain.
	EOF
	for name in c1 c2 c3 c4
	do
		set_reasoning repo $(cat $name.oid) $name.rec || return 1
	done &&
	git -C repo rev-parse refs/provenance/reasoning >orig &&
	git -C repo rev-parse refs/provenance/lines >lines &&
	for name in c0 c1 c2 c3 c4
	do
		git -C repo provenance show $(cat $name.oid) >$name.lines ||
		return 1
	done &&
	test_must_fail git -C repo provenance show --reasoning main~4
'

test_expect_success 'purge of a commit removes its reasoning record' '
	reset_reasoning &&
	git -C repo provenance purge main~2 >actual 2>err &&
	echo "$(cat c2.oid) removed" >expect &&
	test_cmp expect actual &&
	test_must_be_empty err &&
	test_must_fail git -C repo provenance show --reasoning main~2 &&
	check_kept c1 c3 c4
'

test_expect_success 'purge leaves the line records alone' '
	git -C repo rev-parse refs/provenance/lines >actual &&
	test_cmp lines actual &&
	for name in c0 c1 c2 c3 c4
	do
		git -C repo provenance show $(cat $name.oid) >actual &&
		test_cmp $name.lines actual || return 1
	done
'

test_expect_success 'the purged ref has one commit without a parent' '
	git -C repo rev-list refs/provenance/reasoning >actual &&
	test_line_count = 1 actual &&
	echo "purge: remove records" >expect &&
	git -C repo log -1 --format=%s refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C repo log -1 --format="%cn <%ce>" refs/provenance/reasoning >actual &&
	echo "$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>" >expect &&
	test_cmp expect actual &&
	test_must_fail git -C repo merge-base --is-ancestor $(cat orig) \
		refs/provenance/reasoning &&
	echo purge >expect &&
	git -C repo reflog show --format=%gs -1 refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C repo rev-parse refs/provenance/reasoning@{1} >actual &&
	test_cmp orig actual
'

test_expect_success 'purge of a range removes the records in it' '
	reset_reasoning &&
	git -C repo provenance purge main~3..main~1 >actual &&
	cat >expect <<-EOF &&
	$(cat c2.oid) removed
	$(cat c3.oid) removed
	EOF
	test_cmp expect actual &&
	test_must_fail git -C repo provenance show --reasoning main~1 &&
	test_must_fail git -C repo provenance show --reasoning main~2 &&
	check_kept c1 c4
'

test_expect_success 'purge --dry-run writes nothing' '
	reset_reasoning &&
	git -C repo provenance purge --dry-run main~3..main >actual &&
	test_line_count = 3 actual &&
	git -C repo count-objects >expect &&
	git -C repo provenance purge --dry-run --redact secret >actual &&
	echo "$(cat c3.oid) redacted 1" >expect.out &&
	test_cmp expect.out actual &&
	git -C repo provenance purge --dry-run --session s1 >actual &&
	test_line_count = 2 actual &&
	git -C repo count-objects >actual &&
	test_cmp expect actual &&
	git -C repo rev-parse refs/provenance/reasoning >actual &&
	test_cmp orig actual
'

test_expect_success 'nothing to purge writes nothing' '
	reset_reasoning &&
	git -C repo provenance purge main~4 >actual 2>err &&
	test_must_be_empty actual &&
	test_grep "nothing to purge" err &&
	git -C repo provenance purge --session nobody >actual 2>err &&
	test_must_be_empty actual &&
	test_grep "nothing to purge" err &&
	git -C repo rev-parse refs/provenance/reasoning >actual &&
	test_cmp orig actual
'

test_expect_success 'purge --session drops the session and the summary' '
	reset_reasoning &&
	git -C repo provenance purge --session s1 >out &&
	sort out >actual &&
	sort >expect <<-EOF &&
	$(cat c1.oid) removed
	$(cat c2.oid) session dropped
	EOF
	test_cmp expect actual &&
	test_must_fail git -C repo provenance show --reasoning main~3 &&
	cat >expect <<-EOF &&
	version 1
	commit $(cat c2.oid)
	session s2 model=m2 tool=t2 started=1 ended=2

	EOF
	git -C repo provenance show --reasoning main~2 >actual &&
	test_cmp expect actual &&
	check_kept c3 c4 &&
	echo "purge: drop session s1" >expect &&
	git -C repo log -1 --format=%s refs/provenance/reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --session of several sessions' '
	reset_reasoning &&
	git -C repo provenance purge --session s1 --session s2 >out &&
	test_line_count = 3 out &&
	for rev in main~3 main~2 main~1
	do
		test_must_fail git -C repo provenance show --reasoning $rev ||
		return 1
	done &&
	check_kept c4 &&
	echo "purge: drop session s1, s2" >expect &&
	git -C repo log -1 --format=%s refs/provenance/reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --session with commits' '
	reset_reasoning &&
	git -C repo provenance purge --session s1 main~2 >actual &&
	echo "$(cat c2.oid) session dropped" >expect &&
	test_cmp expect actual &&
	check_kept c1 c3 c4
'

test_expect_success 'purge --redact replaces the matches of a basic regex' '
	reset_reasoning &&
	git -C repo provenance purge --redact "sec.et" >actual &&
	echo "$(cat c3.oid) redacted 1" >expect &&
	test_cmp expect actual &&
	sed "s/secret/***REMOVED***/" c3.rec >expect &&
	git -C repo provenance show --reasoning main~1 >actual &&
	test_cmp expect actual &&
	check_kept c1 c2 c4 &&
	echo "purge: redact a pattern" >expect &&
	git -C repo log -1 --format=%s refs/provenance/reasoning >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --redact -i ignores case' '
	reset_reasoning &&
	git -C repo provenance purge -i --redact "secret" >actual &&
	echo "$(cat c3.oid) redacted 2" >expect &&
	test_cmp expect actual &&
	sed -e "s/secret/***REMOVED***/" -e "s/SECRET/***REMOVED***/" \
		c3.rec >expect &&
	git -C repo provenance show --reasoning main~1 >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --redact -E takes an extended regex' '
	reset_reasoning &&
	git -C repo provenance purge -E --redact "(t|T)oken" main~1 >actual &&
	echo "$(cat c3.oid) redacted 2" >expect &&
	test_cmp expect actual &&
	sed -e "s/token/***REMOVED***/" -e "s/Token/***REMOVED***/" \
		c3.rec >expect &&
	git -C repo provenance show --reasoning main~1 >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --redact -F takes a fixed string' '
	reset_reasoning &&
	git -C repo provenance purge --redact "KEY[0].x" >actual 2>err &&
	test_must_be_empty actual &&
	test_grep "nothing to purge" err &&
	git -C repo provenance purge -F --redact "KEY[0].x" >actual &&
	echo "$(cat c1.oid) redacted 1" >expect &&
	test_cmp expect actual &&
	sed "s/KEY\[0\]\.x/***REMOVED***/" c1.rec >expect &&
	git -C repo provenance show --reasoning main~3 >actual &&
	test_cmp expect actual
'

test_expect_success 'purge --redact leaves the header alone' '
	reset_reasoning &&
	git -C repo provenance purge --redact "tool=t1" >actual 2>err &&
	test_grep "nothing to purge" err &&
	git -C repo provenance purge -E --redact "model|commit|version" \
		>actual 2>err &&
	test_grep "nothing to purge" err &&
	git -C repo provenance purge --redact "s1 and" >actual &&
	echo "$(cat c2.oid) redacted 1" >expect &&
	test_cmp expect actual &&
	sed "s/s1 and/***REMOVED***/" c2.rec >expect &&
	git -C repo provenance show --reasoning main~2 >actual &&
	test_cmp expect actual
'

test_expect_success 'purge keeps a record that does not parse' '
	reset_reasoning &&
	echo garbage >garbage &&
	set_reasoning repo $(cat c4.oid) garbage &&
	git -C repo provenance purge --redact "e" main~1.. >actual 2>err &&
	test_must_be_empty actual &&
	test_grep "warning: keeping the reasoning record of $(cat c4.oid)" err &&
	git -C repo provenance show --reasoning main >actual &&
	test_cmp garbage actual
'

test_expect_success 'show, verify and placeholders after a purge' '
	reset_reasoning &&
	git -C repo log -1 --format="%(provenance:share)" main~1 >share &&
	git -C repo provenance purge --redact secret &&
	git -C repo provenance purge main~2 &&
	git -C repo provenance verify main~2 main~1 >actual &&
	cat >expect <<-EOF &&
	$(cat c2.oid) ok
	$(cat c3.oid) ok
	EOF
	test_cmp expect actual &&
	git -C repo log -1 --format="%(provenance:summary)" main~2 >actual &&
	echo >expect &&
	test_cmp expect actual &&
	git -C repo log -1 --format="%(provenance:summary)" main~1 >actual &&
	cat >expect <<-\EOF &&
	The ***REMOVED*** token is here.
	A SECRET in capitals, and Token.
	EOF
	test_cmp expect actual &&
	git -C repo log -1 --format="%(provenance:share)" main~1 >actual &&
	test_cmp share actual
'

test_expect_success 'purge prints advice' '
	reset_reasoning &&
	git -C repo -c advice.provenancePurge=true provenance purge main~1 \
		>actual 2>err &&
	test_grep "hint: .git push --force <remote> refs/provenance/reasoning." err &&
	test_grep "hint: .git reflog expire --expire=now refs/provenance/reasoning." err &&
	test_grep "hint: and .git gc --prune=now." err &&
	reset_reasoning &&
	git -C repo -c advice.provenancePurge=true provenance purge --dry-run \
		main~1 >actual 2>err &&
	test_must_be_empty err
'

test_expect_success 'purge rejects bad options' '
	reset_reasoning &&
	test_must_fail git -C repo provenance purge 2>err &&
	test_grep "usage: git provenance purge" err &&
	test_must_fail git -C repo provenance purge --session s1 \
		--redact x 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git -C repo provenance purge -F main 2>err &&
	test_grep "the option .-F. requires .--redact." err &&
	test_must_fail git -C repo provenance purge -i main 2>err &&
	test_grep "the option .-i. requires .--redact." err &&
	test_must_fail git -C repo provenance purge --redact "\\(" 2>err &&
	test_grep "invalid regular expression" err &&
	test_must_fail git -C repo provenance purge no-such-rev &&
	git -C repo rev-parse refs/provenance/reasoning >actual &&
	test_cmp orig actual
'

test_expect_success 'setup a remote' '
	reset_reasoning &&
	git init --bare -b main remote.git &&
	git -C repo remote add origin ../remote.git &&
	git -C repo push -u origin main &&
	git clone remote.git other &&
	git clone remote.git stale &&
	git -C remote.git provenance show --reasoning $(cat c2.oid) >actual &&
	test_cmp c2.rec actual
'

test_expect_success 'fetch after a purge brings nothing back' '
	git -C repo provenance purge main~2 &&
	git -C repo rev-parse refs/provenance/reasoning >purged &&
	git -C repo fetch &&
	git -C repo rev-parse refs/provenance/reasoning >actual &&
	test_cmp purged actual &&
	test_must_fail git -C repo provenance show --reasoning main~2
'

test_expect_success 'fetch after a purge takes the new records of the remote' '
	commit_in other c5 s5 &&
	git -C other push &&
	git -C repo pull &&
	test_must_fail git -C repo provenance show --reasoning main~3 &&
	git -C repo provenance show --reasoning main >actual &&
	test_grep "^session s5 " actual &&
	git -C repo rev-list --parents -1 refs/provenance/reasoning >actual &&
	echo "$(git -C repo rev-parse refs/provenance/reasoning) $(cat purged)" >expect &&
	test_cmp expect actual
'

test_expect_success 'push after a purge fails with a hint' '
	test_must_fail git -C repo push 2>err &&
	test_grep "hint: The history of .refs/provenance/reasoning. was rewritten locally" err &&
	test_grep "hint: with it, use .git push --force origin refs/provenance/reasoning.." err &&
	git -C remote.git provenance show --reasoning $(cat c2.oid) >actual &&
	test_cmp c2.rec actual
'

test_expect_success 'push --force of the purged ref replaces it on the remote' '
	git -C repo push --force origin refs/provenance/reasoning &&
	git -C repo rev-parse refs/provenance/reasoning >expect &&
	git -C remote.git rev-parse refs/provenance/reasoning >actual &&
	test_cmp expect actual &&
	git -C repo rev-parse refs/provenance/remotes/origin/reasoning >actual &&
	test_cmp expect actual &&
	test_must_fail git -C remote.git provenance show --reasoning $(cat c2.oid) &&
	git -C repo fetch -v 2>err &&
	test_grep ! "^fast-forwarded\|^merged\|^created\|^reset" err &&
	git -C repo push 2>err &&
	test_grep ! "Merging the provenance" err
'

test_expect_success 'a clone that fetches takes the purge and pushes without force' '
	commit_in other c6 s6 &&
	git -C other pull 2>err &&
	test_grep ! "shares no history" err &&
	test_must_fail git -C other provenance show --reasoning $(cat c2.oid) &&
	git -C other provenance show --reasoning main~1 >actual &&
	test_grep "^session s5 " actual &&
	git -C other provenance show --reasoning main >actual &&
	test_grep "^session s6 " actual &&
	git -C other push 2>err &&
	test_grep ! "forced update\|Merging the provenance" err &&
	git -C remote.git provenance show --reasoning $(git -C other rev-parse main) &&
	test_must_fail git -C remote.git provenance show --reasoning $(cat c2.oid)
'

test_expect_success 'a clone that pushes with --force without fetching brings the records back' '
	git -C stale push --force origin refs/provenance/reasoning &&
	git -C remote.git provenance show --reasoning $(cat c2.oid) >actual &&
	test_cmp c2.rec actual
'

test_done
