#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test summing the line records of a range of commits'

GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Attach a record to a commit, as t3312 does: "git notes" only writes
# below refs/notes/, so write the note through a scratch notes ref.
#
# Usage: add_record (lines|reasoning) <commit> <file>
add_record () {
	if git rev-parse -q --verify "refs/provenance/$1" >/dev/null
	then
		git update-ref "refs/notes/provenance-$1" "refs/provenance/$1"
	fi &&
	git notes --ref="provenance-$1" add -f --no-stripspace -F "$3" "$2" &&
	git update-ref "refs/provenance/$1" "refs/notes/provenance-$1" &&
	git update-ref -d "refs/notes/provenance-$1"
}

# Attach a line record to <rev> that declares the actors "s9" and
# "human", followed by the given lines.
#
# Usage: attach_record <rev> <line>...
attach_record () {
	rev=$(git rev-parse "$1") &&
	shift &&
	{
		echo "version 1" &&
		echo "commit $rev" &&
		echo "capture declared" &&
		echo "actor s9 ai model=m9 tool=t9 session=s9 source=declared" &&
		echo "actor human human ident=A U Thor <author@example.com>" &&
		for line in "$@"
		do
			echo "$line" || return 1
		done
	} >record.out &&
	add_record lines "$rev" record.out
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	*.out
	/expect*
	/actual*
	/err
	EOF
	test_commit c1 a &&
	attach_record c1 "file a" \
		"+ 1-4 HUMAN human $ZERO_OID" \
		"+ 5 UNKNOWN - $ZERO_OID" &&
	test_commit c2 b &&
	attach_record c2 "file a" \
		"+ 1-3 AI s9 $ZERO_OID" \
		"+ 4 MIXED s9,human $ZERO_OID" \
		"+ 5-7 HUMAN human $ZERO_OID" \
		"file b" \
		"- 1-2 HUMAN human $ZERO_OID" \
		"- 3 AI s9 $ZERO_OID" &&
	echo c3 >c &&
	git add c &&
	test_tick &&
	git -c provenance.enabled=false commit -m c3 &&
	git tag c3 &&
	test_must_fail git provenance show c3 &&
	test_commit c4 d &&
	attach_record c4 "file d" \
		"+ 1-2 UNKNOWN - $ZERO_OID" \
		"+ 3 HUMAN human $ZERO_OID" \
		"- 1 MIXED s9,human $ZERO_OID" \
		"- 2-3 UNKNOWN - $ZERO_OID"
'

test_expect_success 'stat --porcelain sums the history of HEAD' '
	cat >expect <<-\EOF &&
	commits 4
	recorded 3
	added 15
	added.ai 3
	added.mixed 1
	added.human 8
	added.unknown 3
	removed 6
	removed.ai 1
	removed.mixed 1
	removed.human 2
	removed.unknown 2
	share 27
	EOF
	git provenance stat --porcelain >actual &&
	test_cmp expect actual &&
	git provenance stat --porcelain HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'stat prints an aligned summary' '
	cat >expect <<-\EOF &&
	commits    4  (3 recorded)
	added     15  AI 3 (20%)  MIXED 1 (7%)   HUMAN 8 (53%)  UNKNOWN 3 (20%)
	removed    6  AI 1 (17%)  MIXED 1 (17%)  HUMAN 2 (33%)  UNKNOWN 2 (33%)
	share    27%
	EOF
	git provenance stat >actual &&
	test_cmp expect actual
'

test_expect_success 'stat of a commit sums its history' '
	cat >expect <<-\EOF &&
	commits 2
	recorded 2
	added 12
	added.ai 3
	added.mixed 1
	added.human 7
	added.unknown 1
	removed 3
	removed.ai 1
	removed.mixed 0
	removed.human 2
	removed.unknown 0
	share 33
	EOF
	git provenance stat --porcelain c2 >actual &&
	test_cmp expect actual
'

test_expect_success 'stat of a range sums the commits in it' '
	cat >expect <<-\EOF &&
	commits 2
	recorded 1
	added 3
	added.ai 0
	added.mixed 0
	added.human 1
	added.unknown 2
	removed 3
	removed.ai 0
	removed.mixed 1
	removed.human 0
	removed.unknown 2
	share 0
	EOF
	git provenance stat --porcelain c2..c4 >actual &&
	test_cmp expect actual &&
	git provenance stat --porcelain c4 ^c2 >actual &&
	test_cmp expect actual
'

test_expect_success 'the share of one commit is its %(provenance:share)' '
	git log -1 --format="share %(provenance:share)" c2 >expect.raw &&
	sed -e "s/%$//" expect.raw >expect &&
	test_grep "^share 57$" expect &&
	git provenance stat --porcelain c2^! >actual.raw &&
	grep "^share " actual.raw >actual &&
	test_cmp expect actual
'

test_expect_success 'stat counts commits without records' '
	test_when_finished "rm -rf norec" &&
	git init norec &&
	test_commit -C norec --no-tag one &&
	git -C norec update-ref -d refs/provenance/lines &&
	git -C norec provenance stat --porcelain >actual &&
	cat >expect <<-\EOF &&
	commits 1
	recorded 0
	added 0
	added.ai 0
	added.mixed 0
	added.human 0
	added.unknown 0
	removed 0
	removed.ai 0
	removed.mixed 0
	removed.human 0
	removed.unknown 0
	share 0
	EOF
	test_cmp expect actual
'

test_expect_success 'stat skips a record that does not parse' '
	bad=$(git commit-tree -p c4 -m bad HEAD^{tree}) &&
	printf "version 1\nnonsense\n" >bad.out &&
	add_record lines $bad bad.out &&
	git provenance stat --porcelain c4..$bad >actual 2>err &&
	test_grep "ignoring malformed provenance record of $bad" err &&
	test_grep "^commits 1$" actual &&
	test_grep "^recorded 0$" actual &&
	test_grep "^added 0$" actual
'

test_expect_success 'stat rejects what is not a revision' '
	test_must_fail git provenance stat --nope 2>err &&
	test_grep "unrecognized argument: --nope" err &&
	test_must_fail git provenance stat HEAD -- a 2>err &&
	test_grep "pathspec arguments not supported" err &&
	test_must_fail git provenance stat no-such-rev
'

test_expect_success 'stat fails if the record ref is not a tree' '
	test_when_finished "rm -rf broken" &&
	git init broken &&
	test_commit -C broken --no-tag one &&
	blob=$(echo nonsense | git -C broken hash-object -w --stdin) &&
	git -C broken update-ref refs/provenance/lines $blob &&
	test_expect_code 1 git -C broken provenance stat 2>err &&
	test_grep "does not point to a tree" err
'

test_done
