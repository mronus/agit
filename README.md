[![CI](https://github.com/mronus/agit/actions/workflows/main.yml/badge.svg?branch=master&event=push)](https://github.com/mronus/agit/actions/workflows/main.yml?query=branch%3Amaster+event%3Apush)
[![Release](https://github.com/mronus/agit/actions/workflows/release.yml/badge.svg?event=push)](https://github.com/mronus/agit/actions/workflows/release.yml?query=event%3Apush)

AGit - agentic, fast, scalable, distributed revision control system
===================================================================

AGit is Git with the *provenance* extension. For every commit it
records why the change was made and, for every changed line, who wrote
it: an AI agent, a human, or both. The records live in notes-style refs
under `refs/provenance/`, outside the commit objects, so an AGit
repository stays a plain Git repository that any Git client and any
hosting service can work with. The executable is `git`, and everything
Git does works as before.

Install
-------

    curl -fsSL https://raw.githubusercontent.com/mronus/agit-setup/main/install.sh | sh

The installer downloads a prebuilt release for macOS (12 or later,
arm64 or x86_64) or Linux (glibc 2.31 or later, x86_64 or aarch64),
checks it against the release's checksums, installs it under
`~/.agit`, and puts it first on your `PATH`, so that it becomes your
`git` without touching the Git that came with your system or Homebrew.
It needs no root and builds nothing. At the end it offers to connect
your coding agents. To remove everything it added:

    curl -fsSL https://raw.githubusercontent.com/mronus/agit-setup/main/install.sh | sh -s -- --uninstall

Run it with `--help` for its options. To build from source instead,
see [INSTALL][].

Connecting coding agents
------------------------

AGit knows which lines an agent wrote because the agent's harness tells
it, through `git stamp`, while the agent edits files (see
[git-stamp(1)][]). `agit-setup`, installed with AGit, connects the
agents found on your machine through their hooks; run it again at any
time to change the choice. The hooks and the list of supported agents
are maintained in [mronus/agit-setup][]; this repository stays agent
agnostic. Nothing has to change in how you or the agent use Git: the
next `git commit` picks the stamps up.

What you get
------------

Each line a commit changes is marked `A` (AI), `M` (mixed: touched by
both), `H` (human) or `?` (unknown: made without provenance, such as
history from before AGit or commits made by other Git versions).

    git show --provenance <commit>         # the diff, with a mark per line
    git blame --provenance <file>          # who wrote each line of a file
    git log --format='%h %(provenance:share) %s'
                                           # the AI share of each commit
    git provenance show [--reasoning] <commit>
                                           # the line record, or the reasoning
    git provenance stat <range>            # totals by mark over a range

Rebases, cherry-picks, reverts, merges, amends, `git am` and
`format-patch` carry the records along with the commits. `git
provenance adopt` gives records to commits made without them, and
[git-provenance(1)][] covers the rest: verifying, remapping after a
`fast-import`, purging reasoning, pruning and statistics.

Remotes
-------

`git fetch`, `git pull`, `git push` and `git clone` carry the records
with the branches: when a branch is pushed, its records go along to the
same remote. Hosting services store the refs but do not show them. With
a Git without the extension, fetch them explicitly:

    git fetch origin 'refs/provenance/*:refs/provenance/*'

Commits created by a hosting service, such as the merge commit of a
pull request, have no record of their own; the commits they merge keep
theirs.

Versions
--------

Releases are tagged `v<Git version>.agit.<n>` on top of a Git release,
as Git for Windows does with `.windows.<n>`, and `git version` reports,
for example, `git version 2.56.0.agit.2`. Prebuilt releases are on the
[releases page][].

Documentation
-------------

- [Documentation/technical/git-provenance.adoc][]: the design.
- [Documentation/technical/git-provenance-format.adoc][]: the record
  and journal formats.
- [git-stamp(1)][] and [git-provenance(1)][]: the commands; with AGit
  installed, also `git help stamp` and `git help provenance`.

Everything else is Git: see Git's own [README][git-readme] and
<https://git-scm.com/> for its documentation, community and support.

Contributing and issues
-----------------------

Report problems with the provenance extension, the releases or the
installer in this repository's [issues][]. Problems with Git itself
belong upstream, as Git's [README][git-readme] describes.

License
-------

AGit, like Git, is covered by the GNU General Public License version 2;
see [COPYING][].

[INSTALL]: INSTALL
[COPYING]: COPYING
[git-stamp(1)]: Documentation/git-stamp.adoc
[git-provenance(1)]: Documentation/git-provenance.adoc
[Documentation/technical/git-provenance.adoc]: Documentation/technical/git-provenance.adoc
[Documentation/technical/git-provenance-format.adoc]: Documentation/technical/git-provenance-format.adoc
[mronus/agit-setup]: https://github.com/mronus/agit-setup
[releases page]: https://github.com/mronus/agit/releases
[issues]: https://github.com/mronus/agit/issues
[git-readme]: https://github.com/git/git/blob/master/README.md
