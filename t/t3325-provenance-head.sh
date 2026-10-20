#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test how commits use stamps taken on another commit'

GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Print the ranges of the line record of <rev> without their anchors.
#
# Usage: ranges <rev>
ranges () {
	git provenance show "$1" >show.out &&
	sed -n -e "s/^\([-+] [0-9-]* [A-Z]* [^ ]*\) .*/\1/p" show.out
}

# Compare the ranges of the line record of <rev> with the file "expect".
check_ranges () {
	ranges "$1" >actual &&
	test_cmp expect actual
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	/expect*
	/actual*
	/err
	/*.saved
	EOF
	test_write_lines 1 2 3 4 5 >app &&
	test_write_lines a b c >shared &&
	git add app shared &&
	test_tick &&
	git commit -m initial &&
	git branch -M main &&
	git checkout -b feature &&
	echo f >feature-only &&
	git add feature-only &&
	test_tick &&
	git commit -m feature &&
	git checkout main
'

test_expect_success 'stamps taken on another branch give no MIXED lines' '
	git checkout feature &&
	git stamp begin --session s1 &&
	test_write_lines 1 two-ai 3 4 5 >app &&
	git stamp end --session s1 &&
	cp app app.saved &&
	git checkout app &&
	git checkout main &&
	test_write_lines 1 two-human 3 4 5 >app &&
	git commit -a -m human &&
	cat >expect <<-\EOF &&
	+ 2 HUMAN human
	- 2 HUMAN human
	EOF
	check_ranges HEAD &&
	git provenance verify HEAD &&
	git stamp list --session s1 >list.out &&
	test_line_count = 2 list.out
'

test_expect_success 'they are used again on the commit they were taken on' '
	git checkout feature &&
	cp app.saved app &&
	git commit -a -m ai &&
	cat >expect <<-\EOF &&
	+ 2 AI s1
	- 2 AI s1
	EOF
	check_ranges HEAD &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'a change carried to another branch is found by content' '
	git checkout feature &&
	git stamp begin --session s2 --model m2 &&
	test_write_lines a b c d-ai e-ai >shared &&
	git stamp end --session s2 &&
	echo "The summary." | git stamp reason --session s2 --summary - &&
	git checkout main &&
	echo six >>app &&
	git commit -a -m carried &&
	cat >expect <<-\EOF &&
	+ 6 HUMAN human
	+ 4-5 AI s2
	EOF
	check_ranges HEAD &&
	git provenance show HEAD >show.out &&
	test_grep "^actor s2 ai model=m2 tool=unknown session=s2 source=observed$" show.out &&
	git provenance show --reasoning HEAD >show.out &&
	test_grep "^The summary.$" show.out &&
	git provenance verify HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'lines a foreign interval did not write are not its own' '
	git checkout feature &&
	git stamp begin --session s3 &&
	test_write_lines x-ai y-ai >notes &&
	git stamp claim --session s3 notes &&
	git stamp end --session s3 &&
	git checkout main &&
	test_write_lines x-ai-edited y-ai >notes &&
	git add notes &&
	git commit -m edited &&
	cat >expect <<-\EOF &&
	+ 1-2 HUMAN human
	EOF
	check_ranges HEAD &&
	git stamp list --session s3 >list.out &&
	test_line_count = 3 list.out &&
	rm .git/provenance/journal
'

test_expect_success 'an interval during which HEAD moved is ignored' '
	git checkout feature &&
	git stamp begin --session s4 &&
	git checkout main &&
	echo seven-ai >>app &&
	git stamp end --session s4 &&
	git commit -a -m crossing &&
	cat >expect <<-\EOF &&
	+ 7 HUMAN human
	EOF
	check_ranges HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'an entry without a head counts as taken on HEAD' '
	git stamp begin --session s5 &&
	echo eight-ai >>app &&
	git stamp end --session s5 &&
	sed -e "s/ head=[0-9a-f]*//" .git/provenance/journal >journal.out &&
	cp journal.out .git/provenance/journal &&
	test_grep ! head= .git/provenance/journal &&
	git commit -a -m old-format &&
	cat >expect <<-\EOF &&
	+ 8 AI s5
	EOF
	check_ranges HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success '--amend uses the stamps taken on the amended commit' '
	git stamp begin --session s6 &&
	echo nine-ai >>app &&
	git stamp end --session s6 &&
	git commit --amend -a -m amended &&
	cat >expect <<-\EOF &&
	+ 8 AI s5
	+ 9 AI s6
	EOF
	check_ranges HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'a partial commit keeps the stamps for the next commit' '
	git stamp begin --session s7 &&
	echo ten-ai >>app &&
	echo f-ai >>shared &&
	git stamp end --session s7 &&
	git commit -m partial app &&
	git stamp list >list.out &&
	sed -n -e "/ s7 head=$(git rev-parse HEAD) /p" list.out >heads.out &&
	test_line_count = 2 heads.out &&
	cat >expect <<-\EOF &&
	+ 10 AI s7
	EOF
	check_ranges HEAD &&
	git commit -a -m rest &&
	cat >expect <<-\EOF &&
	+ 6 AI s7
	EOF
	check_ranges HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'foreign stamps on a commit no branch reaches are dropped' '
	git checkout -b gone &&
	test_commit --no-tag gone-commit gone-file &&
	git stamp begin --session s8 &&
	echo g-ai >>gone-file &&
	git stamp end --session s8 &&
	git checkout -f main &&
	git branch -D gone &&
	echo eleven >>app &&
	git commit -a -m human &&
	cat >expect <<-\EOF &&
	+ 11 HUMAN human
	EOF
	check_ranges HEAD &&
	test_path_is_missing .git/provenance/journal
'

test_done
