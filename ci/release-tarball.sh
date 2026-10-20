#!/bin/sh
#
# Build a relocatable binary tarball of Git and smoke-test it
#
#   ci/release-tarball.sh <os>-<arch> <output-dir> [<manpages-tarball>]
#   ci/release-tarball.sh --smoke-test <tarball>
#
# <os>-<arch> is one of darwin-arm64, darwin-x86_64, linux-x86_64 and
# linux-aarch64, and must match the machine the script runs on.  The
# first form writes agit-<version>-<os>-<arch>.tar.gz and its .sha256
# into <output-dir>.  The tarball unpacks into a single directory with
# bin/, libexec/git-core/ and share/ (and share/man/ when a man-pages
# tarball is given) that can be moved anywhere.  The second form only
# runs the smoke test on an existing tarball.
#
# The build uses:
#
#   RUNTIME_PREFIX     find libexec, templates and the Perl modules
#                      relative to the executable, so the tree can be
#                      unpacked anywhere
#   NO_GETTEXT         English only, no libintl at runtime
#   NO_TCLTK           no gitk and git-gui
#   NO_OPENSSL         hashes come from Git's own code; HTTPS goes
#                      through libcurl and the platform's CA bundle
#   INSTALL_STRIP=-s   strip the installed binaries
#
# and, on macOS, Apple's CommonCrypto and nothing from Homebrew, Fink
# or MacPorts, and, on Linux, NO_EXPAT (expat only serves http-push
# over WebDAV).  Rust is built in as in CI; set NO_RUST=1 in the
# environment to leave it out where cargo is missing.
#
# Runtime requirements: on macOS, nothing beyond /usr/lib and /System
# (macOS 12 or later); on Linux, glibc as old as the one the tarball
# was built with, and libcurl (libcurl.so.4) and zlib (libz.so.1).
# The script checks the binaries' dependencies against these lists.
#

set -ex

usage () {
	echo >&2 "usage: $0 <os>-<arch> <output-dir> [<manpages-tarball>]"
	echo >&2 "   or: $0 --smoke-test <tarball>"
	exit 1
}

# Check that the binaries in the staged tree $1 only depend on the
# system libraries listed in the header.
check_libs () {
	git_bin="$1/bin/git"
	for f in "$git_bin" "$1"/libexec/git-core/*
	do
		if test "$f" != "$git_bin" && test "$f" -ef "$git_bin"
		then
			continue
		fi
		case "$os" in
		darwin)
			file "$f" | grep -q Mach-O || continue
			libs=$(otool -L "$f" | sed -e 1d -e 's/^[	 ]*//' -e 's/ (.*//')
			;;
		linux)
			readelf -h "$f" >/dev/null 2>&1 || continue
			libs=$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')
			;;
		esac
		for lib in $libs
		do
			case "$os,$lib" in
			darwin,/usr/lib/*|darwin,/System/*)
				;;
			linux,libc.so.*|linux,libm.so.*|linux,libpthread.so.*|\
			linux,libdl.so.*|linux,librt.so.*|linux,libgcc_s.so.*|\
			linux,ld-linux*|linux,libcurl.so.4|linux,libz.so.1)
				;;
			*)
				echo >&2 "error: ${f#$1/} depends on $lib"
				exit 1
				;;
			esac
		done
	done
}

# Unpack the tarball $1 in a fresh directory and check that the Git in
# it works from there.
smoke_test () {
	tarball=$1
	name=$(basename "$tarball" .tar.gz)
	version=${name#agit-}
	version=${version%-*-*}

	dir=$(mktemp -d)
	tar -xzf "$tarball" -C "$dir"
	cd "$dir"
	dir=$(pwd -P)
	top="$dir/$name"
	git="$top/bin/git"

	HOME="$dir"
	GIT_CONFIG_NOSYSTEM=1
	export HOME GIT_CONFIG_NOSYSTEM

	test "$("$git" version)" = "git version $version"
	case "$("$git" --exec-path)" in
	"$top"/*) ;;
	*) exit 1 ;;
	esac
	if test -d "$top/share/man"
	then
		case "$("$git" --man-path)" in
		"$top"/*) ;;
		*) exit 1 ;;
		esac
	fi

	"$git" stamp -h >stamp-usage
	grep "usage: git stamp begin" stamp-usage

	# The Makefile replaces the Perl scripts' fixed library path with
	# code that finds the modules relative to the script.
	head -3 "$top/libexec/git-core/git-send-email" >perl-header
	grep "^# BEGIN RUNTIME_PREFIX generated code" perl-header

	"$git" ls-remote https://github.com/mronus/agit.git HEAD

	"$git" init repo
	cd repo
	"$git" config user.name "Release Smoke Test"
	"$git" config user.email "smoke@example.com"
	echo one >file
	"$git" stamp begin --session smoke
	echo two >>file
	"$git" stamp end --session smoke
	"$git" add file
	"$git" commit -m x
	"$git" provenance show HEAD >show
	cat show
	grep "AI" show
	"$git" log -1 --format='%(provenance:share)' >share
	cat share
	grep "^[0-9][0-9]*%$" share

	cd /
	rm -rf "$dir"
}

if test "$1" = --smoke-test
then
	test $# = 2 || usage
	smoke_test "$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
	exit 0
fi

test $# = 2 || test $# = 3 || usage

platform=$1
case "$platform" in
darwin-arm64|darwin-x86_64|linux-x86_64|linux-aarch64)
	;;
*)
	usage
	;;
esac
os=${platform%-*}
if test "$(uname -s | tr A-Z a-z)-$(uname -m)" != "$platform"
then
	echo >&2 "error: cannot build $platform on $(uname -s)-$(uname -m)"
	exit 1
fi

mkdir -p "$2"
out=$(cd "$2" && pwd)
manpages=
if test -n "$3"
then
	manpages="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
fi

make GIT-VERSION-FILE
version=$(sed -n 's/^GIT_VERSION *= *//p' GIT-VERSION-FILE)
name="agit-$version-$platform"

case "$os" in
darwin)
	MACOSX_DEPLOYMENT_TARGET=12.0
	export MACOSX_DEPLOYMENT_TARGET
	jobs=$(sysctl -n hw.logicalcpu)
	set -- APPLE_COMMON_CRYPTO=YesPlease NO_FINK=YesPlease \
		NO_DARWIN_PORTS=YesPlease NO_HOMEBREW=YesPlease \
		CURL_CONFIG=/usr/bin/curl-config
	;;
linux)
	jobs=$(nproc)
	set -- NO_EXPAT=YesPlease
	;;
esac

stage=$(mktemp -d)
make -j"$jobs" install DESTDIR="$stage/$name" prefix=/ \
	RUNTIME_PREFIX=YesPlease NO_GETTEXT=YesPlease NO_TCLTK=YesPlease \
	NO_OPENSSL=YesPlease INSTALL_STRIP=-s \
	PERL_PATH=/usr/bin/perl PYTHON_PATH=/usr/bin/python3 V=1 "$@"

if test -n "$manpages"
then
	tar -xzf "$manpages" -C "$stage/$name"
fi

check_libs "$stage/$name"

# Keep macOS tar from adding AppleDouble files for extended attributes.
COPYFILE_DISABLE=1
export COPYFILE_DISABLE
tar -czf "$out/$name.tar.gz" -C "$stage" "$name"
rm -rf "$stage"

(
	cd "$out" &&
	case "$os" in
	darwin) shasum -a 256 "$name.tar.gz" ;;
	linux) sha256sum "$name.tar.gz" ;;
	esac >"$name.tar.gz.sha256"
)

smoke_test "$out/$name.tar.gz"

ls -l "$out/$name.tar.gz"
