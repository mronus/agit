#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test carrying provenance records in patch mails and streams'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME
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
header () {
	echo "version 1" &&
	echo "commit $(git rev-parse "$1")" &&
	echo "capture observed git/$git_version"
}

human_actor="actor human human ident=$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>"
s1_actor="actor s1 ai model=m1 tool=t1 session=s1 source=observed"

# Compare the line record of <rev> with the file "expect".
check_lines () {
	git provenance show "$1" >actual &&
	test_cmp expect actual
}

# Print the "Provenance:" block that a mail carries for <rev>.
record_block () {
	git provenance show "$1" >record.out &&
	echo "Provenance:" &&
	sed -e "s/^/    /" record.out &&
	echo
}

# Print the "Provenance:" blocks of the mails in <file>.
mail_blocks () {
	sed -n -e "/^Provenance:\$/,/^\$/p" "$1"
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	*.mbox
	/expect*
	/actual*
	/err
	EOF
	git version >version.out &&
	git_version=$(sed -n -e "s/^git version //p" version.out) &&
	test_write_lines a b c d e 1 2 3 4 5 6 7 8 9 10 >file &&
	git add file &&
	test_tick &&
	git commit -m base &&
	git tag base &&

	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines a b c d e 1 2 three 4 5 6 7 8 9 10 11 >file &&
	git stamp end --session s1 &&
	test_write_lines a b c d e 1 2 three 4 5 6 seven 8 9 10 11 >file &&
	test_tick &&
	git commit -a -m ai &&
	git tag ai &&
	{
		header ai &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 8 AI s1 $(anchor ai file 8)" &&
		echo "+ 12 HUMAN human $(anchor ai file 12)" &&
		echo "+ 16 AI s1 $(anchor ai file 16)" &&
		echo "- 8 AI s1 $(anchor base file 8)" &&
		echo "- 12 HUMAN human $(anchor base file 12)"
	} >expect &&
	check_lines ai &&

	test_write_lines a b c d e 1 2 three 4 5 6 seven 8 9 10 11 12 >file &&
	test_tick &&
	git -c provenance.enabled=false commit -a -m stock &&
	git tag stock &&
	test_must_fail git provenance show stock
'

test_expect_success 'format-patch puts the line record after the three dashes' '
	git format-patch -1 --stdout ai >ai.mbox &&
	sed -n -e "/^---\$/,/^ file |/p" ai.mbox >actual &&
	{
		echo "---" &&
		record_block ai &&
		echo " file | 5 +++--"
	} >expect &&
	test_cmp expect actual
'

test_expect_success 'format-patch --provenance with --no-stat starts the commentary' '
	git format-patch -1 --stdout --no-stat ai >ai-nostat.mbox &&
	sed -n -e "/^---\$/,/^diff /p" ai-nostat.mbox >actual &&
	{
		echo "---" &&
		record_block ai &&
		echo "diff --git a/file b/file"
	} >expect &&
	test_cmp expect actual
'

test_expect_success 'a commit without a line record gets no block' '
	git format-patch -1 --stdout stock >stock.mbox &&
	test_grep ! "^Provenance:" stock.mbox
'

test_expect_success 'the block keeps the paths that the patch shows' '
	git checkout -b two ai &&
	echo one >other &&
	echo 12 >>file &&
	git add file other &&
	test_tick &&
	git commit -m two &&
	git format-patch -1 --stdout two -- other >two.mbox &&
	mail_blocks two.mbox >actual &&
	test_grep "^    file other\$" actual &&
	test_grep ! "^    file file\$" actual &&
	git format-patch -1 --stdout two -- file >two.mbox &&
	mail_blocks two.mbox >actual &&
	test_grep ! "^    file other\$" actual &&
	test_grep "^    file file\$" actual
'

test_expect_success 'an empty commit gets no block' '
	test_tick &&
	git commit --allow-empty -m empty &&
	git provenance show HEAD &&
	git format-patch -1 --stdout --always HEAD >empty.mbox &&
	test_grep ! "^Provenance:" empty.mbox &&
	git checkout main
'

test_expect_success 'one block per mail, and none in the cover letter' '
	git format-patch --stdout --cover-letter base..stock >series.mbox &&
	sed -e "/^Subject: \[PATCH 1\/2\]/q" series.mbox >cover.out &&
	test_grep "^Subject: \[PATCH 0/2\]" cover.out &&
	test_grep ! "^Provenance:" cover.out &&
	record_block ai >expect &&
	mail_blocks series.mbox >actual &&
	test_cmp expect actual &&
	git format-patch -o out --cover-letter base..ai >files.out &&
	test_grep ! "^Provenance:" out/0000-cover-letter.patch &&
	mail_blocks out/0001-ai.patch >actual &&
	test_cmp expect actual
'

test_expect_success '--no-provenance and format.provenance control the block' '
	git format-patch -1 --stdout --no-provenance ai >actual &&
	test_grep ! "^Provenance:" actual &&
	git -c format.provenance=false format-patch -1 --stdout ai >actual &&
	test_grep ! "^Provenance:" actual &&
	git -c format.provenance=false format-patch -1 --stdout \
		--provenance ai >actual &&
	test_grep "^Provenance:" actual &&
	git -c format.provenance=true format-patch -1 --stdout \
		--no-provenance ai >actual &&
	test_grep ! "^Provenance:" actual
'

test_expect_success 'format.provenance defaults to provenance.enabled' '
	git -c provenance.enabled=false format-patch -1 --stdout ai >actual &&
	test_grep ! "^Provenance:" actual &&
	git -c provenance.enabled=false -c format.provenance=true \
		format-patch -1 --stdout ai >actual &&
	test_grep "^Provenance:" actual &&
	git -c provenance.enabled=false format-patch -1 --stdout \
		--provenance ai >actual &&
	test_grep "^Provenance:" actual
'

test_expect_success 'the block follows the notes' '
	test_when_finished "git notes remove ai" &&
	git notes add -m "a note" ai &&
	git format-patch -1 --stdout --notes ai >notes.mbox &&
	sed -n -e "/^---\$/,/^ file |/p" notes.mbox >actual &&
	{
		echo "---" &&
		echo &&
		echo "Notes:" &&
		echo "    a note" &&
		echo &&
		record_block ai &&
		echo " file | 5 +++--"
	} >expect &&
	test_cmp expect actual
'

test_expect_success 'log and show do not print the block' '
	git log -1 --format=email ai >actual &&
	test_grep ! "^Provenance:" actual &&
	git show --format=email ai >actual &&
	test_grep ! "^Provenance:" actual
'

test_expect_success 'git am and git apply take a mail with the block' '
	git checkout --detach base &&
	git -c provenance.enabled=false am ai.mbox &&
	test_cmp_rev ai^{tree} HEAD^{tree} &&
	git cat-file commit HEAD >commit.out &&
	sed -e "1,/^\$/d" commit.out >actual &&
	echo ai >expect &&
	test_cmp expect actual &&
	test_must_fail git provenance show HEAD &&
	git checkout --detach base &&
	git apply --index ai.mbox &&
	git diff --cached --exit-code ai &&
	git reset --hard
'

test_expect_success 'am records the mailed record for the commit it makes' '
	git checkout --detach base &&
	test_tick &&
	git -c provenance.require=true am ai.mbox 2>err &&
	test_grep ! "session" err &&
	git provenance show ai >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_lines HEAD &&
	test_must_fail git provenance show --reasoning HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am locates the ranges of a patch applied with an offset' '
	git checkout -b shifted base &&
	test_write_lines a 0 b c d e 1 2 3 4 5 6 7 8 9 10 >file &&
	test_tick &&
	git commit -a -m shift &&
	git tag shift &&
	test_tick &&
	git am ai.mbox &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 9 AI s1 $(anchor HEAD file 9)" &&
		echo "+ 13 HUMAN human $(anchor HEAD file 13)" &&
		echo "+ 17 AI s1 $(anchor HEAD file 17)" &&
		echo "- 9 AI s1 $(anchor shift file 9)" &&
		echo "- 13 HUMAN human $(anchor shift file 13)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am -3 keeps the block across a conflict resolved by hand' '
	git checkout -b resolved shift &&
	test_write_lines a 0 b c d e 1 2 tri 4 5 6 7 8 9 10 >file &&
	test_tick &&
	git commit -a -m tri &&
	git tag tri &&
	test_must_fail git am -3 ai.mbox &&
	test_path_is_file .git/rebase-apply/provenance &&
	test_write_lines a 0 b c d e 1 2 three-ish 4 5 6 seven 8 9 10 11 >file &&
	git add file &&
	test_tick &&
	git am --continue &&
	test_path_is_missing .git/rebase-apply &&
	{
		header HEAD &&
		echo "$human_actor" &&
		echo "$s1_actor" &&
		echo "file file" &&
		echo "+ 9 UNKNOWN - $(anchor HEAD file 9)" &&
		echo "+ 13 HUMAN human $(anchor HEAD file 13)" &&
		echo "+ 17 AI s1 $(anchor HEAD file 17)" &&
		echo "- 9 UNKNOWN - $(anchor tri file 9)" &&
		echo "- 13 HUMAN human $(anchor tri file 13)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am gives the lines of a path without a block to the human' '
	git checkout -b resolved-other shift &&
	test_write_lines a 0 b c d e 1 2 tri 4 5 6 7 8 9 10 >file &&
	echo other >other &&
	git add file other &&
	test_tick &&
	git commit -m tri-other &&
	test_must_fail git am -3 ai.mbox &&
	test_write_lines a 0 b c d e 1 2 three 4 5 6 seven 8 9 10 11 >file &&
	echo another >>other &&
	git add file other &&
	test_tick &&
	git am --continue &&
	{
		header HEAD &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "file file" &&
		echo "+ 9 AI s1 $(anchor HEAD file 9)" &&
		echo "+ 13 HUMAN human $(anchor HEAD file 13)" &&
		echo "+ 17 AI s1 $(anchor HEAD file 17)" &&
		echo "- 9 UNKNOWN - $(anchor HEAD^ file 9)" &&
		echo "- 13 HUMAN human $(anchor HEAD^ file 13)" &&
		echo "file other" &&
		echo "+ 2 HUMAN human $(anchor HEAD other 2)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am of a patch without a block marks its lines UNKNOWN' '
	git checkout --detach ai &&
	test_tick &&
	git am stock.mbox &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 17 UNKNOWN - $(anchor HEAD file 17)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am ignores a block that does not parse, with a warning' '
	git checkout --detach base &&
	sed -e "s/^    version 1\$/    version 9/" ai.mbox >bad.mbox &&
	test_tick &&
	git am bad.mbox 2>err &&
	test_grep "ignoring the provenance record of patch 0001" err &&
	{
		header HEAD &&
		echo "file file" &&
		echo "+ 8 UNKNOWN - $(anchor HEAD file 8)" &&
		echo "+ 12 UNKNOWN - $(anchor HEAD file 12)" &&
		echo "+ 16 UNKNOWN - $(anchor HEAD file 16)" &&
		echo "- 8 UNKNOWN - $(anchor base file 8)" &&
		echo "- 12 UNKNOWN - $(anchor base file 12)"
	} >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'am of a root commit records its block' '
	git format-patch -1 --stdout --root base >base.mbox &&
	git checkout --orphan root &&
	git rm -rf . &&
	test_tick &&
	git am base.mbox &&
	git provenance show base >old.out &&
	sed -e "s/^commit .*/commit $(git rev-parse HEAD)/" old.out >expect &&
	check_lines HEAD &&
	git provenance verify HEAD
'

test_expect_success 'fast-export --all and fast-import carry the records' '
	git checkout main &&
	git fast-export --all >all.out &&
	git init all.git &&
	git -C all.git fast-import --quiet <all.out &&
	for ref in refs/provenance/lines refs/provenance/reasoning
	do
		git rev-parse $ref >expect &&
		git -C all.git rev-parse $ref >actual &&
		test_cmp expect actual || return 1
	done &&
	git rev-list --branches >commits.out &&
	git -C all.git provenance verify $(cat commits.out) &&
	git log -p --provenance --branches >expect &&
	git -C all.git log -p --provenance --branches >actual &&
	test_cmp expect actual &&
	test_grep "^A+three\$" actual
'

test_expect_success 'fast-export of a branch carries no records' '
	git fast-export main >main.out &&
	git init main.git &&
	git -C main.git fast-import --quiet <main.out &&
	test_must_fail git -C main.git rev-parse --verify -q \
		refs/provenance/lines &&
	test_must_fail git -C main.git provenance show main
'

test_expect_success 'records stay with the old commits when ids change' '
	sed -e "s/^committer C O Mitter/committer C O Mutter/" all.out \
		>changed.out &&
	git init changed.git &&
	git -C changed.git fast-import --quiet <changed.out &&
	old=$(git rev-parse ai) &&
	test "$(git -C changed.git rev-parse ai)" != "$old" &&
	test_must_fail git -C changed.git provenance show ai &&
	git -C changed.git ls-tree -r --name-only refs/provenance/lines \
		>notes.out &&
	tr -d / <notes.out >actual &&
	test_grep "^$old\$" actual
'

test_done
