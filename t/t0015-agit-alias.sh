#!/bin/sh
#
# Copyright (c) 2026 M. Abdullah Onus
#

test_description='agit is an alias of the git executable'

. ./test-lib.sh

# Tools validate the executable by this prefix, so "agit" must keep
# reporting itself as "git version".
test_expect_success 'agit version reports git version' '
	agit version >actual &&
	test_grep "^git version " actual
'

test_expect_success 'agit uses the same exec-path as git' '
	git --exec-path >expect &&
	agit --exec-path >actual &&
	test_cmp expect actual
'

test_expect_success 'agit init creates a repository' '
	agit init repo &&
	echo .git >expect &&
	agit -C repo rev-parse --git-dir >actual &&
	test_cmp expect actual
'

test_expect_success 'agit add -h shows usage' '
	agit add -h >usage 2>err &&
	test_must_be_empty err &&
	test_grep "^usage: git add " usage
'

test_done
