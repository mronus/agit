#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test viewing provenance in diff, blame and log'

GIT_TEST_PROVENANCE=true
export GIT_TEST_PROVENANCE

. ./test-lib.sh

# Print the lines of the patch in <file> from the first hunk header
# on, without file headers and the function names of hunk headers.
#
# Usage: patch_body <file>
patch_body () {
	sed -n -e '/^@@/,$p' "$1" |
	sed -e 's/^\(@@ .* @@\) .*/\1/' \
	    -e '/^diff --git /d' -e '/^index /d' -e '/^--- /d' -e '/^+++ /d' \
	    -e '/^new file mode /d' -e '/^deleted file mode /d' \
	    -e '/^similarity index /d' -e '/^rename from /d' -e '/^rename to /d'
}

# Insert the marks listed in <marks>, one per line, after the first
# occurrence of <sep> on each line of <file>.
#
# Usage: add_marks <marks> <file> [<sep>]
add_marks () {
	awk -v sep="${3:- }" '
		NR == FNR { m[NR] = $0; next }
		{ i = index($0, sep); print substr($0, 1, i - 1) " " m[FNR] substr($0, i) }
	' "$1" "$2"
}

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
	*.raw
	/expect*
	/actual*
	/plain*
	/marks*
	/live*
	/index.*
	/before
	/after
	/reasoning
	/err
	/repo*
	EOF
	test_write_lines 1 2 3 4 5 >file &&
	git add file &&
	test_tick &&
	git commit -m initial &&
	git tag initial &&
	git stamp begin --session s1 --model m1 --tool t1 &&
	test_write_lines 1 two 3 four 5 6 >file &&
	git stamp end --session s1 &&
	test_write_lines one two 3 FOUR 5 6 >file &&
	test_tick &&
	git commit -a -m second &&
	git tag second &&
	cat >expect.second <<-\EOF
	@@ -1,5 +1,6 @@
	H-1
	A-2
	H+one
	A+two
	  3
	A-4
	M+FOUR
	  5
	A+6
	EOF
'

test_expect_success 'show --provenance marks the lines of a commit' '
	git show --provenance second >actual &&
	patch_body actual >actual.body &&
	test_cmp expect.second actual.body &&
	git show second >plain &&
	sed -n -e "1,/^@@/p" plain >expect.head &&
	sed -n -e "1,/^@@/p" actual >actual.head &&
	test_cmp expect.head actual.head
'

test_expect_success 'log -p, diff and diff-tree -p show the same marks' '
	git log -1 -p --provenance second >actual &&
	patch_body actual >actual.body &&
	test_cmp expect.second actual.body &&
	git diff --provenance second^ second >actual &&
	patch_body actual >actual.body &&
	test_cmp expect.second actual.body &&
	git diff --provenance second^! >actual &&
	patch_body actual >actual.body &&
	test_cmp expect.second actual.body &&
	git diff-tree -p --provenance second >actual &&
	patch_body actual >actual.body &&
	test_cmp expect.second actual.body
'

test_expect_success 'a root commit is marked against the empty tree' '
	cat >expect <<-\EOF &&
	@@ -0,0 +1,5 @@
	H+1
	H+2
	H+3
	H+4
	H+5
	EOF
	git show --provenance initial >actual &&
	patch_body actual >actual.body &&
	test_cmp expect actual.body &&
	git log -p --provenance --reverse --format= >actual &&
	patch_body actual >actual.body &&
	cat expect expect.second >expect.all &&
	test_cmp expect.all actual.body
'

test_expect_success '-R swaps the marks with the sides' '
	cat >expect <<-\EOF &&
	@@ -1,6 +1,5 @@
	H-one
	A-two
	H+1
	A+2
	  3
	M-FOUR
	A+4
	  5
	A-6
	EOF
	git show -R --provenance second >actual &&
	patch_body actual >actual.body &&
	test_cmp expect actual.body
'

test_expect_success 'a diff between other commits shows ?' '
	git commit --allow-empty -m empty &&
	test_when_finished "git reset --hard second" &&
	sed -e "s/^[AMH]\([-+]\)/?\1/" expect.second >expect &&
	git diff --provenance initial HEAD >actual &&
	patch_body actual >actual.body &&
	test_cmp expect actual.body &&
	git show --provenance HEAD >actual &&
	test_grep ! "^[AMH?][-+ ]" actual
'

test_expect_success 'commits without a record show ?' '
	test_config provenance.enabled false &&
	echo 7 >>file &&
	git commit -a -m norecord &&
	test_when_finished "git reset --hard second" &&
	test_must_fail git provenance show HEAD &&
	cat >expect <<-\EOF &&
	@@ -4,3 +4,4 @@
	  FOUR
	  5
	  6
	?+7
	EOF
	git show --provenance HEAD >actual 2>err &&
	patch_body actual >actual.body &&
	test_cmp expect actual.body &&
	test_must_be_empty err
'

test_expect_success 'paths and lines that the record does not cover show ?' '
	echo 7 >>file &&
	echo other >other &&
	git add other &&
	git -c provenance.enabled=false commit -a -m partial &&
	test_when_finished "git reset --hard second" &&
	attach_record HEAD "file file" "+ 7 AI s9 $ZERO_OID" &&
	git show --provenance HEAD >actual &&
	patch_body actual >actual.body &&
	cat >expect <<-\EOF &&
	@@ -4,3 +4,4 @@
	  FOUR
	  5
	  6
	A+7
	@@ -0,0 +1 @@
	?+other
	EOF
	test_cmp expect actual.body
'

test_expect_success 'a malformed record shows ? with a warning' '
	echo 7 >>file &&
	git -c provenance.enabled=false commit -a -m malformed &&
	test_when_finished "git reset --hard second" &&
	echo garbage >garbage.out &&
	add_record lines HEAD garbage.out &&
	git show --provenance HEAD >actual 2>err &&
	patch_body actual >actual.body &&
	test_grep "^?+7$" actual.body &&
	test_grep "warning: ignoring malformed provenance record of $(git rev-parse HEAD)" err &&
	test_line_count = 1 err
'

test_expect_success 'a renamed file is marked under its old and new paths' '
	git mv file renamed &&
	git stamp begin --session s2 --model m2 &&
	sed -e "s/^two$/TWO/" renamed >renamed.tmp &&
	mv renamed.tmp renamed &&
	git stamp end --session s2 &&
	git commit -a -m renamed &&
	git tag moved &&
	cat >expect <<-\EOF &&
	@@ -1,5 +1,5 @@
	  one
	H-two
	M+TWO
	  3
	  FOUR
	  5
	EOF
	git show -M --provenance moved >actual &&
	patch_body actual >actual.body &&
	head -n 7 actual.body >actual.head &&
	test_cmp expect actual.head
'

test_expect_success '--color puts the mark inside the colored sign' '
	git show --provenance --color=always second >actual.raw &&
	test_decode_color <actual.raw >actual &&
	test_grep "^<GREEN>A+<RESET><GREEN>two<RESET>$" actual &&
	test_grep "^<RED>H-1<RESET>$" actual &&
	test_grep "^  3<RESET>$" actual &&
	git show --color=always second >plain.raw &&
	test_decode_color <plain.raw >plain &&
	test_grep "^<GREEN>+<RESET><GREEN>two<RESET>$" plain &&
	test_grep "^ 3<RESET>$" plain
'

test_expect_success '--provenance does not change other output formats' '
	for opt in --stat --numstat --raw --name-only --name-status --shortstat --summary
	do
		git show $opt second >expect &&
		git show $opt --provenance second >actual &&
		test_cmp expect actual || return 1
	done
'

test_expect_success 'word diffs get no column' '
	git show --word-diff second >expect &&
	git show --word-diff --provenance second >actual &&
	test_cmp expect actual &&
	git show --color-words --provenance second >actual &&
	git show --color-words second >expect &&
	test_cmp expect actual
'

test_expect_success 'combined diffs get no column; other parents show ?' '
	git checkout -b side initial &&
	test_when_finished "git checkout -f - && git branch -D side" &&
	echo side >>file &&
	git commit -a -m side &&
	test_must_fail git merge second &&
	test_write_lines one two 3 FOUR 5 6 side >file &&
	git commit -a -m merge &&
	git show --cc HEAD >expect &&
	git show --cc --provenance HEAD >actual &&
	test_cmp expect actual &&
	git show -c HEAD >expect &&
	git show -c --provenance HEAD >actual &&
	test_cmp expect actual &&
	git show --first-parent --provenance HEAD >actual &&
	patch_body actual >actual.body &&
	test_grep "^?+one$" actual.body &&
	git show -m --provenance HEAD >actual &&
	sed -n -e "/^commit .*(from $(git rev-parse HEAD^2))/,\$p" actual >actual.second &&
	patch_body actual.second >actual.body &&
	test_grep "^?+side$" actual.body &&
	test_grep ! "^[AMH][-+]" actual.body
'

test_expect_success 'diff --provenance works without provenance refs' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		GIT_TEST_PROVENANCE=false &&
		export GIT_TEST_PROVENANCE &&
		test_commit one &&
		test_commit two &&
		test_must_fail git rev-parse --verify -q refs/provenance/lines &&
		git show --provenance >actual 2>err &&
		test_must_be_empty err &&
		test_grep "^?+two$" actual &&
		git log -p --provenance >actual 2>err &&
		test_must_be_empty err &&
		test_grep "^?+one$" actual
	)
'

test_expect_success 'the boundary of a shallow clone shows ? quietly' '
	test_when_finished "rm -rf repo" &&
	git clone --depth=1 "file://$(pwd)" repo &&
	git -C repo fetch --depth=1 origin \
		refs/provenance/lines:refs/provenance/lines &&
	git -C repo show --provenance HEAD >actual 2>err &&
	test_must_be_empty err &&
	patch_body actual >actual.body &&
	test_grep "^?+" actual.body &&
	test_grep ! "^[AMH][-+]" actual.body
'

test_expect_success 'format-patch rejects --provenance' '
	test_must_fail git format-patch --provenance -1 second 2>err &&
	test_grep "provenance does not make sense" err
'

# Print the marks that "git show --provenance" prints for HEAD.
shown_marks () {
	git show --provenance HEAD >show.out &&
	patch_body show.out
}

test_expect_success 'diff --cached shows the marks that the commit records' '
	git reset --hard second &&
	git stamp begin --session s3 --model m3 &&
	test_write_lines one two three FOUR 5 six >file &&
	git stamp end --session s3 &&
	test_write_lines one two three FOUR 5 SIX 7 >file &&
	git add file &&
	git diff --cached --provenance >live &&
	patch_body live >live.body &&
	cat >expect <<-\EOF &&
	@@ -1,6 +1,7 @@
	  one
	  two
	A-3
	A+three
	  FOUR
	  5
	A-6
	M+SIX
	H+7
	EOF
	test_cmp expect live.body &&
	git commit -m cached &&
	shown_marks >actual &&
	test_cmp live.body actual
'

test_expect_success 'diff HEAD shows the marks that "commit -a" records' '
	git stamp begin --session s4 --model m4 &&
	test_write_lines one TWO three FOUR 5 SIX 7 8 >file &&
	echo new >new &&
	git add new &&
	git stamp end --session s4 &&
	echo NEW >new &&
	git diff HEAD --provenance >live &&
	patch_body live >live.body &&
	cat >expect <<-\EOF &&
	@@ -1,7 +1,8 @@
	  one
	A-two
	A+TWO
	  three
	  FOUR
	  5
	  SIX
	  7
	A+8
	@@ -0,0 +1 @@
	M+NEW
	EOF
	test_cmp expect live.body &&
	git commit -a -m worktree &&
	shown_marks >actual &&
	test_cmp live.body actual
'

test_expect_success 'diff marks added lines of the working tree, not removed ones' '
	git stamp begin --session s5 &&
	test_write_lines ONE TWO three FOUR 5 SIX 7 8 >file &&
	git stamp end --session s5 &&
	git add file &&
	test_write_lines ONE TWO three FOUR five SIX 7 8 >file &&
	git diff --provenance >actual &&
	patch_body actual >actual.body &&
	cat >expect <<-\EOF &&
	@@ -2,7 +2,7 @@
	  TWO
	  three
	  FOUR
	?-5
	H+five
	  SIX
	  7
	  8
	EOF
	test_cmp expect actual.body &&
	git stamp begin --session s5 &&
	test_write_lines ONE TWO three FOUR five SIX 7 8 9 >file &&
	git stamp end --session s5 &&
	git diff --provenance >actual &&
	patch_body actual >actual.body &&
	test_grep "^A+9$" actual.body &&
	git diff HEAD --provenance >actual &&
	patch_body actual >actual.body &&
	test_grep "^A+ONE$" actual.body &&
	test_grep "^A-one$" actual.body
'

test_expect_success 'diff --cached on an unborn branch is marked' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		git stamp begin --session s1 &&
		test_write_lines a b >file &&
		git stamp end --session s1 &&
		echo c >>file &&
		git add file &&
		git diff --cached --provenance >actual &&
		patch_body actual >actual.body &&
		cat >expect <<-\EOF &&
		@@ -0,0 +1,3 @@
		A+a
		A+b
		H+c
		EOF
		test_cmp expect actual.body
	)
'

test_expect_success 'a live diff against another commit shows ?' '
	git diff --provenance HEAD^ >actual &&
	patch_body actual >actual.body &&
	test_grep ! "^[AMH][-+]" actual.body &&
	test_grep "^?+ONE$" actual.body &&
	git diff --cached --provenance HEAD^ >actual &&
	patch_body actual >actual.body &&
	test_grep ! "^[AMH][-+]" actual.body
'

test_expect_success 'live diffs show ? without provenance.enabled' '
	git -c provenance.enabled=false diff HEAD --provenance >actual &&
	patch_body actual >actual.body &&
	test_grep ! "^[AMH][-+]" actual.body &&
	test_grep "^?+ONE$" actual.body &&
	git -c provenance.enabled=false show --provenance HEAD >actual &&
	test_grep "^A+8$" actual
'

test_expect_success 'live diffs write no objects and leave the index alone' '
	git count-objects -v >before &&
	git ls-files --stage >index.before &&
	git diff --provenance >/dev/null &&
	git diff --cached --provenance >/dev/null &&
	git diff HEAD --provenance >/dev/null &&
	git count-objects -v >after &&
	test_cmp before after &&
	git ls-files --stage >index.after &&
	test_cmp index.before index.after &&
	git commit -a -m live
'

test_expect_success 'setup blame' '
	git reset --hard second
'

test_expect_success 'blame --provenance shows the mark after the commit' '
	test_write_lines H A H M H A >marks &&
	git blame -s file >plain &&
	git blame -s --provenance file >actual &&
	add_marks marks plain >expect &&
	test_cmp expect actual &&
	for opt in "" -e -l -t -n -f --date=short
	do
		git blame $opt file >plain &&
		git blame $opt --provenance file >actual &&
		add_marks marks plain >expect &&
		test_cmp expect actual || return 1
	done &&
	git blame -c file >plain &&
	git blame -c --provenance file >actual &&
	add_marks marks plain "	" >expect &&
	test_cmp expect actual &&
	git annotate file >plain &&
	git annotate --provenance file >actual &&
	add_marks marks plain "	" >expect &&
	test_cmp expect actual
'

ai_actor="provenance-actor s1 ai model=m1 tool=t1 session=s1 source=observed"
human_actor="provenance-actor human human ident=A U Thor <author@example.com>"

test_expect_success 'blame --porcelain gives the provenance of each line' '
	git blame --porcelain file >plain &&
	git blame --porcelain --provenance file >actual &&
	grep -v "^provenance-" actual >actual.plain &&
	test_cmp plain actual.plain &&
	grep "^provenance-" actual >actual.prov &&
	cat >expect <<-EOF &&
	provenance-mark HUMAN
	$human_actor
	provenance-mark AI
	$ai_actor
	provenance-mark HUMAN
	$human_actor
	provenance-mark MIXED
	$human_actor
	$ai_actor
	provenance-mark HUMAN
	$human_actor
	provenance-mark AI
	$ai_actor
	EOF
	test_cmp expect actual.prov &&
	git blame --line-porcelain file >plain &&
	git blame --line-porcelain --provenance file >actual &&
	grep -v "^provenance-" actual >actual.plain &&
	test_cmp plain actual.plain &&
	grep "^provenance-" actual >actual.prov &&
	test_cmp expect actual.prov
'

test_expect_success 'blame --porcelain puts the provenance before the line' '
	git blame --porcelain --provenance -L 2,2 file >actual &&
	tail -n 3 actual >actual.tail &&
	{
		echo "provenance-mark AI" &&
		echo "$ai_actor" &&
		printf "\ttwo\n"
	} >expect &&
	test_cmp expect actual.tail
'

test_expect_success 'blame --incremental splits entries by provenance' '
	git blame --incremental --provenance file >actual &&
	sed -n -e "/^[0-9a-f]\{40,\} /s/^[0-9a-f]* //p" \
		-e "/^provenance-/p" -e "/^filename /p" actual >actual.entries &&
	cat >expect <<-EOF &&
	1 1 1
	provenance-mark HUMAN
	$human_actor
	filename file
	2 2 1
	provenance-mark AI
	$ai_actor
	filename file
	4 4 1
	provenance-mark MIXED
	$human_actor
	$ai_actor
	filename file
	6 6 1
	provenance-mark AI
	$ai_actor
	filename file
	3 3 1
	provenance-mark HUMAN
	$human_actor
	filename file
	5 5 1
	provenance-mark HUMAN
	$human_actor
	filename file
	EOF
	test_cmp expect actual.entries
'

test_expect_success 'blame of history without records shows ?' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		GIT_TEST_PROVENANCE=false &&
		export GIT_TEST_PROVENANCE &&
		test_commit one file &&
		test_commit two file &&
		git blame -s file >plain &&
		test_write_lines "?" >marks &&
		git blame -s --provenance file >actual &&
		add_marks marks plain >expect &&
		test_cmp expect actual &&
		git blame --porcelain --provenance file >actual &&
		grep "^provenance-" actual >actual.prov &&
		echo "provenance-mark UNKNOWN" >expect &&
		test_cmp expect actual.prov
	)
'

test_expect_success 'blame follows a rename to the records of older commits' '
	git blame -s -M --provenance moved -- renamed >actual &&
	git blame -s -M moved -- renamed >plain &&
	test_write_lines H M H M H A >marks.renamed &&
	add_marks marks.renamed plain >expect &&
	test_cmp expect actual &&
	sed -n -e "2p" actual >actual.line &&
	test_grep "^$(git rev-parse --short=8 moved) " actual.line
'

test_expect_success 'blame --reverse shows ?' '
	git blame -s --reverse initial.. --provenance file >actual &&
	test_grep ! "^[^ ]* [AMH] " actual
'

test_expect_success 'test-tool round-trips reasoning records' '
	cat >reasoning.out <<-EOF &&
	version 1
	commit $(git rev-parse second)
	session s1 model=m1 tool=t1 started=10 ended=20
	session s2 model=m2 tool=t2

	The summary.

	More.
	EOF
	test-tool provenance format-reasoning <reasoning.out >actual &&
	test_cmp reasoning.out actual &&
	sed -e "s/started=10/started=x/" reasoning.out >bad.out &&
	test_must_fail test-tool provenance format-reasoning <bad.out 2>err &&
	test_grep "line 3: malformed session .s1." err &&
	sed -e "/^commit /d" reasoning.out >bad.out &&
	test_must_fail test-tool provenance format-reasoning <bad.out 2>err &&
	test_grep "line 2: expected .commit. line" err
'

test_expect_success 'setup placeholders' '
	git stamp begin --session s6 --model mA &&
	test_write_lines a b c >pfile &&
	git add pfile &&
	git stamp end --session s6 &&
	git stamp begin --session s7 --model mB &&
	test_write_lines a b c d e f >pfile &&
	git stamp end --session s7 &&
	git stamp begin --session s8 --model mA &&
	test_write_lines a b c d e f g >pfile &&
	git stamp end --session s8 &&
	test_write_lines a b c d e f g h i >pfile &&
	printf "First line.\n\nSecond paragraph.\n" >summary.out &&
	git stamp reason --session s6 --summary summary.out &&
	git add pfile &&
	git commit -m placeholders &&
	git tag placeholders
'

test_expect_success '%(provenance:...) shows the summary, share, models and sessions' '
	cat >expect <<-\EOF &&
	78%|mA,mB|s6,s7,s8
	First line.

	Second paragraph.
	EOF
	git log -1 --format="%(provenance:share)|%(provenance:models)|%(provenance:sessions)%n%(provenance:summary)" placeholders >actual &&
	test_cmp expect actual &&
	git provenance show --reasoning placeholders >reasoning &&
	sed -e "1,/^\$/d" reasoning >expect &&
	git log -1 --format="%(provenance:summary)" placeholders >actual &&
	test_cmp expect actual
'

test_expect_success '%(provenance:share) rounds to the nearest percent' '
	git log -1 --format="%(provenance:share)" second >actual &&
	echo "75%" >expect &&
	test_cmp expect actual &&
	git log -1 --format="%(provenance:share)" initial >actual &&
	echo "0%" >expect &&
	test_cmp expect actual &&
	attach_record initial "file file" "+ 1-2 AI s9 $ZERO_OID" "+ 3 HUMAN human $ZERO_OID" &&
	git log -1 --format="%(provenance:share)" initial >actual &&
	echo "67%" >expect &&
	test_cmp expect actual &&
	attach_record initial "file file" "+ 1 MIXED s9,human $ZERO_OID" "+ 2-3 HUMAN human $ZERO_OID" &&
	git log -1 --format="%(provenance:share)" initial >actual &&
	echo "33%" >expect &&
	test_cmp expect actual &&
	attach_record initial "file file" "+ 1 HUMAN human $ZERO_OID" &&
	git log -1 --format="%(provenance:share)|%(provenance:models)|%(provenance:sessions)" initial >actual &&
	echo "0%|m9|s9" >expect &&
	test_cmp expect actual
'

test_expect_success '%(provenance:...) is empty without records' '
	git -c provenance.enabled=false commit --allow-empty -m none &&
	git log -1 --format="[%(provenance:share)|%(provenance:models)|%(provenance:sessions)|%(provenance:summary)]" >actual &&
	echo "[|||]" >expect &&
	test_cmp expect actual &&
	git log -1 --format="[%(provenance:summary)]" second >actual &&
	echo "[]" >expect &&
	test_cmp expect actual
'

test_expect_success 'unknown %(provenance:...) fields are shown as is' '
	git log -1 --format="%(provenance:nope)|%(provenance:share" second >actual &&
	echo "%(provenance:nope)|%(provenance:share" >expect &&
	test_cmp expect actual
'

test_done
