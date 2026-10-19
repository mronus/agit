#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test carrying provenance records across imports with remap and prune'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME
GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Print the anchor of lines <a> to <b> of <rev>:<path> in <repo>.
#
# Usage: anchor <repo> <rev> <path> <a>[,<b>]
anchor () {
	git -C "$1" cat-file blob "$2:$3" >anchor.tmp &&
	sed -n "$4p" anchor.tmp | git hash-object --stdin &&
	rm -f anchor.tmp
}

# Import the stream <file> into the new repository <repo>, and write
# the map of the ids of this repository to those of <repo> to
# <repo>.map, joining the marks files of the export and of the import
# as the manual page says.
#
# Usage: import <repo> <file>
import () {
	git init -q "$1" &&
	git -C "$1" config core.logAllRefUpdates always &&
	git -C "$1" fast-import --quiet --export-marks="$PWD/$1.marks" <"$2" &&
	sort old.marks >old.sorted &&
	sort "$1.marks" >new.sorted &&
	join old.sorted new.sorted >joined.out &&
	cut -d" " -f2,3 joined.out >"$1.map"
}

# Print the records of <rev> in <repo> without their "commit" lines;
# <rev> may have no reasoning record.
#
# Usage: records <repo> <rev>
records () {
	git -C "$1" provenance show "$2" >records.tmp &&
	if git -C "$1" provenance show --reasoning "$2" >reasoning.tmp 2>/dev/null
	then
		cat reasoning.tmp >>records.tmp
	fi &&
	grep -v "^commit " records.tmp &&
	rm -f records.tmp reasoning.tmp
}

# Print the commits that have a record under <ref> in <repo>.
#
# Usage: keys <repo> <ref>
keys () {
	git -C "$1" ls-tree -r --name-only "$2" >keys.tmp &&
	tr -d / <keys.tmp &&
	rm -f keys.tmp
}

# Print the commits that the provenance refs of <repo> point to.
#
# Usage: provenance_refs <repo>
provenance_refs () {
	git -C "$1" rev-parse refs/provenance/lines refs/provenance/reasoning
}

test_expect_success 'setup' '
	git config --global advice.provenanceSession false &&
	test_write_lines a b c d e f g h >file &&
	git add file &&
	test_tick &&
	git commit -m first &&

	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines a b C d e f g h >file &&
	git stamp end --session s1 &&
	test_write_lines a b C d e F g h >file &&
	test_tick &&
	git commit -a -m second &&

	git stamp begin --session s2 --model m2 --tool t2 &&
	test_write_lines p1 p2 >other &&
	git stamp end --session s2 &&
	test_write_lines p1 p2 h1 >other &&
	git add other &&
	test_tick &&
	git commit -m third &&

	git rev-list main >old.out &&
	git fast-export --all --export-marks=old.marks >all.out
'

test_expect_success 'records move to the commits whose messages changed' '
	sed -e "s/^first\$/First/" -e "s/^second\$/Second/" \
		-e "s/^third\$/Third/" all.out >msg.out &&
	import msg msg.out &&
	git -C msg rev-list main >new.out &&
	! cmp -s old.out new.out &&
	paste -d" " old.out new.out >old-new.out &&

	git -C msg provenance remap ../msg.map >actual &&
	while read old new
	do
		echo "$old $new moved" || return 1
	done <old-new.out >expect &&
	sort expect >expect.sorted &&
	sort actual >actual.sorted &&
	test_cmp expect.sorted actual.sorted &&

	git -C msg provenance verify $(cat new.out) &&
	while read old new
	do
		records . $old >expect &&
		records msg $new >actual &&
		test_cmp expect actual &&
		git -C msg provenance show $new >actual &&
		test_grep "^commit $new\$" actual || return 1
	done <old-new.out &&
	git -C msg provenance show --reasoning main >actual &&
	test_grep "^commit $(git -C msg rev-parse main)\$" actual &&

	for ref in refs/provenance/lines refs/provenance/reasoning
	do
		keys msg $ref >keys.out &&
		test_grep ! -f old.out keys.out &&
		git rev-parse $ref >expect &&
		git -C msg rev-parse $ref^ >actual &&
		test_cmp expect actual &&
		git -C msg reflog -1 --format=%gs $ref >actual &&
		echo remap >expect &&
		test_cmp expect actual || return 1
	done &&
	git log -p --provenance --format=%T main >expect &&
	git -C msg log -p --provenance --format=%T main >actual &&
	test_cmp expect actual
'

test_expect_success 'a record whose changes differ is re-attached' '
	sed -e "s/^h1\$/H1/" all.out >blob.out &&
	import blob blob.out &&
	old=$(git rev-parse main) &&
	new=$(git -C blob rev-parse main) &&
	test $(git rev-parse main~1) = $(git -C blob rev-parse main~1) &&
	{
		echo "# third" &&
		echo &&
		cat blob.map
	} | git -C blob provenance remap >actual &&
	echo "$old $new re-attached" >expect &&
	test_cmp expect actual &&

	git -C blob provenance verify main &&
	cat >expect <<-EOF &&
	version 1
	commit $new
	capture adopted
	actor s2 ai model=m2 tool=t2 session=s2 source=observed
	file other
	+ 1-2 AI s2 $(anchor blob main other 1,2)
	+ 3 UNKNOWN - $(anchor blob main other 3)
	EOF
	git -C blob provenance show main >actual &&
	test_cmp expect actual &&
	git provenance show --reasoning main >expect.tmp &&
	grep -v "^commit " expect.tmp >expect &&
	git -C blob provenance show --reasoning main >actual.tmp &&
	test_grep "^commit $new\$" actual.tmp &&
	grep -v "^commit " actual.tmp >actual &&
	test_cmp expect actual &&
	keys blob refs/provenance/lines >keys.out &&
	test_grep ! $old keys.out
'

test_expect_success 'pairs whose new commit is missing or has a record are skipped' '
	provenance_refs . >expect.refs &&
	second=$(git rev-parse main~1) &&
	third=$(git rev-parse main) &&
	missing=$(test_oid deadbeef) &&
	blob=$(git rev-parse main:file) &&
	cat >skip.map <<-EOF &&
	$second $missing
	$second $blob
	$second $third
	EOF
	git provenance remap skip.map >actual 2>err &&
	cat >expect <<-EOF &&
	$second $missing skipped: not a commit
	$second $blob skipped: not a commit
	$second $third skipped: has a line record
	EOF
	test_cmp expect actual &&
	test_grep "warning: .* $missing is not a commit" err &&
	test_grep "warning: .* $blob is not a commit" err &&
	test_grep "warning: .* $third has a line record" err &&
	provenance_refs . >actual.refs &&
	test_cmp expect.refs actual.refs
'

test_expect_success '--dry-run writes nothing' '
	import dry msg.out &&
	provenance_refs dry >expect.refs &&
	git -C dry provenance remap --dry-run ../dry.map >dry.out &&
	provenance_refs dry >actual.refs &&
	test_cmp expect.refs actual.refs &&
	test_line_count = 3 dry.out &&
	git -C dry provenance remap ../dry.map >actual &&
	test_cmp dry.out actual
'

test_expect_success 'a line that is not a pair is an error' '
	provenance_refs . >expect.refs &&
	first=$(git rev-parse main~2) &&
	printf "# map\n%s %s\n%s\n" $first $first $first >bad.map &&
	test_must_fail git provenance remap bad.map >actual 2>err &&
	test_must_be_empty actual &&
	test_grep "line 3: " err &&
	printf "%s %s x\n" $first $first |
	test_must_fail git provenance remap 2>err &&
	test_grep "line 1: " err &&
	provenance_refs . >actual.refs &&
	test_cmp expect.refs actual.refs
'

test_expect_success 'nothing is written when nothing moves' '
	provenance_refs . >expect.refs &&
	git reflog refs/provenance/lines >expect.reflog &&
	first=$(git rev-parse main~2) &&
	git rev-parse main:file main:other >blobs.out &&
	paste -d" " blobs.out blobs.out >same.map &&
	echo "$first $first" >>same.map &&
	git provenance remap same.map >actual &&
	test_must_be_empty actual &&
	git provenance remap </dev/null >actual &&
	test_must_be_empty actual &&
	provenance_refs . >actual.refs &&
	test_cmp expect.refs actual.refs &&
	git reflog refs/provenance/lines >actual.reflog &&
	test_cmp expect.reflog actual.reflog
'

test_expect_success 'prune removes the records of the commits that are gone' '
	import gone blob.out &&
	old=$(git rev-parse main) &&
	git -C gone rev-list main >kept.out &&
	test_grep ! $old kept.out &&
	provenance_refs gone >expect.refs &&
	git -C gone provenance prune --dry-run >actual &&
	echo "$old pruned" >expect &&
	test_cmp expect actual &&
	provenance_refs gone >actual.refs &&
	test_cmp expect.refs actual.refs &&

	git -C gone provenance prune >actual &&
	test_cmp expect actual &&
	for ref in refs/provenance/lines refs/provenance/reasoning
	do
		keys gone $ref >keys.out &&
		test_grep ! $old keys.out &&
		git rev-parse $ref >expect &&
		git -C gone rev-parse $ref^ >actual &&
		test_cmp expect actual &&
		git -C gone reflog -1 --format=%gs $ref >actual &&
		echo prune >expect &&
		test_cmp expect actual || return 1
	done &&
	git -C gone provenance verify main~1 main~2 &&
	keys gone refs/provenance/lines >keys.out &&
	test_line_count = 2 keys.out
'

test_expect_success 'prune keeps the records of unreachable commits' '
	git checkout -b side &&
	echo side >side &&
	git add side &&
	test_tick &&
	git commit -m side &&
	side=$(git rev-parse side) &&
	git checkout main &&
	git branch -D side &&
	provenance_refs . >expect.refs &&
	git provenance prune >actual &&
	test_must_be_empty actual &&
	provenance_refs . >actual.refs &&
	test_cmp expect.refs actual.refs &&
	git provenance show $side
'

test_done
