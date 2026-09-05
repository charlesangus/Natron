# Milestone 14: Documentation tree → orphan branch

Move the inherited `Documentation/` tree (~70 pages of upstream 2.4-era
Sphinx guide) and `.readthedocs.yaml` off the main development branch and
onto an orphan `docs` branch. The content is preserved for future work but
no longer clutters the working tree or misleads contributors. This resolves
the publish-or-delete question by deferring it while removing the stale
content from code branches — see
`DECISIONS/2026-09-04-docs-to-orphan-branch.md`.

The user specifically asked on 2026-09-03 that `getstarted-about-faq.rst`'s
OS-support matrix be fixed; that fix rides the docs branch before it is
parked.

## Phase 14.1: Move documentation to orphan branch

- [ ] M14.P1.T1 — Create orphan `docs` branch with the Documentation/ tree
  - files: Documentation/ (entire tree)
  - approach: Create orphan branch `docs` with no shared history
    (`git checkout --orphan docs`, remove everything, copy Documentation/
    and .readthedocs.yaml from main, commit, push). The branch preserves
    the full tree for future work. Push with `git push -u origin docs`.
  - verify: `git log docs --oneline` shows a single initial commit;
    `git ls-tree docs Documentation/` lists the expected files.
  - size: S

- [ ] M14.P1.T2 — Fix FAQ OS-support matrix on docs branch
  - files: Documentation/source/guide/getstarted-about-faq.rst
  - approach: On the `docs` branch, edit lines 22-23 and 79-91 of
    `getstarted-about-faq.rst` to remove the Windows 7/8/10 and
    "MacOSX 10.6 or greater" entries. Replace the OS-support matrix with
    a single entry: Linux x86_64 (Rocky Linux 9 / EL9 baseline). Commit
    and push. The user specifically asked for this fix.
  - verify: `grep -i 'windows\|macos\|mac os' Documentation/source/guide/getstarted-about-faq.rst`
    returns no hits on the docs branch.
  - size: S

- [ ] M14.P1.T3 — Remove Documentation/ and .readthedocs.yaml from main
  - files: Documentation/, .readthedocs.yaml, .github/workflows/nightly.yml
  - approach: On the milestone branch (off main), delete the entire
    `Documentation/` directory and `.readthedocs.yaml`. Also remove the
    `paths-ignore: - Documentation` entry from `nightly.yml:28-29` — with
    the directory gone, the entry is dead, and the pattern was wrong anyway
    (matched only a literal file, not `Documentation/**`).
  - verify: `ls Documentation/ 2>/dev/null` returns nothing;
    `ls .readthedocs.yaml 2>/dev/null` returns nothing; `grep -c
    Documentation .github/workflows/nightly.yml` returns 0.
  - size: S

**Verification gate:** `Documentation/` and `.readthedocs.yaml` are absent
from the milestone branch's tree. The orphan `docs` branch exists on the
remote with the full tree and the FAQ fix. CI passes on the milestone PR.
