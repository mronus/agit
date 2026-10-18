#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test provenance records written by git commit'

GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Print the anchor of lines <a> to <b> of <rev>:<path>.
#
# Usage: anchor <rev> <path> <a>[,<b>]
anchor () {
	git cat-file blob "$1:$2" >anchor.tmp &&
	sed -n "$3p" anchor.tmp | git hash-object --stdin &&
	rm -f anchor.tmp
}

# Print the first lines of the line record of <rev>.
#
# Usage: header <rev> [declared]
header () {
	echo "version 1" &&
	echo "commit $(git rev-parse "$1")" &&
	if test "$2" = declared
	then
		echo "capture declared"
	else
		echo "capture observed git/$git_version"
	fi
}

human_actor="actor human human ident=$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>"

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

# Print the time of the first <type> stamp of <session>, or the last
# one with "last".
#
# Usage: stamp_time (begin|end) <session> [last]
stamp_time () {
	git stamp list --session "$2" >stamps.out &&
	sed -n -e "s/^$1 \([0-9]*\) .*/\1/p" stamps.out >times.out &&
	if test "$3" = last
	then
		tail -n 1 times.out
	else
		head -n 1 times.out
	fi
}

check_advice () {
	test_grep "^hint: no session is linked to this commit; if the change came from an$" "$1" &&
	test_grep "^hint: LLM session, provide its details with --session$" "$1"
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	/expect*
	/actual*
	/err
	/pre
	/before
	/file.new
	/patch
	/repo*
	EOF
	git version >version.out &&
	git_version=$(sed -n -e "s/^git version //p" version.out) &&
	test_write_lines 1 2 3 4 5 >file &&
	git add file &&
	test_tick &&
	git commit -m initial 2>err &&
	check_advice err &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 1-5 HUMAN human $(git rev-parse HEAD:file)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'provenance is enabled by default' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		sane_unset GIT_TEST_PROVENANCE &&
		test_commit one &&
		git rev-parse --verify refs/provenance/lines
	)
'

test_expect_success 'GIT_TEST_PROVENANCE=false disables provenance' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		GIT_TEST_PROVENANCE=false &&
		export GIT_TEST_PROVENANCE &&
		test_commit one 2>err &&
		test_must_fail git rev-parse --verify -q refs/provenance/lines &&
		test_grep ! "hint: no session" err
	)
'

test_expect_success 'provenance.enabled overrides GIT_TEST_PROVENANCE' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		GIT_TEST_PROVENANCE=false &&
		export GIT_TEST_PROVENANCE &&
		git config provenance.enabled true &&
		test_commit one &&
		git rev-parse --verify refs/provenance/lines
	) &&
	git init repo2 &&
	test_when_finished "rm -rf repo2" &&
	(
		cd repo2 &&
		git config provenance.enabled false &&
		test_commit one 2>err &&
		test_must_fail git rev-parse --verify -q refs/provenance/lines &&
		test_grep ! "hint: no session" err
	)
'

test_expect_success 'human changes are HUMAN' '
	test_write_lines 1 two 3 4 5 6 >file &&
	git commit -a -m human 2>err &&
	check_advice err &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 2 HUMAN human $(anchor HEAD file 2)" &&
		echo "+ 6 HUMAN human $(anchor HEAD file 6)" &&
		echo "- 2 HUMAN human $(anchor HEAD^ file 2)"
	} >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'advice.provenanceSession silences the advice' '
	test_config advice.provenanceSession false &&
	echo quiet >quiet &&
	git add quiet &&
	git commit -m quiet 2>err &&
	test_grep ! "hint:" err
'

test_expect_success 'an empty commit gets a record without files' '
	git commit --allow-empty -m empty 2>err &&
	test_grep ! "hint:" err &&
	header HEAD >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'provenance.require refuses a commit without a session' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		git config provenance.require true &&
		echo one >file &&
		git add file &&
		test_must_fail git commit -m one 2>err &&
		test_grep "no session is linked to this commit and provenance.require is set" err &&
		test_must_fail git rev-parse --verify -q HEAD &&
		test_must_fail git rev-parse --verify -q refs/provenance/lines &&
		git commit -m one --session s1 2>err &&
		git rev-parse --verify refs/provenance/lines &&
		git rev-parse --verify refs/provenance/reasoning &&
		git commit --allow-empty -m empty &&
		git provenance show HEAD >actual &&
		test_grep ! "^file " actual
	)
'

test_expect_success 'begin and end stamps make lines AI' '
	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines 1 two 3 four 5 6 7 >file &&
	git stamp end --session s1 &&
	echo "The summary." | git stamp reason --session s1 --summary - &&
	started=$(stamp_time begin s1) &&
	ended=$(stamp_time end s1 last) &&
	git commit -a -m ai --session s1 2>err &&
	test_grep ! "hint:" err &&
	{
		header HEAD &&
		echo "actor s1 ai model=m1 tool=t1 session=s1 source=observed" &&
		echo "file file" &&
		echo "+ 4 AI s1 $(anchor HEAD file 4)" &&
		echo "+ 7 AI s1 $(anchor HEAD file 7)" &&
		echo "- 4 AI s1 $(anchor HEAD^ file 4)"
	} >expect &&
	check_lines HEAD &&
	cat >expect <<-EOF &&
	version 1
	commit $(git rev-parse HEAD)
	session s1 model=m1 tool=t1 started=$started ended=$ended

	The summary.
	EOF
	check_reasoning HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal &&
	git stamp list >actual &&
	test_must_be_empty actual
'

test_expect_success 'stamps without --session give no advice' '
	git stamp begin --session s1 &&
	echo 8 >>file &&
	git stamp end --session s1 &&
	git commit -a -m ai 2>err &&
	test_grep ! "hint:" err &&
	git provenance verify HEAD
'

test_expect_success '--session must be valid' '
	test_must_fail git commit --allow-empty -m invalid \
		--session "has space" 2>err &&
	test_grep "invalid session id .has space." err
'

test_expect_success 'lines edited by AI and human are MIXED' '
	test_write_lines 1 two 3 four five 6 7 8 >file &&
	git stamp begin --session s2 &&
	test_write_lines one two 3 four FIVE 6 7 8 9 >file &&
	git stamp end --session s2 &&
	started=$(stamp_time begin s2) &&
	ended=$(stamp_time end s2) &&
	test_write_lines one two 3 four FIVE 6 7 8 nine >file &&
	git commit -a -m mixed &&
	{
		header HEAD &&
		echo "actor s2 ai model=unknown tool=unknown session=s2 source=observed" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 1 AI s2 $(anchor HEAD file 1)" &&
		echo "+ 5 MIXED s2,human $(anchor HEAD file 5)" &&
		echo "+ 9 MIXED s2,human $(anchor HEAD file 9)" &&
		echo "- 1 AI s2 $(anchor HEAD^ file 1)" &&
		echo "- 5 HUMAN human $(anchor HEAD^ file 5)"
	} >expect &&
	check_lines HEAD &&
	cat >expect <<-EOF &&
	version 1
	commit $(git rev-parse HEAD)
	session s2 model=unknown tool=unknown started=$started ended=$ended

	EOF
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'two sessions on one line stay AI' '
	git stamp begin --session s3 --model m3 &&
	test_write_lines one two four FIVE 6 7 8 nine x >file &&
	git stamp end --session s3 &&
	git stamp begin --session s4 --tool t4 &&
	test_write_lines one two four FIVE 6 7 8 nine X >file &&
	git stamp end --session s4 &&
	git commit -a -m two-sessions &&
	{
		header HEAD &&
		echo "actor s3 ai model=m3 tool=unknown session=s3 source=observed" &&
		echo "actor s4 ai model=unknown tool=t4 session=s4 source=observed" &&
		echo "file file" &&
		echo "+ 9 AI s3,s4 $(anchor HEAD file 9)" &&
		echo "- 3 AI s3 $(anchor HEAD^ file 3)"
	} >expect &&
	check_lines HEAD &&
	git provenance show --reasoning HEAD >actual &&
	test_grep "^session s3 model=m3 tool=unknown " actual &&
	test_grep "^session s4 model=unknown tool=t4 " actual &&
	git provenance verify HEAD
'

test_expect_success 'edit stamps make lines AI' '
	cp file pre &&
	test_write_lines one two four FIVE 6 seven 8 nine X >file &&
	git stamp edit --session s5 --path file --pre pre &&
	git commit -a -m edit &&
	{
		header HEAD &&
		echo "actor s5 ai model=unknown tool=unknown session=s5 source=observed" &&
		echo "file file" &&
		echo "+ 6 AI s5 $(anchor HEAD file 6)" &&
		echo "- 6 AI s5 $(anchor HEAD^ file 6)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'new and deleted files' '
	test_write_lines a b c >human.txt &&
	git add human.txt &&
	git stamp begin --session s6 &&
	test_write_lines x y >ai.txt &&
	git stamp end --session s6 &&
	git add ai.txt &&
	git commit -m new-files &&
	{
		header HEAD &&
		echo "actor s6 ai model=unknown tool=unknown session=s6 source=observed" &&
		echo "$human_actor" &&
		echo "file ai.txt" &&
		echo "+ 1-2 AI s6 $(git rev-parse HEAD:ai.txt)" &&
		echo "file human.txt" &&
		echo "+ 1-3 HUMAN human $(git rev-parse HEAD:human.txt)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD &&

	git rm -q human.txt &&
	git stamp begin --session s7 &&
	rm ai.txt &&
	git stamp end --session s7 &&
	git commit -a -m deleted-files &&
	{
		header HEAD &&
		echo "actor s7 ai model=unknown tool=unknown session=s7 source=observed" &&
		echo "$human_actor" &&
		echo "file ai.txt" &&
		echo "- 1-2 AI s7 $(git rev-parse HEAD^:ai.txt)" &&
		echo "file human.txt" &&
		echo "- 1-3 HUMAN human $(git rev-parse HEAD^:human.txt)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'binary files and symlinks are skipped' '
	printf "a\000b" >binary &&
	git add binary &&
	test_ln_s_add file link &&
	echo 10 >>file &&
	git commit -a -m skipped &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 10 HUMAN human $(anchor HEAD file 10)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success '--session without stamps declares everything' '
	echo 11 >>file &&
	echo b >>binary &&
	git commit -a -m declared --session s8 2>err &&
	test_grep "session .s8. has no stamps in the journal" err &&
	test_grep ! "hint:" err &&
	{
		header HEAD declared &&
		echo "actor s8 ai model=unknown tool=unknown session=s8 source=declared" &&
		echo "file file" &&
		echo "+ 11 AI s8 $(anchor HEAD file 11)"
	} >expect &&
	check_lines HEAD &&
	cat >expect <<-EOF &&
	version 1
	commit $(git rev-parse HEAD)
	session s8 model=unknown tool=unknown

	EOF
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'claims declare the claimed paths only' '
	test_write_lines p q >other &&
	git add other &&
	git commit -m other &&
	echo 12 >>file &&
	echo r >>other &&
	git stamp claim --session s9 other &&
	git commit -a -m claimed 2>err &&
	test_grep ! "has no stamps" err &&
	{
		header HEAD declared &&
		echo "$human_actor" &&
		echo "actor s9 ai model=unknown tool=unknown session=s9 source=declared" &&
		echo "file file" &&
		echo "+ 12 HUMAN human $(anchor HEAD file 12)" &&
		echo "file other" &&
		echo "+ 3 AI s9 $(anchor HEAD other 3)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'claims of a patch declare the paths it names' '
	echo 13 >>file &&
	echo s >>other &&
	git diff -- other >patch &&
	git stamp claim --session s10 --patch patch &&
	git commit -a -m claimed-patch --session s10 &&
	{
		header HEAD declared &&
		echo "$human_actor" &&
		echo "actor s10 ai model=unknown tool=unknown session=s10 source=declared" &&
		echo "file file" &&
		echo "+ 13 HUMAN human $(anchor HEAD file 13)" &&
		echo "file other" &&
		echo "+ 4 AI s10 $(anchor HEAD other 4)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'partial commits keep the journal' '
	git stamp begin --session s11 &&
	echo 14 >>file &&
	echo t >>other &&
	git stamp end --session s11 &&
	git commit -m partial file &&
	test_path_is_file .git/provenance/journal &&
	git commit -m only --only other &&
	test_path_is_file .git/provenance/journal &&
	git commit --allow-empty -m full &&
	test_path_is_missing .git/provenance/journal &&
	git provenance verify HEAD~2 HEAD~1 HEAD
'

test_expect_success 'provenance.enabled=false leaves the journal alone' '
	git stamp begin --session s12 &&
	echo 15 >>file &&
	git stamp end --session s12 &&
	git -c provenance.enabled=false commit -a -m disabled 2>err &&
	test_grep ! "hint:" err &&
	test_must_fail git provenance show HEAD &&
	test_path_is_file .git/provenance/journal &&
	git stamp list --session s12 >actual &&
	test_line_count = 2 actual &&
	rm .git/provenance/journal
'

test_expect_success '--amend carries the records of the amended commit over' '
	git stamp begin --session s13 &&
	echo 16 >>file &&
	git stamp end --session s13 &&
	git commit -a -m to-amend --session s13 &&
	git provenance show HEAD >before &&
	git provenance show --reasoning HEAD >before-reasoning &&
	git commit --amend -m amended &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" before >expect &&
	check_lines HEAD &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" \
		before-reasoning >expect &&
	check_reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a conflicted merge concluded with commit records its resolution' '
	test_write_lines 1 2 3 4 5 6 7 8 9 >merged &&
	git add merged &&
	git commit -m merged &&
	git checkout -b side &&
	test_write_lines 1 S2 3 4 6 7 8 9 side >merged &&
	git commit -a -m side &&
	git checkout - &&
	test_write_lines 1 M2 3 4 5 6 7 8 9 >merged &&
	git commit -a -m main &&
	test_must_fail git merge side &&
	test_write_lines 1 R2 3 4 6 7 8 9 side >merged &&
	git stamp begin --session s15 --model m15 --tool t15 &&
	test_write_lines 1 R2 3 4 6 7 8 9 SIDE >merged &&
	git stamp end --session s15 &&
	git commit -a --no-edit 2>err &&
	test_grep ! "hint:" err &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "actor s15 ai model=m15 tool=t15 session=s15 source=observed" &&
		echo "file merged" &&
		echo "+ 2 HUMAN human $(anchor HEAD merged 2)" &&
		echo "+ 9 AI s15 $(anchor HEAD merged 9)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success '--amend of a merge keeps its record' '
	git provenance show HEAD >before &&
	git commit --amend -m amended-merge &&
	git rev-parse HEAD^2 &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" before >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

# Make a repository "repo" whose branches "side" and "main" change line
# 2 of "file", so that merging them conflicts; "side" also deletes line
# 5 and adds "side" at the end, which merge cleanly.
setup_merge () {
	git init --initial-branch=main repo &&
	(
		cd repo &&
		test_write_lines 1 2 3 4 5 6 7 8 9 >file &&
		git add file &&
		git commit -m base &&
		git checkout -b side &&
		test_write_lines 1 S2 3 4 6 7 8 9 side >file &&
		git commit -a -m side &&
		git checkout - &&
		test_write_lines 1 M2 3 4 5 6 7 8 9 >file &&
		git commit -a -m main
	)
}

test_expect_success 'provenance.require refuses an unlinked conflict resolution' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git config provenance.require true &&
		test_must_fail git merge side &&
		test_write_lines 1 R2 3 4 6 7 8 9 side >file &&
		test_must_fail git commit -a --no-edit 2>err &&
		test_grep "no session is linked to this commit and provenance.require is set" err &&
		git rev-parse --verify MERGE_HEAD &&
		git commit -a --no-edit --session s1 &&
		git rev-parse --verify HEAD^2
	)
'

test_expect_success 'a merge that resolves nothing gets a record without files' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git checkout -b other side^ &&
		test_write_lines 1 2 3 4 5 6 7 8 nine >file &&
		git commit -a -m other &&
		git checkout - &&
		git config provenance.require true &&
		git merge --no-commit other &&
		git commit --no-edit 2>err &&
		test_grep ! "hint:" err &&
		git rev-parse --verify HEAD^2 &&
		header HEAD >expect &&
		check_lines HEAD &&
		git provenance verify HEAD
	)
'

test_expect_success 'an octopus merge records what it resolves' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git checkout -b one side^ &&
		test_write_lines 1 2 3 4 B5 6 7 8 9 >file &&
		git commit -a -m one &&
		git checkout -b two side^ &&
		test_write_lines 1 2 3 4 5 6 7 8 B9 >file &&
		git commit -a -m two &&
		git checkout main &&
		git merge --no-commit one two &&
		test_write_lines 1 M2 3 4 R5 6 7 8 B9 >file &&
		git commit -a --no-edit &&
		git rev-parse --verify HEAD^3 &&
		{
			header HEAD &&
			echo "$human_actor" &&
			echo "file file" &&
			echo "+ 5 HUMAN human $(anchor HEAD file 5)"
		} >expect &&
		check_lines HEAD &&
		git provenance verify HEAD
	)
'

test_expect_success 'merge records the merge commits it makes' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git checkout -b other side^ &&
		test_write_lines 1 2 3 4 5 6 7 8 nine >file &&
		git commit -a -m other &&
		git checkout main &&
		git config provenance.require true &&
		git merge --no-edit other 2>err &&
		test_grep ! "hint:" err &&
		git rev-parse --verify HEAD^2 &&
		header HEAD >expect &&
		check_lines HEAD &&
		git provenance verify HEAD &&
		git reflog -1 HEAD >actual &&
		test_grep "Merge made by the .ort. strategy" actual
	)
'

test_expect_success 'merge records nothing without a merge commit' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git checkout -b other side^ &&
		test_write_lines 1 2 3 4 5 6 7 8 nine >file &&
		git commit -a -m other &&
		git checkout main &&
		git rev-parse refs/provenance/lines >before &&
		git merge --no-commit other &&
		git rev-parse --verify MERGE_HEAD &&
		git rev-parse refs/provenance/lines >actual &&
		test_cmp before actual &&
		git merge --abort &&
		git checkout -b ahead side &&
		git checkout -b behind side^ &&
		git merge --ff-only ahead &&
		git rev-parse refs/provenance/lines >actual &&
		test_cmp before actual
	)
'

test_expect_success 'merge --squash carries the records of the squashed commits' '
	test_when_finished "rm -rf repo" &&
	setup_merge &&
	(
		cd repo &&
		git checkout -b topic main &&
		git stamp begin --session s1 &&
		test_write_lines 1 M2 3 4 5 6 7 8 9 ten >file &&
		git stamp end --session s1 &&
		git commit -a -m ten &&
		git stamp begin --session s2 &&
		test_write_lines 1 M2 3 4 5 6 7 8 nine ten >file &&
		git stamp end --session s2 &&
		git commit -a -m nine &&
		git checkout -b picked main &&
		echo other >other &&
		git add other &&
		git commit -m other &&
		git cherry-pick --no-commit topic~2..topic &&
		git commit -m picked &&
		git checkout main &&
		git reset --hard picked^ &&
		git merge --squash topic &&
		test_path_is_file .git/provenance/picks &&
		git commit --no-edit &&
		test_path_is_missing .git/provenance/picks &&
		git provenance show picked >picked.out &&
		grep -v "^commit " picked.out >expect &&
		git provenance show HEAD >actual.out &&
		grep -v "^commit " actual.out >actual &&
		test_cmp expect actual &&
		test_grep "^+ 9 AI s2 " actual &&
		test_grep "^+ 10 AI s1 " actual &&
		git provenance show --reasoning picked >picked.out &&
		grep -v "^commit " picked.out >expect &&
		git provenance show --reasoning HEAD >actual.out &&
		grep -v "^commit " actual.out >actual &&
		test_cmp expect actual &&
		git provenance verify HEAD
	)
'

test_expect_success 'a failed commit leaves no provenance refs' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		echo one >file &&
		git add file &&
		test_must_fail env GIT_EDITOR=true git commit 2>err &&
		test_grep "empty commit message" err &&
		write_script .git/hooks/pre-commit <<-\EOF &&
		exit 1
		EOF
		test_must_fail git commit -m one &&
		test_must_fail git rev-parse --verify -q HEAD &&
		test_must_fail git rev-parse --verify -q refs/provenance/lines &&
		test_must_fail git rev-parse --verify -q refs/provenance/reasoning &&
		rm .git/hooks/pre-commit &&
		git commit -m one --session s1 &&
		git rev-parse --verify refs/provenance/lines &&
		git rev-parse --verify refs/provenance/reasoning
	)
'

test_expect_success REFFILES 'HEAD does not move if a provenance ref cannot' '
	git rev-parse HEAD >head.out &&
	git rev-parse refs/provenance/lines >lines.out &&
	touch .git/refs/provenance/lines.lock &&
	test_when_finished "rm -f .git/refs/provenance/lines.lock" &&
	test_must_fail git commit --allow-empty -m locked &&
	git rev-parse HEAD >actual &&
	test_cmp head.out actual &&
	git rev-parse refs/provenance/lines >actual &&
	test_cmp lines.out actual
'

test_expect_success 'missing journal objects are skipped with a warning' '
	bogus=$(echo bogus | git hash-object --stdin) &&
	mkdir -p .git/provenance &&
	echo "end 1234567890 s13 $bogus" >.git/provenance/journal &&
	echo 17 >>file &&
	git commit -a -m missing 2>err &&
	test_grep "ignoring journal entry .end 1234567890 s13 $bogus.: cannot read object $bogus" err &&
	test_grep "hint: no session" err &&
	git provenance show HEAD >actual &&
	test_grep "^+ [0-9]* HUMAN human " actual &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'every commit made with provenance has a valid record' '
	git rev-list HEAD >commits.out &&
	git provenance verify $(cat commits.out) >actual &&
	echo "$(git rev-parse HEAD^{/^disabled}) no record" >expect &&
	grep -v " ok$" actual >actual.out &&
	test_cmp expect actual.out
'

test_expect_success 'worktrees have journals of their own' '
	git worktree add wt &&
	test_when_finished "git worktree remove --force wt" &&
	git -C wt stamp begin --session s14 &&
	echo 18 >>wt/file &&
	git -C wt stamp end --session s14 &&
	git commit --allow-empty -m main &&
	git -C wt stamp list --session s14 >actual &&
	test_line_count = 2 actual &&
	git -C wt commit -a -m wt &&
	git -C wt stamp list >actual &&
	test_must_be_empty actual &&
	git provenance show wt >actual &&
	test_grep "^+ [0-9]* AI s14 " actual &&
	git provenance verify wt
'

test_done
