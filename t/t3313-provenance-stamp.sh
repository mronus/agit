#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test git stamp and the provenance capture journal'

. ./test-lib.sh

# Print the journal without the timestamps.
#
# Usage: list_entries [<git-stamp-list-option>...]
list_entries () {
	git stamp list "$@" >list.out &&
	sed -e "s/^\([a-z]*\) [0-9][0-9]* /\1 /" list.out
}

# Print the object id in the last <type> entry of the journal.
#
# Usage: last_oid (begin|end)
last_oid () {
	list_entries >entries.out &&
	sed -n -e "s/^$1 [^ ]* \([0-9a-f]*\).*/\1/p" entries.out | tail -n 1
}

# Remember the index, and check later that it has not changed.
save_index () {
	cp .git/index index.saved &&
	git ls-files -s >index.ls.saved
}

check_index () {
	cmp .git/index index.saved &&
	git ls-files -s >index.ls.now &&
	test_cmp index.ls.saved index.ls.now &&
	git diff-index --cached --quiet HEAD
}

test_expect_success 'setup' '
	cat >.git/info/exclude <<-\EOF &&
	/.gitconfig
	*.out
	*.saved
	*.now
	/expect*
	/actual*
	/err
	/pre*
	/post*
	/summary*
	/transcript*
	/patch*
	/wt/
	EOF
	echo "*.ignored" >.gitignore &&
	echo "*.txt text" >.gitattributes &&
	test_write_lines one two three >file1 &&
	test_write_lines four five six >file2 &&
	mkdir sub &&
	echo seven >sub/file3 &&
	git add . &&
	test_tick &&
	git commit -m initial
'

test_expect_success 'session is required and must be valid' '
	test_must_fail git stamp begin 2>err &&
	test_grep "option .--session. is required" err &&
	test_must_fail git stamp end --session "has space" 2>err &&
	test_grep "invalid session id .has space." err &&
	test_must_fail git stamp claim --session "a/b" file1 2>err &&
	test_grep "invalid session id .a/b." err &&
	test_must_fail git stamp reason --session "" --summary file1 2>err &&
	test_grep "invalid session id" err &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'begin and end snapshot the whole working tree' '
	test_when_finished "rm -f .git/provenance/journal" &&
	save_index &&
	git stamp begin --session s1 --model some-model --tool some-tool &&
	check_index &&
	echo four >>file1 &&
	git stamp end --session s1 &&
	check_index &&
	git checkout file1 &&

	begin=$(last_oid begin) &&
	end=$(last_oid end) &&
	cat >expect <<-EOF &&
	begin s1 $begin model=some-model tool=some-tool
	end s1 $end
	EOF
	list_entries >actual &&
	test_cmp expect actual &&

	echo tree >expect &&
	git cat-file -t $begin >actual &&
	test_cmp expect actual &&
	git cat-file -t $end >actual &&
	test_cmp expect actual &&
	git rev-parse HEAD^{tree} >expect &&
	echo $begin >actual &&
	test_cmp expect actual &&

	echo file1 >expect &&
	git diff-tree --name-only $begin $end >actual &&
	test_cmp expect actual
'

test_expect_success 'snapshot adds untracked files and removes deleted ones' '
	test_when_finished "rm -f .git/provenance/journal new file.ignored" &&
	test_when_finished "git checkout file2" &&
	save_index &&
	echo new >new &&
	echo ignored >file.ignored &&
	rm file2 &&
	git stamp begin --session s1 &&
	check_index &&
	tree=$(last_oid begin) &&
	git ls-tree -r --name-only $tree >actual &&
	cat >expect <<-\EOF &&
	.gitattributes
	.gitignore
	file1
	new
	sub/file3
	EOF
	test_cmp expect actual
'

test_expect_success 'pathspec limits what is taken from the working tree' '
	test_when_finished "rm -f .git/provenance/journal new sub/new" &&
	test_when_finished "git checkout file1 file2" &&
	save_index &&
	echo changed >>file1 &&
	echo changed >>file2 &&
	echo new >new &&
	echo new >sub/new &&
	git stamp begin --session s1 -- file1 sub &&
	check_index &&
	tree=$(last_oid begin) &&

	git hash-object file1 >expect &&
	git rev-parse $tree:file1 >actual &&
	test_cmp expect actual &&
	git rev-parse :file2 >expect &&
	git rev-parse $tree:file2 >actual &&
	test_cmp expect actual &&
	test "$(git hash-object file2)" != "$(cat expect)" &&

	git ls-tree -r --name-only $tree >actual &&
	test_grep "^sub/new$" actual &&
	test_grep ! "^new$" actual
'

test_expect_success 'pathspec is relative to the current directory' '
	test_when_finished "rm -f .git/provenance/journal" &&
	test_when_finished "git checkout file1 sub/file3" &&
	echo changed >>file1 &&
	echo changed >>sub/file3 &&
	(
		cd sub &&
		git stamp end --session s1 file3
	) &&
	tree=$(last_oid end) &&
	echo sub/file3 >expect &&
	git diff-tree -r --name-only HEAD $tree >actual &&
	test_cmp expect actual
'

test_expect_success 'edit records the pre- and post-image' '
	test_when_finished "rm -f .git/provenance/journal" &&
	save_index &&
	echo old >pre &&
	echo new >post &&
	git stamp edit --session s2 --path file1 --pre pre --post post &&
	check_index &&
	echo "edit s2 $(git hash-object pre) $(git hash-object post) file1" >expect &&
	list_entries >actual &&
	test_cmp expect actual &&
	git cat-file blob $(git hash-object post) >actual &&
	test_cmp post actual
'

test_expect_success 'edit takes the post-image from the working tree' '
	test_when_finished "rm -f .git/provenance/journal" &&
	test_when_finished "git checkout file1" &&
	save_index &&
	cp file1 pre &&
	echo four >>file1 &&
	(
		cd sub &&
		git stamp edit --session s2 --path ../file1 --pre ../pre
	) &&
	check_index &&
	echo "edit s2 $(git hash-object pre) $(git hash-object file1) file1" >expect &&
	list_entries >actual &&
	test_cmp expect actual
'

test_expect_success 'edit converts contents as git add does' '
	test_when_finished "rm -f .git/provenance/journal" &&
	printf "a\r\nb\r\n" >pre &&
	printf "a\r\nc\r\n" >post &&
	git stamp edit --session s2 --path new.txt --pre pre --post post &&
	pre=$(printf "a\nb\n" | git hash-object --stdin) &&
	post=$(printf "a\nc\n" | git hash-object --stdin) &&
	echo "edit s2 $pre $post new.txt" >expect &&
	list_entries >actual &&
	test_cmp expect actual
'

test_expect_success 'edit rejects a missing file' '
	test_must_fail git stamp edit --session s2 --path file1 \
		--pre does-not-exist 2>err &&
	test_grep "does-not-exist" err &&
	test_must_fail git stamp edit --session s2 --path does-not-exist \
		--pre file1 2>err &&
	test_grep "does-not-exist" err &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'reason stores the summary and the transcript' '
	test_when_finished "rm -f .git/provenance/journal" &&
	save_index &&
	echo "Add a fourth line." >summary &&
	echo "A transcript." >transcript &&
	git stamp reason --session s3 --summary summary &&
	echo "From stdin." | git stamp reason --session s3 --summary - \
		--transcript transcript &&
	check_index &&
	summary=$(git hash-object summary) &&
	stdin=$(echo "From stdin." | git hash-object --stdin) &&
	transcript=$(git hash-object transcript) &&
	cat >expect <<-EOF &&
	reason s3 $summary
	reason s3 $stdin $transcript
	EOF
	list_entries >actual &&
	test_cmp expect actual &&
	git cat-file blob $stdin >actual &&
	echo "From stdin." >expect &&
	test_cmp expect actual &&
	git cat-file blob $transcript >actual &&
	test_cmp transcript actual
'

test_expect_success 'reason rejects a missing file' '
	test_must_fail git stamp reason --session s3 --summary does-not-exist 2>err &&
	test_grep "does-not-exist" err &&
	test_must_fail git stamp reason --session s3 2>err &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'claim records paths and patches' '
	test_when_finished "rm -f .git/provenance/journal" &&
	save_index &&
	git diff HEAD >patch &&
	(
		cd sub &&
		git stamp claim --session s4 file3 "with space" &&
		git stamp claim --session s4 --patch ../patch
	) &&
	check_index &&
	cat >expect <<-EOF &&
	claim s4 path sub/file3
	claim s4 path sub/with space
	claim s4 patch $(git hash-object patch)
	EOF
	list_entries >actual &&
	test_cmp expect actual
'

test_expect_success 'claim rejects a path with a newline' '
	test_must_fail git stamp claim --session s4 file1 "$(printf "a\nb")" 2>err &&
	test_grep "contains a newline" err &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'claim needs either paths or a patch' '
	echo >patch &&
	test_must_fail git stamp claim --session s4 &&
	test_must_fail git stamp claim --session s4 --patch patch file1 &&
	test_path_is_missing .git/provenance/journal
'

test_expect_success 'list shows all entries or those of one session' '
	test_when_finished "rm -f .git/provenance/journal" &&
	git stamp claim --session s5 file1 &&
	git stamp claim --session s6 file2 &&
	git stamp claim --session s5 sub/file3 &&
	cat >expect <<-\EOF &&
	claim s5 path file1
	claim s6 path file2
	claim s5 path sub/file3
	EOF
	list_entries >actual &&
	test_cmp expect actual &&
	cat >expect <<-\EOF &&
	claim s5 path file1
	claim s5 path sub/file3
	EOF
	list_entries --session s5 >actual &&
	test_cmp expect actual &&
	list_entries --session s7 >actual &&
	test_must_be_empty actual
'

test_expect_success 'list skips malformed lines' '
	test_when_finished "rm -f .git/provenance/journal" &&
	git stamp claim --session s5 file1 &&
	{
		echo garbage &&
		echo "end 1 s5" &&
		echo "claim 1 s5 path" &&
		echo "end 1 s5 $ZERO_OID trailing"
	} >>.git/provenance/journal &&
	echo "end 1 s5 $ZERO_OID" >>.git/provenance/journal &&
	git stamp claim --session s5 file2 &&
	cat >expect <<-EOF &&
	claim s5 path file1
	end s5 $ZERO_OID
	claim s5 path file2
	EOF
	list_entries >actual 2>err &&
	test_cmp expect actual &&
	test_grep "ignoring malformed line 2 " err &&
	test_grep "ignoring malformed line 5 " err &&
	test_grep ! "ignoring malformed line 6 " err
'

test_expect_success 'linked worktrees have their own journal' '
	test_when_finished "rm -f .git/provenance/journal" &&
	git worktree add wt &&
	test_when_finished "git worktree remove --force wt" &&
	git stamp claim --session main file1 &&
	git -C wt stamp begin --session linked &&
	echo changed >>wt/file1 &&
	git -C wt stamp end --session linked &&
	test_path_is_file .git/worktrees/wt/provenance/journal &&

	echo "claim main path file1" >expect &&
	list_entries >actual &&
	test_cmp expect actual &&

	git -C wt stamp list >list.out &&
	test_line_count = 2 list.out &&
	test_grep ! main list.out &&
	begin=$(sed -n "s/^begin [^ ]* linked \(.*\)/\1/p" list.out) &&
	end=$(sed -n "s/^end [^ ]* linked \(.*\)/\1/p" list.out) &&
	echo file1 >expect &&
	git diff-tree --name-only $begin $end >actual &&
	test_cmp expect actual
'

test_done
