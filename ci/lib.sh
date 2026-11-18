# Library of functions shared by all CI scripts

if test true = "$GITHUB_ACTIONS"
then
	begin_group () {
		need_to_end_group=t
		echo "::group::$1" >&2
		set -x
	}

	end_group () {
		test -n "$need_to_end_group" || return 0
		set +x
		need_to_end_group=
		echo '::endgroup::' >&2
	}
elif test true = "$GITLAB_CI"
then
	begin_group () {
		need_to_end_group=t
		printf '\e[0Ksection_start:%s:%s[collapsed=true]\r\e[0K%s\n' \
			"$(date +%s)" "$(echo "$1" | tr ' ' _)" "$1"
		trap "end_group '$1'" EXIT
		set -x
	}

	end_group () {
		test -n "$need_to_end_group" || return 0
		set +x
		need_to_end_group=
		printf '\e[0Ksection_end:%s:%s\r\e[0K\n' \
			"$(date +%s)" "$(echo "$1" | tr ' ' _)"
		trap - EXIT
	}
else
	begin_group () { :; }
	end_group () { :; }

	set -x
fi

group () {
	group="$1"
	shift
	begin_group "$group"

	# work around `dash` not supporting `set -o pipefail`
	(
		"$@" 2>&1
		echo $? >exit.status
	) |
	sed 's/^\(\([^ ]*\):\([0-9]*\):\([0-9]*:\) \)\(error\|warning\): /::\5 file=\2,line=\3::\1/'
	res=$(cat exit.status)
	rm exit.status

	end_group "$group"
	return $res
}

begin_group "CI setup via $(basename $0)"

# Set 'exit on error' for all CI scripts to let the caller know that
# something went wrong.
#
# We already enabled tracing executed commands earlier. This helps by showing
# how # environment variables are set and dependencies are installed.
set -e

skip_branch_tip_with_tag () {
	# Sometimes, a branch is pushed at the same time the tag that points
	# at the same commit as the tip of the branch is pushed, and building
	# both at the same time is a waste.
	#
	# When the build is triggered by a push to a tag, $CI_BRANCH will
	# have that tagname, e.g. v2.14.0.  Let's see if $CI_BRANCH is
	# exactly at a tag, and if so, if it is different from $CI_BRANCH.
	# That way, we can tell if we are building the tip of a branch that
	# is tagged and we can skip the build because we won't be skipping a
	# build of a tag.

	if TAG=$(git describe --exact-match "$CI_BRANCH" 2>/dev/null) &&
		test "$TAG" != "$CI_BRANCH"
	then
		echo "$(tput setaf 2)Tip of $CI_BRANCH is exactly at $TAG$(tput sgr0)"
		exit 0
	fi
}

# Check whether we can use the path passed via the first argument as Git
# repository.
is_usable_git_repository () {
	# We require Git in our PATH, otherwise we cannot access repositories
	# at all.
	if ! command -v git >/dev/null
	then
		return 1
	fi

	# And the target directory needs to be a proper Git repository.
	if ! git -C "$1" rev-parse 2>/dev/null
	then
		return 1
	fi
}

# Save some info about the current commit's tree, so we can skip the build
# job if we encounter the same tree again and can provide a useful info
# message.
save_good_tree () {
	if ! is_usable_git_repository .
	then
		return
	fi

	echo "$(git rev-parse $CI_COMMIT^{tree}) $CI_COMMIT $CI_JOB_NUMBER $CI_JOB_ID" >>"$good_trees_file"
	# limit the file size
	tail -1000 "$good_trees_file" >"$good_trees_file".tmp
	mv "$good_trees_file".tmp "$good_trees_file"
}

# Skip the build job if the same tree has already been built and tested
# successfully before (e.g. because the branch got rebased, changing only
# the commit messages).
skip_good_tree () {
	if test true = "$GITHUB_ACTIONS"
	then
		return
	fi

	if ! is_usable_git_repository .
	then
		return
	fi

	if ! good_tree_info="$(grep "^$(git rev-parse $CI_COMMIT^{tree}) " "$good_trees_file")"
	then
		# Haven't seen this tree yet, or no cached good trees file yet.
		# Continue the build job.
		return
	fi

	echo "$good_tree_info" | {
		read tree prev_good_commit prev_good_job_number prev_good_job_id

		if test "$CI_JOB_ID" = "$prev_good_job_id"
		then
			cat <<-EOF
			$(tput setaf 2)Skipping build job for commit $CI_COMMIT.$(tput sgr0)
			This commit has already been built and tested successfully by this build job.
			To force a re-build delete the branch's cache and then hit 'Restart job'.
			EOF
		else
			cat <<-EOF
			$(tput setaf 2)Skipping build job for commit $CI_COMMIT.$(tput sgr0)
			This commit's tree has already been built and tested successfully in build job $prev_good_job_number for commit $prev_good_commit.
			The log of that build job is available at $SYSTEM_TASKDEFINITIONSURI$SYSTEM_TEAMPROJECT/_build/results?buildId=$prev_good_job_id
			To force a re-build delete the branch's cache and then hit 'Restart job'.
			EOF
		fi
	}

	exit 0
}

check_unignored_build_artifacts () {
	if ! is_usable_git_repository .
	then
		return
	fi

	! git ls-files --other --exclude-standard --error-unmatch \
		-- ':/*' 2>/dev/null ||
	{
		echo "$(tput setaf 1)error: found unignored build artifacts$(tput sgr0)"
		false
	}
}

handle_failed_tests () {
	return 1
}

create_failed_test_artifacts () {
	mkdir -p "${TEST_OUTPUT_DIRECTORY:-t}"/failed-test-artifacts

	for test_exit in "${TEST_OUTPUT_DIRECTORY:-t}"/test-results/*.exit
	do
		test 0 != "$(cat "$test_exit")" || continue

		test_name="${test_exit%.exit}"
		test_name="${test_name##*/}"
		printf "\\e[33m\\e[1m=== Failed test: ${test_name} ===\\e[m\\n"
		echo "The full logs are in the 'print test failures' step below."
		echo "See also the 'failed-tests-*' artifacts attached to this run."
		cat "${TEST_OUTPUT_DIRECTORY:-t}/test-results/$test_name.markup"

		trash_dir="${TEST_OUTPUT_DIRECTORY:-t}/trash directory.$test_name"
		cp "${TEST_OUTPUT_DIRECTORY:-t}/test-results/$test_name.out" "${TEST_OUTPUT_DIRECTORY:-t}"/failed-test-artifacts/
		tar czf "${TEST_OUTPUT_DIRECTORY:-t}/failed-test-artifacts/$test_name.trash.tar.gz" "$trash_dir"
	done
}

# GitHub Action doesn't set TERM, which is required by tput
export TERM=${TERM:-dumb}

# Clear MAKEFLAGS that may come from the outside world.
export MAKEFLAGS=

if test true = "$GITHUB_ACTIONS"
then
	CI_TYPE=github-actions
	CI_BRANCH="$GITHUB_REF"
	CI_COMMIT="$GITHUB_SHA"
	CI_OS_NAME="$(echo "$RUNNER_OS" | tr A-Z a-z)"
	test macos != "$CI_OS_NAME" || CI_OS_NAME=osx
	CI_REPO_SLUG="$GITHUB_REPOSITORY"
	CI_JOB_ID="$GITHUB_RUN_ID"
	CI_EVENT="$GITHUB_EVENT_NAME"
	CC="${CC_PACKAGE:-${CC:-gcc}}"
	DONT_SKIP_TAGS=t
	handle_failed_tests () {
		echo "FAILED_TEST_ARTIFACTS=${TEST_OUTPUT_DIRECTORY:-t}/failed-test-artifacts" >>$GITHUB_ENV
		create_failed_test_artifacts
		return 1
	}

	cache_dir="$HOME/none"

	GIT_TEST_OPTS="--github-workflow-markup"

	distro=$(echo "$CI_JOB_IMAGE" | tr : -)
elif test true = "$GITLAB_CI"
then
	# This environment is multiple kB in size and may cause us to exceed
	# xargs(1) limits on Windows.
	unset GITLAB_FEATURES

	CI_TYPE=gitlab-ci
	CI_BRANCH="$CI_COMMIT_REF_NAME"
	CI_COMMIT="$CI_COMMIT_SHA"

	case "$CI_PIPELINE_SOURCE" in
	merge_request_event)
		CI_EVENT=pull_request;;
	*)
		CI_EVENT="$CI_PIPELINE_SOURCE";;
	esac

	case "$OS,$CI_JOB_IMAGE" in
	Windows_NT,*)
		CI_OS_NAME=windows
		;;
	*,macos-*)
		# GitLab CI has Python installed via multiple package managers,
		# most notably via asdf and Homebrew. Ensure that our builds
		# pick up the Homebrew one by prepending it to our PATH as the
		# asdf one breaks tests.
		export PATH="$(brew --prefix)/bin:$PATH"

		CI_OS_NAME=osx
		;;
	*,almalinux:*|*,alpine:*|*,debian:*|*,fedora:*|*,ubuntu:*|*,i386/debian:*)
		CI_OS_NAME=linux
		;;
	*)
		echo "Could not identify OS image" >&2
		env >&2
		exit 1
		;;
	esac
	CI_REPO_SLUG="$CI_PROJECT_PATH"
	CI_JOB_ID="$CI_JOB_ID"
	CC="${CC_PACKAGE:-${CC:-gcc}}"
	DONT_SKIP_TAGS=t

	handle_failed_tests () {
		create_failed_test_artifacts
		return 1
	}

	cache_dir="$HOME/none"

	distro=$(echo "$CI_JOB_IMAGE" | tr : -)
else
	echo "Could not identify CI type" >&2
	env >&2
	exit 1
fi

case "$CI_OS_NAME" in
windows|windows_nt)
	JOBS=$NUMBER_OF_PROCESSORS
	;;
osx)
	JOBS=$(sysctl -n hw.logicalcpu)
	;;
*)
	JOBS=$(nproc)
	;;
esac
JOBS=$((2 * JOBS))

MAKEFLAGS="$MAKEFLAGS --jobs=$JOBS"
GIT_PROVE_OPTS="--timer --jobs $JOBS"

GIT_TEST_OPTS="$GIT_TEST_OPTS --verbose-log -x"
case "$CI_OS_NAME" in
windows|windows_nt)
	GIT_TEST_OPTS="$GIT_TEST_OPTS --no-chain-lint --no-bin-wrappers"
	;;
esac

export GIT_TEST_OPTS
export GIT_PROVE_OPTS

good_trees_file="$cache_dir/good-trees"

mkdir -p "$cache_dir"

test -n "${DONT_SKIP_TAGS-}" ||
skip_branch_tip_with_tag
skip_good_tree

if test -z "$jobname"
then
	jobname="$CI_OS_NAME-$CC"
fi

export DEVELOPER=1
export DEFAULT_TEST_TARGET=prove
export GIT_TEST_CLONE_2GB=true
export SKIP_DASHED_BUILT_INS=YesPlease

# In order to give maximum test coverage to contributor builds,
# preferrably even before the changes consume public review bandwidth,
# enable "expensive" tests for PR events.
# In order to catch bugs introduced at integration time by mismerges,
# enable the long tests for pushes to the integration branches as well.
case "$CI_EVENT,$CI_BRANCH" in
pull_request,*|push,*next*|push,*master*|push,*main*|push,*maint*)
	export GIT_TEST_LONG=${GIT_TEST_LONG:-true}
	;;
esac

case "$distro" in
ubuntu-*)
	MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=/usr/bin/python3"

	export GIT_TEST_HTTPD=true

	# The Linux build installs the defined dependency versions below.
	# The OS X build installs much more recent versions, whichever
	# were recorded in the Homebrew database upon creating the OS X
	# image.
	# Keep that in mind when you encounter a broken OS X build!
	export LINUX_GIT_LFS_VERSION="1.5.2"
	;;
macos-*)
	MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=$(which python3)"
	if [ "$jobname" != osx-gcc ]
	then
		MAKEFLAGS="$MAKEFLAGS APPLE_COMMON_CRYPTO_SHA1=Yes"
	fi
	;;
esac

CUSTOM_PATH="${CUSTOM_PATH:-$HOME/path}"
export PATH="$CUSTOM_PATH:$PATH"

case "$CI_OS_NAME" in
linux)
	export GIT_TEST_USE_SET_E=yes
	;;
esac

case "$jobname" in
*breaking-changes*)
	export WITH_BREAKING_CHANGES=YesPlease
	MESONFLAGS="$MESONFLAGS -Dbreaking_changes=true"
	;;
esac

case "$jobname" in
*reftable*)
	export GIT_TEST_DEFAULT_REF_STORAGE_FORMAT=reftable
	;;
esac

case "$jobname" in
*sha256*)
	export GIT_TEST_DEFAULT_HASH=sha256
	;;
esac

case "$jobname" in
linux32)
	CC=gcc
	;;
linux-feature-testbed)
	export OPENSSL_SHA1_UNSAFE=YesPlease
	export GIT_TEST_SPLIT_INDEX=yes
	export GIT_TEST_FULL_IN_PACK_ARRAY=true
	export GIT_TEST_OE_SIZE=10
	export GIT_TEST_OE_DELTA_SIZE=5
	export GIT_TEST_COMMIT_GRAPH=1
	export GIT_TEST_COMMIT_GRAPH_CHANGED_PATHS=1
	export GIT_TEST_MULTI_PACK_INDEX=1
	export GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL=1
	export GIT_TEST_NO_WRITE_REV_INDEX=1
	export GIT_TEST_CHECKOUT_WORKERS=2
	export GIT_TEST_PACK_USE_BITMAP_BOUNDARY_TRAVERSAL=1
	;;
linux-meson)
	MESONFLAGS="$MESONFLAGS -Dcredential_helpers=libsecret,netrc"
	;;
linux-musl-meson)
	MESONFLAGS="$MESONFLAGS -Dtest_utf8_locale=C.UTF-8 -Drust=disabled"
	;;
linux-leaks|linux-reftable-leaks)
	export SANITIZE=leak
	export NO_CVS_TESTS=LetsSaveSomeTime
	export NO_SVN_TESTS=LetsSaveSomeTime
	export NO_P4_TESTS=LetsSaveSomeTime
	GIT_TEST_OPTS="$GIT_TEST_OPTS --immediate"
	;;
linux-asan-ubsan)
	export SANITIZE=address,undefined
	export NO_SVN_TESTS=LetsSaveSomeTime
	MAKEFLAGS="$MAKEFLAGS NO_PYTHON=YepBecauseP4FlakesTooOften NO_RUST=Yes"
	;;
linux-provenance)
	# Run the suite as users get it, with provenance recorded by
	# default.  Skip the tests that only expect what stock Git
	# prints or stores, one line per script, grouped by reason, with
	# the tests that depend on what a skipped test sets up.
	export GIT_TEST_PROVENANCE=true
	GIT_SKIP_TESTS="$GIT_SKIP_TESTS $(sed -e 's/#.*//' <<-\EOF
	# The advice.provenanceSession hint on stderr.
	t7502.80
	# Commits outside of the tests print the hint into the TAP
	# output, which fails the whole script.
	t4041 t4060 t7401
	# Ref listings (for-each-ref, ls-remote, show-ref, completion,
	# hooks, fetch and push output) show the provenance refs.
	t0002.12 t0002.13 t0002.14
	t0041.14
	t0610.39 t0610.48 t0610.57 t0610.59 t0610.63
	t0611.1
	t1408.1 t1408.2
	t1419.11 t1419.12 t1419.13
	t1461.219 t1461.220 t1461.226 t1461.227 t1461.228 t1461.229
	t1461.246 t1461.247 t1461.248 t1461.249 t1461.256
	t3211.4 t3211.5 t3211.7
	t5400.17
	t5411 # push output in most of its tests
	t5503.3 t5503.6 t5503.9 t5503.11 t5503.12
	t5509.2 t5509.3 t5509.4 t5509.5 t5509.6 t5509.7
	t5512.31 t5512.32 t5512.33
	t5513.2
	t5515 # the refs after fetch, in all of its tests
	t5526 # fetch output, in most of its tests
	t5541.10
	t5543.9 t5543.10 t5543.11 t5543.12 t5543.13
	t5548 # push output, in most of its tests
	t5550.5 t5550.6
	t5555.4 t5555.5 t5555.6 t5555.7 t5555.8 t5555.10
	t5558.18 t5558.21
	t5571 # the refs given to the hook, from its setup on
	t5574.2 t5574.3 t5574.5 t5574.6 t5574.7 t5574.9 t5574.10
	t5607.3
	t5611.6 t5611.7 t5611.8
	t5618.3 t5618.6
	t5701.13 t5701.17
	t5702.47
	t5702.71 # the push retry fetches the provenance refs with v2
	t6300.220 t6300.221 t6300.227 t6300.228 t6300.229 t6300.230
	t6300.247 t6300.248 t6300.249 t6300.250 t6300.257
	t6302 # for-each-ref output, in most of its tests
	t9902.59 t9902.62 t9902.65 t9902.67 t9902.70 t9902.78 t9902.80
	t9902.103 t9902.104 t9902.225
	# Walks of all refs (--all, --branches=*, bundles, fast-export)
	# include the provenance notes commits.
	t0062.2
	t1410.9 t1410.10
	t4013.102 t4013.103 t4013.104 t4013.105
	t4207.2
	t4218.20 t4218.21 t4218.22
	t5620.18
	t6000.18
	t6009.5 t6009.6 t6009.7 t6009.8 t6009.11 t6009.13
	t6011.2
	t6014.2
	t6016.2 t6016.3 t6016.5 t6016.9 t6016.10 t6016.11 t6016.12
	t6018.18 t6018.19 t6018.29 t6018.32 t6018.40 t6018.43
	t6018.51 t6018.54 t6018.86 t6018.87
	t6020.8 t6020.13 t6020.14 t6020.19 t6020.20 t6020.21 t6020.22
	t6020.29
	t6113.14
	t6120.96 t6120.98
	t6601.2 t6601.17 t6601.19 t6601.20 t6601.21 t6601.24 t6601.26
	t6601.27 t6601.28 t6601.33
	t9350.39 t9350.41 t9350.42 t9350.43
	t9351.17
	# Counts of objects, packs, commits or refs include the
	# provenance records and refs.
	t0601.41 t0601.42 t0601.43 t0601.44 t0601.45 t0601.46
	t0613.1 t0613.2 t0613.3 t0613.5 t0613.8 t0613.10 t0613.11
	t1006.377 t1006.378 t1006.379 t1006.380 t1006.381 t1006.382
	t1463.41 t1463.42 t1463.43 t1463.44 t1463.45 t1463.46
	t1901.4
	t4216.2 t4216.137 t4216.138 t4216.139 t4216.140 t4216.141
	t4216.142 t4216.154 t4216.160 t4216.161 t4216.163 t4216.165
	t5310.5 t5310.6 t5310.14 t5310.23 t5310.35 t5310.44 t5310.47
	t5310.51 t5310.52 t5310.53 t5310.81 t5310.82 t5310.90 t5310.99
	t5310.111 t5310.120 t5310.123 t5310.127 t5310.128 t5310.129
	t5310.163 t5310.164 t5310.172 t5310.181 t5310.193 t5310.202
	t5310.205 t5310.209 t5310.210 t5310.211
	t5318.12 t5318.25 t5318.32 t5318.35 t5318.38 t5318.47
	t5318.53 t5318.62 t5318.99 t5318.100
	t5319.62 t5319.64 t5319.65 t5319.66 t5319.77 t5319.95 t5319.97
	t5319.98
	t5324 # commit counts, in most of its tests once its setup is skipped
	t5326.46 t5326.48 t5326.49 t5326.50 t5326.91 t5326.167
	t5326.220 t5326.222 t5326.223 t5326.224
	t5326.265 t5326.341
	t5327.46 t5327.48 t5327.49 t5327.50 t5327.91 t5327.157
	t5327.203 t5327.205 t5327.206 t5327.207
	t5327.248 t5327.314
	t5328 # commit counts, in most of its tests once its setup is skipped
	t5329.1 t5329.2 t5329.7 t5329.8 t5329.13 t5329.14 t5329.15
	t5329.16
	t5331.5 t5331.6 t5331.11
	t5332.1 t5332.2 t5332.3 t5332.4 t5332.6 t5332.7 t5332.11
	t5333.17
	t5335.8
	t5500.43 t5500.44 t5500.46
	t5501.2 t5501.3
	t5502 # object counts, in most of its tests
	t5537.12
	t5538.7 t5538.8
	t5552.6
	t5604.14 t5604.16
	t6500.11 t6500.35
	t7700.26
	t7703.7
	t7704.2 t7704.12 t7704.14 t7704.15
	t7705.3
	t7900.19 t7900.31 t7900.52
	# The Provenance: block of format-patch, in mails compared
	# with what stock Git writes.
	t4013.139 t4013.140 t4013.141 t4013.142 t4013.143 t4013.144
	t4013.145 t4013.146 t4013.147 t4013.148 t4013.149 t4013.150
	t4013.151 t4013.152 t4013.154 t4013.155
	t4014.74
	t9001.38 # a line number in the mail
	# The tests that use the patch t9001.38 writes, or the hook
	# that those set up.
	t9001.42 t9001.44 t9001.46 t9001.47 t9001.48 t9001.49 t9001.50
	t9001.51 t9001.52 t9001.53
	# Objects are deleted or packs crafted by hand, leaving the
	# provenance refs pointing to objects that are gone.
	t0410.11 t0410.13 t0410.14 t0410.15 t0410.27 t0410.40
	t5616.47 t5616.48
	t6011.6
	t7700.13
	t7900.27 t7900.53
	# A depth-limited clone makes the tips of the provenance refs
	# shallow as well, and the two clones list the same shallow
	# points in another order.
	t5601.99
	EOF
	)"
	export GIT_SKIP_TESTS
	;;
osx-meson)
	MESONFLAGS="$MESONFLAGS -Dcredential_helpers=osxkeychain"
	;;
esac

MAKEFLAGS="$MAKEFLAGS CC=${CC:-cc}"
export TEST_CONTRIB_TOO=yes

end_group "CI setup via $(basename $0)"
set -x
