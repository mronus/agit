#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='Test provenance records'

. ./test-lib.sh

# Attach a record to a commit.  "git notes" only writes below
# refs/notes/, so write the note through a scratch notes ref that
# starts from the current provenance ref, point the provenance ref at
# the resulting notes commit and delete the scratch ref.
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

# Print the anchor of lines <a> to <b> of <rev>:<path>.
#
# Usage: anchor <rev> <path> <a>[,<b>]
anchor () {
	git cat-file blob "$1:$2" >anchor.tmp &&
	sed -n "$3p" anchor.tmp | git hash-object --stdin &&
	rm -f anchor.tmp
}

test_expect_success 'setup' '
	# records are attached by hand below, not by "git commit"
	git config provenance.enabled false &&
	test_write_lines one two three >file &&
	git add file &&
	test_tick &&
	git commit -m first &&
	test_write_lines one TWO three four five >file &&
	test_tick &&
	git commit -a -m second &&
	cat >record <<-EOF
	version 1
	commit $(git rev-parse HEAD)
	capture observed git/0.1
	actor a1 ai model=some-model tool=some-tool session=s1 source=observed
	actor h1 human ident=A U Thor <author@example.com>
	file file
	+ 2 MIXED a1,h1 $(anchor HEAD file 2)
	+ 4-5 AI a1 $(anchor HEAD file 4,5)
	- 2 HUMAN h1 $(anchor HEAD^ file 2)
	EOF
'

test_expect_success 'verify reports commits without a record' '
	git rev-parse HEAD >oid &&
	echo "$(cat oid) no record" >expect &&
	git provenance verify >actual &&
	test_cmp expect actual
'

test_expect_success 'show fails without a record' '
	test_must_fail git provenance show HEAD 2>err &&
	test_grep "no provenance record" err &&
	test_must_fail git provenance show --reasoning HEAD
'

test_expect_success 'show prints the record' '
	add_record lines HEAD record &&
	git provenance show HEAD >actual &&
	test_cmp record actual
'

test_expect_success 'record round-trips through the parser' '
	test-tool provenance format <record >actual &&
	test_cmp record actual
'

test_expect_success 'parser ignores blank and comment lines' '
	{
		echo "# a comment" &&
		sed -n "1,5p" record &&
		echo &&
		sed -n "6,\$p" record
	} >commented &&
	test-tool provenance format <commented >actual &&
	test_cmp record actual
'

test_expect_success 'parser rejects an actor after a file block' '
	cat record >bad &&
	echo "actor h2 human ident=C O Mitter <committer@example.com>" >>bad &&
	test_must_fail test-tool provenance format <bad 2>err &&
	test_grep "line 10: .actor. line after .file. line" err
'

test_expect_success 'parser treats the observing tool as an opaque token' '
	sed -e "s|^capture .*|capture observed some-tool/1.2.3|" record >other &&
	test-tool provenance format <other >actual &&
	test_cmp other actual &&
	sed -e "s|^capture .*|capture observed git|" record >bad &&
	test_must_fail test-tool provenance format <bad 2>err &&
	test_grep "line 3: malformed .capture. line" err
'

test_expect_success 'show --reasoning prints the reasoning record' '
	cat >reasoning <<-EOF &&
	version 1
	commit $(git rev-parse HEAD)
	session s1 model=some-model tool=some-tool started=1112911993 ended=1112912053

	Capitalize the second line and add two more.
	EOF
	add_record reasoning HEAD reasoning &&
	git provenance show --reasoning HEAD >actual &&
	test_cmp reasoning actual
'

test_expect_success 'records are only kept below refs/provenance/' '
	cat >expect <<-\EOF &&
	refs/provenance/lines
	refs/provenance/reasoning
	EOF
	git for-each-ref --format="%(refname)" refs/provenance/ refs/notes/ >actual &&
	test_cmp expect actual
'

test_expect_success 'verify accepts a valid record' '
	echo "$(git rev-parse HEAD) ok" >expect &&
	git provenance verify HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'verify handles several commits' '
	cat >expect <<-EOF &&
	$(git rev-parse HEAD^) no record
	$(git rev-parse HEAD) ok
	EOF
	git provenance verify HEAD^ HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'verify rejects an invalid argument' '
	test_must_fail git provenance verify does-not-exist 2>err &&
	test_grep "not a valid commit" err
'

# Usage: test_verify_fails <description> <sed-expression> <message>
#
# Edit the valid record with <sed-expression>, attach it to HEAD and
# check that "verify" fails with <message>.
test_verify_fails () {
	test_expect_success "verify rejects $1" "
		test_when_finished 'add_record lines HEAD record' &&
		sed -e \"$2\" record >bad &&
		add_record lines HEAD bad &&
		test_must_fail git provenance verify HEAD >out 2>err &&
		test_must_be_empty out &&
		test_grep \"$3\" err
	"
}

test_expect_success 'verify rejects a wrong commit field' '
	test_when_finished "add_record lines HEAD record" &&
	parent=$(git rev-parse HEAD^) &&
	sed -e "s/^commit .*/commit $parent/" record >bad &&
	add_record lines HEAD bad &&
	test_must_fail git provenance verify HEAD >out 2>err &&
	test_must_be_empty out &&
	test_grep "record names commit .$parent." err
'

test_verify_fails 'an undeclared actor' \
	's/ a1,h1 / a1,h2 /' \
	"undeclared actor 'h2'"

test_verify_fails 'an unknown mark' \
	's/ MIXED / BOGUS /' \
	"unknown mark 'BOGUS'"

test_verify_fails 'UNKNOWN without capture adopted' \
	's/ MIXED a1,h1 / UNKNOWN - /' \
	'UNKNOWN mark requires'

test_verify_fails 'a path not in the tree' \
	's/^file file$/file nosuch/' \
	'path not found in the commit'

test_verify_fails 'a range past the end of the file' \
	's/^+ 4-5 /+ 4-6 /' \
	'range past end of file'

test_verify_fails 'a line number with a leading zero' \
	's/^+ 4-5 /+ 04-5 /' \
	'line 8: malformed line range'

test_verify_fails 'an anchor mismatch' \
	's/^+ 4-5 /+ 3-4 /' \
	'anchor mismatch'

test_verify_fails 'a malformed record' \
	's/^version 1$/version 2/' \
	"unsupported version '2'"

test_expect_success 'verify accepts UNKNOWN with capture adopted' '
	test_when_finished "add_record lines HEAD record" &&
	sed -e "s/^capture .*/capture adopted/" \
	    -e "s/ MIXED a1,h1 / UNKNOWN - /" record >adopted &&
	add_record lines HEAD adopted &&
	git provenance verify HEAD
'

test_expect_success 'verify rejects a - range on a root commit' '
	cat >root-record <<-EOF &&
	version 1
	commit $(git rev-parse HEAD^)
	capture declared
	actor h1 human ident=A U Thor <author@example.com>
	file file
	- 1 HUMAN h1 $(anchor HEAD^ file 1)
	EOF
	add_record lines HEAD^ root-record &&
	test_must_fail git provenance verify HEAD^ 2>err &&
	test_grep "range in a root commit" err
'

test_expect_success 'records are found below a fan-out directory' '
	saved=$(git rev-parse refs/provenance/lines) &&
	test_when_finished "git update-ref refs/provenance/lines $saved" &&
	hex=$(git rev-parse HEAD) &&
	blob=$(git hash-object -w record) &&
	sub=$(printf "100644 blob %s\t%s\n" $blob ${hex#??} | git mktree) &&
	dir=$(echo $hex | cut -c1-2) &&
	root=$(printf "040000 tree %s\t%s\n" $sub $dir | git mktree) &&
	git update-ref refs/provenance/lines $root &&
	git provenance show HEAD >actual &&
	test_cmp record actual &&
	echo "$hex ok" >expect &&
	git provenance verify HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'verify and show fail if the ref is not a tree' '
	saved=$(git rev-parse refs/provenance/lines) &&
	test_when_finished "git update-ref refs/provenance/lines $saved" &&
	blob=$(echo junk | git hash-object -w --stdin) &&
	git update-ref refs/provenance/lines $blob &&
	test_must_fail git provenance verify HEAD >out 2>err &&
	test_must_be_empty out &&
	test_grep "ref .refs/provenance/lines. does not point to a tree" err &&
	test_must_fail git provenance show HEAD 2>err &&
	test_grep "ref .refs/provenance/lines. does not point to a tree" err
'

test_expect_success 'verify rejects a path that is not a regular file' '
	blob=$(echo target | git hash-object -w --stdin) &&
	git update-index --add --cacheinfo 120000,$blob,link &&
	test_tick &&
	git commit -m symlink &&
	cat >link-record <<-EOF &&
	version 1
	commit $(git rev-parse HEAD)
	capture declared
	actor h1 human ident=A U Thor <author@example.com>
	file link
	+ 1 HUMAN h1 $blob
	EOF
	add_record lines HEAD link-record &&
	test_must_fail git provenance verify HEAD 2>err &&
	test_grep "path is not a regular file in the commit" err
'

test_done
