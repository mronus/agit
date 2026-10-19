#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test adopting provenance records for commits made without them'

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

# Print the first lines of the adopted line record of <rev>.
header () {
	echo "version 1" &&
	echo "commit $(git rev-parse "$1")" &&
	echo "capture adopted"
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

# Check that <new> has the records of <old>, adopted: they differ only
# in the commit they name and in how they were captured.
#
# Usage: adopted_records <old> <new>
adopted_records () {
	new=$(git rev-parse "$2") &&
	git provenance show "$1" >old.out &&
	sed -e "s/^commit .*/commit $new/" \
	    -e "s/^capture .*/capture adopted/" old.out >expect &&
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

# Run a git command without the extension, as stock git would.
stock () {
	git -c provenance.enabled=false "$@"
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	/expect*
	/actual*
	/err
	/bad
	EOF
	git config core.logAllRefUpdates always &&
	test_write_lines 1 2 3 4 5 6 7 8 9 10 >file &&
	echo other >other &&
	git add file other &&
	test_tick &&
	git commit -m base &&
	git tag base &&

	git checkout -b feature &&
	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines 1 2 three 4 5 6 7 8 9 10 11 >file &&
	git stamp end --session s1 &&
	echo "First summary." | git stamp reason --session s1 --summary - &&
	test_tick &&
	git commit -a -m f1 --session s1 &&
	git tag f1 &&

	test_write_lines 1 2 three 4 5 6 seven 8 9 10 11 >file &&
	git stamp begin --session s2 &&
	test_write_lines 1 2 three 4 5 6 seven 8 nine 10 11 >file &&
	git stamp end --session s2 &&
	echo "Second summary." | git stamp reason --session s2 --summary - &&
	test_tick &&
	git commit -a -m f2 --session s2 &&
	git tag f2 &&

	git stamp begin --session s3 &&
	test_write_lines 1 2 three 4 5 6 seven 8 nine 10 11 twelve >file &&
	git stamp end --session s3 &&
	echo "Third summary." | git stamp reason --session s3 --summary - &&
	test_tick &&
	git commit -a -m f3 --session s3 &&
	git tag f3 &&

	test_write_lines 1 2 three 4 5 6 seven 8 nine 10 11 >file &&
	test_tick &&
	git commit -a -m f4 &&
	git tag f4 &&

	git checkout -b moved base &&
	echo moved >other &&
	test_tick &&
	git commit -a -m moved &&
	git checkout main
'

test_expect_success 'adopt a squash merge made without the extension' '
	stock merge --squash feature &&
	test_write_lines 1 2 three 4 five-ish 6 seven 8 nine 10 11 >file &&
	test_tick &&
	stock commit -a -m squash &&
	test_must_fail git provenance show HEAD &&
	git provenance adopt main >actual &&
	echo "$(git rev-parse main) adopted" >expect &&
	test_cmp expect actual &&
	{
		header main &&
		echo "$s1_actor" &&
		echo "$human_actor" &&
		echo "$s2_actor" &&
		echo "file file" &&
		echo "+ 3 AI s1 $(anchor main file 3)" &&
		echo "+ 5 UNKNOWN - $(anchor main file 5)" &&
		echo "+ 7 HUMAN human $(anchor main file 7)" &&
		echo "+ 9 AI s2 $(anchor main file 9)" &&
		echo "+ 11 AI s1 $(anchor main file 11)" &&
		echo "- 3 AI s1 $(anchor base file 3)" &&
		echo "- 5 UNKNOWN - $(anchor base file 5)" &&
		echo "- 7 HUMAN human $(anchor base file 7)" &&
		echo "- 9 AI s2 $(anchor base file 9)"
	} >expect &&
	check_lines main &&
	{
		echo "version 1" &&
		echo "commit $(git rev-parse main)" &&
		session_line f1 s1 &&
		session_line f2 s2 &&
		echo &&
		echo "First summary." &&
		echo &&
		echo "Second summary."
	} >expect &&
	check_reasoning main &&
	git provenance verify main &&
	git tag squash
'

test_expect_success 'show marks the lines of an adopted commit' '
	git show --provenance squash >actual &&
	test_grep "^A+three$" actual &&
	test_grep "^?+five-ish$" actual &&
	test_grep "^H+seven$" actual
'

test_expect_success 'adopt a rebase made without the extension' '
	git checkout -b rebased f1 &&
	stock rebase moved &&
	git provenance adopt HEAD >actual &&
	echo "$(git rev-parse HEAD) adopted" >expect &&
	test_cmp expect actual &&
	adopted_records f1 HEAD &&
	git provenance verify HEAD
'

test_expect_success 'adopt a cherry-pick -n made without the extension' '
	git checkout -b picked moved &&
	stock cherry-pick -n f2 &&
	test_tick &&
	stock commit -m picked &&
	git provenance adopt HEAD >actual &&
	echo "$(git rev-parse HEAD) adopted" >expect &&
	test_cmp expect actual &&
	adopted_records f2 HEAD &&
	git provenance verify HEAD
'

test_expect_success 'a commit with an observed record is left alone' '
	git provenance show f1 >expect &&
	git provenance adopt --force f1 >actual &&
	echo "$(git rev-parse f1) has record" >actual.expect &&
	test_cmp actual.expect actual &&
	check_lines f1
'

test_expect_success 'an adopted record is replaced only with --force' '
	git provenance adopt squash >actual &&
	echo "$(git rev-parse squash) has record" >expect &&
	test_cmp expect actual &&
	git provenance show squash >expect &&
	git provenance adopt --force squash >actual &&
	echo "$(git rev-parse squash) adopted" >actual.expect &&
	test_cmp actual.expect actual &&
	check_lines squash &&
	git reflog show --format=%gs refs/provenance/lines >reflog.out &&
	grep -c "^provenance adopt: provenance for $(git rev-parse --short squash)$" \
		reflog.out >actual &&
	echo 2 >expect &&
	test_cmp expect actual
'

test_expect_success 'merges are not adopted' '
	git checkout -b merge-target moved &&
	stock merge --no-ff -m merge f1 &&
	git provenance adopt HEAD >actual &&
	echo "$(git rev-parse HEAD) merge" >expect &&
	test_cmp expect actual &&
	test_must_fail git provenance show HEAD
'

test_expect_success 'a commit that no record accounts for gets no record' '
	git checkout -b lonely moved &&
	echo lonely >lonely &&
	git add lonely &&
	stock commit -m lonely &&
	git provenance adopt HEAD >actual &&
	echo "$(git rev-parse HEAD) no source" >expect &&
	test_cmp expect actual &&
	test_must_fail git provenance show HEAD
'

test_expect_success '--from limits the commits records are taken from' '
	git checkout -b from-target moved &&
	stock cherry-pick f1 &&
	git provenance adopt --from moved HEAD >actual &&
	echo "$(git rev-parse HEAD) no source" >expect &&
	test_cmp expect actual &&
	test_must_fail git provenance show HEAD &&
	git provenance adopt --from moved --from feature HEAD >actual &&
	echo "$(git rev-parse HEAD) adopted" >expect &&
	test_cmp expect actual &&
	adopted_records f1 HEAD
'

test_expect_success 'a commit in the history of the target is not its source' '
	git checkout -b ancestor base &&
	git stamp begin --session s4 &&
	echo extra >>file &&
	git stamp end --session s4 &&
	git commit -a -m extra --session s4 &&
	git provenance show HEAD >actual &&
	test_grep "^+ 11 AI s4 " actual &&
	git checkout HEAD^ -- file &&
	git commit -a -m "drop extra" &&
	echo extra >>file &&
	stock commit -a -m "extra again" &&
	git provenance adopt HEAD >actual &&
	echo "$(git rev-parse HEAD) no source" >expect &&
	test_cmp expect actual
'

test_expect_success 'a range is adopted oldest first' '
	git checkout -b range moved &&
	test_tick &&
	stock cherry-pick f1 f2 &&
	git provenance adopt moved..range >actual &&
	{
		echo "$(git rev-parse range^) adopted" &&
		echo "$(git rev-parse range) adopted"
	} >expect &&
	test_cmp expect actual &&
	adopted_records f1 range^ &&
	adopted_records f2 range
'

test_expect_success 'unusable records are skipped with a warning' '
	missing=$(echo missing | git hash-object --stdin) &&
	git provenance show f1 >good.out &&
	add_record lines "$missing" good.out &&
	git checkout -b unusable moved &&
	test_commit junk &&
	echo garbage >bad &&
	add_record lines junk bad &&
	git checkout -b late moved &&
	test_tick &&
	stock cherry-pick f1 &&
	git provenance adopt HEAD >actual 2>err &&
	echo "$(git rev-parse HEAD) adopted" >expect &&
	test_cmp expect actual &&
	test_grep "ignoring the provenance record of $missing" err &&
	test_grep "ignoring malformed provenance record of $(git rev-parse junk)" err &&
	adopted_records f1 HEAD
'

test_expect_success '--force replaces the reasoning record, too' '
	git checkout -b human-f1 base &&
	test_write_lines 1 2 three 4 5 6 7 8 9 10 11 >file &&
	test_tick &&
	git commit -a -m human-f1 &&
	test_must_fail git provenance show --reasoning human-f1 &&
	git checkout -b reasoned moved &&
	stock cherry-pick f1 &&
	git provenance adopt --from feature HEAD >actual &&
	echo "$(git rev-parse HEAD) adopted" >adopted.expect &&
	test_cmp adopted.expect actual &&
	adopted_records f1 HEAD &&
	git provenance adopt --force --from human-f1 HEAD >actual &&
	test_cmp adopted.expect actual &&
	adopted_records human-f1 HEAD &&
	git provenance verify HEAD
'

test_done
