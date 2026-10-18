#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test carrying provenance records across history rewrites'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME
GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-rebase.sh

# Print the anchor of lines <a> to <b> of <rev>:<path>.
#
# Usage: anchor <rev> <path> <a>[,<b>]
anchor () {
	git cat-file blob "$1:$2" >anchor.tmp &&
	sed -n "$3p" anchor.tmp | git hash-object --stdin &&
	rm -f anchor.tmp
}

# Print the first lines of the line record of <rev>.
header () {
	echo "version 1" &&
	echo "commit $(git rev-parse "$1")" &&
	echo "capture observed git/$git_version"
}

human_actor="actor human human ident=$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>"
s1_actor="actor s1 ai model=m1 tool=t1 session=s1 source=observed"
s2_actor="actor s2 ai model=unknown tool=unknown session=s2 source=observed"

# Compare the line record of <rev> with the file "expect".
check_lines () {
	git provenance show "$1" >actual &&
	test_cmp expect actual
}

# Compare the reasoning record of <rev> with the file "expect".
check_reasoning () {
	git provenance show --reasoning "$1" >actual &&
	test_cmp expect actual
}

# Check that <new> has the records of <old>, except for the commit
# they name.
#
# Usage: same_records <old> <new>
same_records () {
	new=$(git rev-parse "$2") &&
	git provenance show "$1" >old.out &&
	sed -e "s/^commit .*/commit $new/" old.out >expect &&
	check_lines "$2" &&
	if git provenance show --reasoning "$1" >old.out
	then
		sed -e "s/^commit .*/commit $new/" old.out >expect &&
		check_reasoning "$2"
	else
		test_must_fail git provenance show --reasoning "$2"
	fi
}

# Print the session line of <session> in the reasoning record of <rev>.
#
# Usage: session_line <rev> <session>
session_line () {
	git provenance show --reasoning "$1" >session.out &&
	grep "^session $2 " session.out
}

# Attach a record to a commit, as t3315 does: "git notes" only writes
# below refs/notes/, so write the note through a scratch notes ref.
#
# Usage: add_record (lines|reasoning) <commit> <file>
add_record () {
	git update-ref "refs/notes/provenance-$1" "refs/provenance/$1" &&
	git notes --ref="provenance-$1" add -f --no-stripspace -F "$3" "$2" &&
	git update-ref "refs/provenance/$1" "refs/notes/provenance-$1" &&
	git update-ref -d "refs/notes/provenance-$1"
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	/expect*
	/actual*
	/err
	/bad
	EOF
	git version >version.out &&
	git_version=$(sed -n -e "s/^git version //p" version.out) &&
	test_write_lines 1 2 3 4 5 6 7 8 9 10 >file &&
	git add file &&
	test_tick &&
	git commit -m base &&
	git tag base &&

	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines 1 2 three 4 5 6 7 8 9 10 11 >file &&
	git stamp end --session s1 &&
	echo "First summary." | git stamp reason --session s1 --summary - &&
	test_tick &&
	git commit -a -m ai --session s1 &&
	git tag ai &&
	{
		header ai &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1 $(anchor ai file 3)" &&
		echo "+ 11 AI s1 $(anchor ai file 11)" &&
		echo "- 3 AI s1 $(anchor base file 3)"
	} >expect &&
	check_lines ai &&

	git checkout -b shifted base &&
	test_write_lines 0 1 2 3 4 5 6 7 8 9 10 >file &&
	test_tick &&
	git commit -a -m shift &&
	git tag shift &&
	git checkout main
'

test_expect_success '--amend keeps the marks of the lines it leaves alone' '
	git checkout -b amend ai &&
	test_write_lines 1 2 THREE 4 6 7 8 9 10 11 12 >file &&
	git stamp begin --session s2 &&
	test_write_lines 1 2 THREE 4 6 7 8 9 10 11 12 13 >file &&
	git stamp end --session s2 &&
	echo "Second summary." | git stamp reason --session s2 --summary - &&
	git stamp list --session s2 >stamps.out &&
	started=$(sed -n -e "s/^begin \([0-9]*\) .*/\1/p" stamps.out) &&
	ended=$(sed -n -e "s/^end \([0-9]*\) .*/\1/p" stamps.out) &&
	git commit -a --amend --no-edit &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 3 MIXED s1,human $(anchor HEAD file 3)" &&
		echo "+ 10 AI s1 $(anchor HEAD file 10)" &&
		echo "+ 11 HUMAN human $(anchor HEAD file 11)" &&
		echo "+ 12 AI s2 $(anchor HEAD file 12)" &&
		echo "- 3 AI s1 $(anchor base file 3)" &&
		echo "- 5 HUMAN human $(anchor base file 5)"
	} >expect &&
	check_lines HEAD &&
	{
		echo "version 1" &&
		echo "commit $(git rev-parse HEAD)" &&
		session_line ai s1 &&
		echo "session s2 model=unknown tool=unknown started=$started ended=$ended" &&
		echo &&
		echo "First summary." &&
		echo &&
		echo "Second summary."
	} >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success '--amend of a commit without a record marks its lines UNKNOWN' '
	git checkout -b stock base &&
	echo 11 >>file &&
	git -c provenance.enabled=false commit -a -m stock &&
	test_must_fail git provenance show HEAD &&
	echo 12 >>file &&
	git commit -a --amend --no-edit &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 11 UNKNOWN - $(anchor HEAD file 11)" &&
		echo "+ 12 HUMAN human $(anchor HEAD file 12)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'cherry-pick carries the records to the new lines' '
	git checkout -b pick shift &&
	git cherry-pick ai &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 4 AI s1 $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "- 4 AI s1 $(anchor shift file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning ai >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD &&
	git tag picked
'

test_expect_success 'diff and show mark the lines of a rewritten commit' '
	git show --provenance picked >actual &&
	test_grep "^A-3$" actual &&
	test_grep "^A+three$" actual &&
	test_grep "^A+11$" actual &&
	git diff --provenance picked^ picked >actual.diff &&
	sed -n -e "/^@@/,\$p" actual >expect &&
	sed -n -e "/^@@/,\$p" actual.diff >actual &&
	test_cmp expect actual
'

test_expect_success 'cherry-pick -e records what an in-process pick does' '
	test_tick &&
	git checkout -b pick-edit shift &&
	GIT_EDITOR=true git cherry-pick -e ai &&
	same_records picked HEAD &&
	git provenance verify HEAD
'

test_expect_success 'setup a conflicting base' '
	git checkout -b conflict shift &&
	test_write_lines 0 1 2 tri 4 5 6 7 8 9 10 >file &&
	git commit -a -m tri &&
	git tag tri
'

test_expect_success 'a resolved conflict is HUMAN, or MIXED where it replaces AI lines' '
	git checkout -b resolve tri &&
	test_must_fail git cherry-pick ai &&
	test_write_lines 0 1 2 three-ish 4 5 6 7 8 9 10 11 >file &&
	git add file &&
	git cherry-pick --continue --no-edit &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 4 MIXED s1,human $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "- 4 MIXED s1,human $(anchor tri file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning ai >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD &&
	git tag resolved
'

test_expect_success 'a cherry-pick concluded with commit records the same' '
	test_tick &&
	git checkout -b resolved-commit tri &&
	test_must_fail git cherry-pick ai &&
	test_write_lines 0 1 2 three-ish 4 5 6 7 8 9 10 11 >file &&
	git add file &&
	git commit --no-edit &&
	same_records resolved HEAD &&
	git provenance verify HEAD
'

test_expect_success 'stamps made while resolving a conflict count' '
	test_tick &&
	git checkout -b resolved-stamped tri &&
	test_must_fail git cherry-pick ai &&
	test_write_lines 0 1 2 three-ish 4 5 6 7 8 9 10 11 >file &&
	git stamp begin --session s2 &&
	echo 12 >>file &&
	git stamp end --session s2 &&
	git commit -a --no-edit &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 4 MIXED s1,human $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "+ 13 AI s2 $(anchor HEAD file 13)" &&
		echo "- 4 MIXED s1,human $(anchor tri file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning HEAD >actual &&
	test_grep "^session s1 " actual &&
	test_grep "^session s2 " actual &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'revert restores the marks of the commits that wrote the lines' '
	git checkout -b revert ai &&
	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 HUMAN human $(anchor HEAD file 3)" &&
		echo "- 3 AI s1 $(anchor ai file 3)" &&
		echo "- 11 AI s1 $(anchor ai file 11)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD &&
	git tag reverted
'

test_expect_success 'revert -e records what an in-process revert does' '
	test_tick &&
	git checkout -b revert-edit ai &&
	GIT_EDITOR=true git revert -e HEAD &&
	same_records reverted HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a revert concluded with commit carries the marks' '
	git checkout -b revert-conflict ai &&
	test_write_lines 1 2 drei 4 5 6 7 8 9 10 11 >file &&
	git commit -a -m drei &&
	test_must_fail git revert ai &&
	test_write_lines 1 2 3 4 5 6 7 8 9 10 >file &&
	git add file &&
	git commit --no-edit &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 HUMAN human $(anchor HEAD file 3)" &&
		echo "- 3 MIXED human,s1 $(anchor HEAD^ file 3)" &&
		echo "- 11 AI s1 $(anchor HEAD^ file 11)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'revert restores a line that a session wrote in an older commit' '
	git checkout -b revert-older ai &&
	test_write_lines 1 2 4 5 6 7 8 9 10 11 >file &&
	git commit -a -m drop-three &&
	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1 $(anchor HEAD file 3)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a restored line from a commit without a record is UNKNOWN' '
	git checkout -b revert-unrecorded base &&
	echo 11 >>file &&
	git -c provenance.enabled=false commit -a -m stock-eleven &&
	test_write_lines 1 2 3 4 5 6 7 8 9 10 >file &&
	git commit -a -m drop-eleven &&
	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 11 UNKNOWN - $(anchor HEAD file 11)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'reverting a commit without a record restores the marks' '
	git checkout -b revert-stock ai &&
	test_write_lines 1 2 THREE 4 5 6 7 8 9 10 11 >file &&
	git -c provenance.enabled=false commit -a -m stock-three &&
	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1 $(anchor HEAD file 3)" &&
		echo "- 3 UNKNOWN - $(anchor HEAD^ file 3)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'revert follows renames to the commit that wrote a line' '
	git checkout -b revert-renamed ai &&
	git mv file moved &&
	git commit -m moved &&
	test_write_lines 1 2 4 5 6 7 8 9 10 11 >moved &&
	git commit -a -m drop-moved-three &&
	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file moved" &&
		echo "+ 3 AI s1 $(anchor HEAD moved 3)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'rebase picks carry the records' '
	test_tick &&
	git checkout -b rebased ai &&
	git rebase shift &&
	same_records picked HEAD &&
	git provenance verify HEAD
'

test_expect_success 'rebase --apply carries the records' '
	test_tick &&
	git checkout -b rebased-apply ai &&
	git rebase --apply shift &&
	same_records picked HEAD &&
	git provenance verify HEAD
'

test_expect_success 'reword keeps the records' '
	test_tick &&
	git checkout -b reworded ai &&
	(
		set_fake_editor &&
		FAKE_COMMIT_MESSAGE=reworded FAKE_LINES="reword 1" \
			git rebase -i shift
	) &&
	git log -1 --format=%s >actual &&
	echo reworded >expect &&
	test_cmp expect actual &&
	same_records picked HEAD &&
	git provenance verify HEAD
'

test_expect_success 'edit, then amend, merges the records' '
	git checkout -b edit ai &&
	(
		set_fake_editor &&
		FAKE_LINES="edit 1" git rebase -i shift
	) &&
	echo 12 >>file &&
	git commit -a --amend --no-edit &&
	git rebase --continue &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 4 AI s1 $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "+ 13 HUMAN human $(anchor HEAD file 13)" &&
		echo "- 4 AI s1 $(anchor shift file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD &&
	git tag edited
'

test_expect_success 'edit, then continue with staged changes, records the same' '
	test_tick &&
	git checkout -b edited-continue ai &&
	(
		set_fake_editor &&
		FAKE_LINES="edit 1" git rebase -i shift &&
		echo 12 >>file &&
		git add file &&
		git rebase --continue
	) &&
	same_records edited HEAD &&
	git provenance verify HEAD
'

test_expect_success 'setup commits to squash' '
	git checkout -b to-squash ai &&
	git stamp begin --session s2 &&
	test_write_lines 1 2 THREE 4 five 6 7 8 9 10 11 >file &&
	git stamp end --session s2 &&
	echo "Second summary." | git stamp reason --session s2 --summary - &&
	git stamp begin --session s1 &&
	test_write_lines 1 2 THREE 4 five 6 7 8 9 10 11 12 >file &&
	git stamp end --session s1 &&
	test_tick &&
	git commit -a -m second --session s2 &&
	git tag second &&
	git provenance show HEAD >actual &&
	test_grep "^actor s1 ai model=unknown tool=unknown session=s1 source=observed$" actual
'

test_expect_success 'squash unites the records' '
	git checkout -b squash second &&
	(
		set_fake_editor &&
		FAKE_LINES="1 squash 2" git rebase -i base
	) &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1,s2 $(anchor HEAD file 3)" &&
		echo "+ 5 AI s2 $(anchor HEAD file 5)" &&
		echo "+ 11-12 AI s1 $(anchor HEAD file 11,12)" &&
		echo "- 3 AI s1 $(anchor base file 3)" &&
		echo "- 5 AI s2 $(anchor base file 5)"
	} >expect &&
	check_lines HEAD &&
	{
		echo "version 1" &&
		echo "commit $(git rev-parse HEAD)" &&
		session_line ai s1 &&
		session_line second s2 &&
		echo &&
		echo "First summary." &&
		echo &&
		echo "Second summary."
	} >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD &&
	git tag squashed
'

test_expect_success 'fixup records what squash does' '
	test_tick &&
	git checkout -b fixed-up second &&
	(
		set_fake_editor &&
		FAKE_LINES="1 fixup 2" git rebase -i base
	) &&
	same_records squashed HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a fixup with conflicts carries the records of both commits' '
	git checkout -b tres ai &&
	test_write_lines 1 2 tres 4 5 6 7 8 9 10 11 >file &&
	git commit -a -m tres &&
	test_tick &&
	git checkout -b fixup-conflict second &&
	cat >todo.out <<-EOF &&
	pick $(git rev-parse ai)
	fixup $(git rev-parse tres)
	fixup $(git rev-parse second)
	EOF
	test_must_fail env GIT_SEQUENCE_EDITOR="cp todo.out" \
		git rebase -i base &&
	git cat-file blob second:file >file &&
	git add file &&
	GIT_EDITOR=true git rebase --continue &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$s2_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 3 MIXED s1,s2,human $(anchor HEAD file 3)" &&
		echo "+ 5 AI s2 $(anchor HEAD file 5)" &&
		echo "+ 11-12 AI s1 $(anchor HEAD file 11,12)" &&
		echo "- 3 AI s1 $(anchor base file 3)" &&
		echo "- 5 AI s2 $(anchor base file 5)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning HEAD >actual &&
	test_grep "^session s1 " actual &&
	test_grep "^session s2 " actual &&
	git provenance verify HEAD &&
	git tag fixup-conflicted
'

# Start "rebase -i base" with the todo list "pick ai", "fixup tres",
# "<command> second", which stops at the conflicting third command, and
# resolve the conflict.
#
# Usage: stop_at_fixup <branch> (fixup|squash)
stop_at_fixup () {
	test_tick &&
	git checkout -b "$1" second &&
	cat >todo.out <<-EOF &&
	pick $(git rev-parse ai)
	fixup $(git rev-parse tres)
	$2 $(git rev-parse second)
	EOF
	test_must_fail env GIT_SEQUENCE_EDITOR="cp todo.out" \
		git rebase -i base &&
	git cat-file blob second:file >file &&
	git add file
}

test_expect_success 'commit --amend at a conflicting fixup carries its records' '
	stop_at_fixup fixup-amended fixup &&
	git commit --amend --no-edit &&
	same_records fixup-conflicted HEAD &&
	git rebase --continue &&
	same_records fixup-conflicted HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/rebase-merge &&

	git commit --amend --no-edit &&
	same_records fixup-conflicted HEAD
'

test_expect_success 'commit at a conflicting fixup takes it as a pick' '
	stop_at_fixup fixup-committed fixup &&
	git commit -m second-on-its-own &&
	git rebase --continue &&
	test_cmp_rev base HEAD~2 &&
	{
		header HEAD &&
		echo "$s2_actor" &&
		echo "actor s1 ai model=unknown tool=unknown session=s1 source=observed" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 3 AI s2 $(anchor HEAD file 3)" &&
		echo "+ 5 AI s2 $(anchor HEAD file 5)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "- 3 MIXED s2,human $(anchor HEAD^ file 3)" &&
		echo "- 5 AI s2 $(anchor HEAD^ file 5)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning second >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'commit --amend at a conflicting squash carries its records' '
	stop_at_fixup squash-amended squash &&
	git commit --amend --no-edit &&
	git rebase --continue &&
	same_records fixup-conflicted HEAD &&
	git provenance verify HEAD
'

test_expect_success 'cherry-pick -n records its picks for the next commit' '
	git checkout -b pick-n shift &&
	git cherry-pick -n ai &&
	git rev-parse ai >expect &&
	test_cmp expect .git/provenance/picks &&
	git commit -m picked-n &&
	same_records picked HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/picks
'

test_expect_success 'several cherry-pick -n are carried by one commit' '
	git checkout -b pick-n-two shift &&
	git cherry-pick -n ai second &&
	git commit -m picked-two &&
	{
		header HEAD &&
		echo "$s2_actor" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 4 AI s2 $(anchor HEAD file 4)" &&
		echo "+ 6 AI s2 $(anchor HEAD file 6)" &&
		echo "+ 12-13 AI s1 $(anchor HEAD file 12,13)" &&
		echo "- 4 AI s1 $(anchor shift file 4)" &&
		echo "- 6 AI s2 $(anchor shift file 6)"
	} >expect &&
	check_lines HEAD &&
	{
		echo "version 1" &&
		echo "commit $(git rev-parse HEAD)" &&
		session_line ai s1 &&
		session_line second s2 &&
		echo &&
		echo "First summary." &&
		echo &&
		echo "Second summary."
	} >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a conflicting cherry-pick -n resolved by hand and by a session' '
	git checkout -b pick-n-conflict tri &&
	test_must_fail git cherry-pick -n ai &&
	git rev-parse ai >expect &&
	test_cmp expect .git/provenance/picks &&
	test_write_lines 0 1 2 three-ish 4 5 6 7 8 9 10 11 >file &&
	git stamp begin --session s2 &&
	echo 12 >>file &&
	git stamp end --session s2 &&
	git commit -a -m resolved-n &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 4 MIXED s1,human $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "+ 13 AI s2 $(anchor HEAD file 13)" &&
		echo "- 4 MIXED s1,human $(anchor tri file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning HEAD >actual &&
	test_grep "^session s1 " actual &&
	test_grep "^session s2 " actual &&
	test_grep "^First summary.$" actual &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/picks
'

test_expect_success 'a cherry-pick -n that was discarded lends nothing' '
	git checkout -b pick-n-stale shift &&
	git cherry-pick -n ai &&
	git reset --hard &&
	echo unrelated >other &&
	git add other &&
	git commit -m unrelated &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "file other" &&
		echo "+ 1 HUMAN human $(anchor HEAD other 1)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	test_path_is_missing .git/provenance/picks
'

test_expect_success 'commit --amend after cherry-pick -n unites the records' '
	git checkout -b pick-n-amend shift &&
	git cherry-pick -n ai &&
	git commit --amend --no-edit &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 1 HUMAN human $(anchor HEAD file 1)" &&
		echo "+ 4 AI s1 $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "- 3 AI s1 $(anchor base file 3)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning ai >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'stamps do not consume the picks' '
	git checkout -b pick-n-stamp shift &&
	git cherry-pick -n ai &&
	git stamp begin --session s2 &&
	echo 12 >>file &&
	git stamp end --session s2 &&
	test_path_is_file .git/provenance/picks &&
	git commit -a -m picked-stamped &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 4 AI s1 $(anchor HEAD file 4)" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "+ 13 AI s2 $(anchor HEAD file 13)" &&
		echo "- 4 AI s1 $(anchor shift file 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'cherry-pick -n records no picks when provenance is disabled' '
	git checkout -b pick-n-disabled shift &&
	git -c provenance.enabled=false cherry-pick -n ai &&
	test_path_is_missing .git/provenance/picks &&
	git reset --hard
'

test_expect_success 'rebases that fast-forward record nothing' '
	git checkout -b fast-forward ai &&
	git rev-parse refs/provenance/lines >before &&
	(
		set_fake_editor &&
		FAKE_LINES="1" git rebase -i base
	) &&
	git rev-parse HEAD >actual &&
	git rev-parse ai >expect &&
	test_cmp expect actual &&
	git rev-parse refs/provenance/lines >actual &&
	test_cmp before actual
'

test_expect_success 'rewrites of commits without records mark their lines UNKNOWN' '
	git checkout -b unrecorded-base base &&
	echo 11 >>file &&
	git -c provenance.enabled=false commit -a -m unrecorded &&
	git tag unrecorded &&
	git checkout -b unrecorded-pick shift &&
	git cherry-pick unrecorded &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 12 UNKNOWN - $(anchor HEAD file 12)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD &&
	git show --provenance HEAD >actual &&
	test_grep "^?+11$" actual &&
	git tag unrecorded-picked &&

	git revert --no-edit HEAD &&
	{
		header HEAD &&
		echo "file file" &&
		echo "- 12 UNKNOWN - $(anchor HEAD^ file 12)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD &&

	test_tick &&
	git checkout -b unrecorded-rebase unrecorded &&
	git rebase shift &&
	same_records unrecorded-picked HEAD &&
	test_tick &&
	git checkout -b unrecorded-apply unrecorded &&
	git rebase --apply shift &&
	same_records unrecorded-picked HEAD
'

test_expect_success 'squash marks the lines of a commit without a record UNKNOWN' '
	git checkout -b squash-unrecorded ai &&
	test_write_lines 1 2 three 4 five 6 7 8 9 10 11 12 >file &&
	git -c provenance.enabled=false commit -a -m unrecorded-on-ai &&
	(
		set_fake_editor &&
		FAKE_LINES="1 squash 2" git rebase -i base
	) &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1 $(anchor HEAD file 3)" &&
		echo "+ 5 UNKNOWN - $(anchor HEAD file 5)" &&
		echo "+ 11 AI s1 $(anchor HEAD file 11)" &&
		echo "+ 12 UNKNOWN - $(anchor HEAD file 12)" &&
		echo "- 3 AI s1 $(anchor base file 3)" &&
		echo "- 5 UNKNOWN - $(anchor base file 5)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning ai >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'setup a conflict with a commit without a record' '
	git checkout -b unrecorded-five base &&
	test_write_lines 1 2 3 4 five 6 7 8 9 10 >file &&
	git -c provenance.enabled=false commit -a -m five &&
	git checkout -b FIVE base &&
	test_write_lines 1 2 3 4 FIVE 6 7 8 9 10 >file &&
	git commit -a -m FIVE
'

test_expect_success 'an UNKNOWN line edited in a conflict resolution stays UNKNOWN' '
	git checkout -b unknown-resolve FIVE &&
	test_must_fail git cherry-pick unrecorded-five &&
	test_write_lines 1 2 3 4 five-ish 6 7 8 9 10 >file &&
	git add file &&
	git cherry-pick --continue --no-edit &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 5 UNKNOWN - $(anchor HEAD file 5)" &&
		echo "- 5 UNKNOWN - $(anchor HEAD^ file 5)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD &&
	git tag unknown-resolved
'

test_expect_success 'an UNKNOWN line rewritten by a session stays UNKNOWN' '
	test_tick &&
	git checkout -b unknown-stamped FIVE &&
	test_must_fail git cherry-pick unrecorded-five &&
	test_write_lines 1 2 3 4 five 6 7 8 9 10 >file &&
	git stamp begin --session s2 &&
	test_write_lines 1 2 3 4 five-ish 6 7 8 9 10 >file &&
	git stamp end --session s2 &&
	echo "Second summary." | git stamp reason --session s2 --summary - &&
	git commit -a --no-edit &&
	same_records unknown-resolved HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'UNKNOWN ranges of a record are carried as UNKNOWN' '
	git checkout -b adopted base &&
	test_write_lines 1 2 3 4 5 6 7 8 9 10 11 12 >file &&
	git -c provenance.enabled=false commit -a -m adopted &&
	{
		echo "version 1" &&
		echo "commit $(git rev-parse HEAD)" &&
		echo "capture adopted" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 11 AI s1 $(anchor HEAD file 11)" &&
		echo "+ 12 UNKNOWN - $(anchor HEAD file 12)"
	} >adopted.out &&
	add_record lines HEAD adopted.out &&
	git provenance verify HEAD &&
	git checkout -b adopted-pick shift &&
	git cherry-pick adopted &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 12 AI s1 $(anchor HEAD file 12)" &&
		echo "+ 13 UNKNOWN - $(anchor HEAD file 13)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a record that does not parse is ignored with a warning' '
	git checkout -b bad-base base &&
	echo 11 >>file &&
	git -c provenance.enabled=false commit -a -m bad &&
	echo "version 2" >bad &&
	add_record lines HEAD bad &&
	git checkout -b bad-pick shift &&
	git cherry-pick bad-base 2>err &&
	test_grep "ignoring the line record of $(git rev-parse bad-base)" err &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 12 UNKNOWN - $(anchor HEAD file 12)"
	} >expect &&
	check_lines HEAD
'

test_expect_success 'nothing is recorded when provenance is disabled' '
	git checkout -b disabled shift &&
	git rev-parse refs/provenance/lines >before &&
	git -c provenance.enabled=false cherry-pick ai &&
	git -c provenance.enabled=false commit --amend --no-edit &&
	test_must_fail git provenance show HEAD &&
	git checkout -b disabled-rebase ai &&
	git -c provenance.enabled=false rebase shift &&
	test_must_fail git provenance show HEAD &&
	git -c provenance.enabled=false rebase --apply --force-rebase base &&
	test_must_fail git provenance show HEAD &&
	git rev-parse refs/provenance/lines >actual &&
	test_cmp before actual
'

test_done
