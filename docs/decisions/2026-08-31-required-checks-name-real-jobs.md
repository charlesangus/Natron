# Required status checks name the real jobs

Branch protection on `main` will require the actual CI job names (`format`,
`lint-ci`, and the build/test job(s)), not a stand-in. The `ci` aggregator job
— whose entire body is `if [ "$result" != "success" ]; then exit 1; fi` — is
deleted in M10.

M8 introduced that aggregator after a job rename broke branch protection, on
the reasoning that a required check should be a name nobody has cause to touch.
The premise does not hold here: this is our fork and our branch-protection
settings, and updating the required-checks list is a one-line configuration
change we are entitled to make deliberately. Paying for it with a permanent
extra job, an extra runner start per PR, and a layer of indirection between
"what failed" and "what the gate reports" is the wrong trade.

The obligation this creates is explicit and accepted: **renaming a CI job means
updating `main`'s required status checks in the same change.** Jobs are named
for what they do, not for incidental detail like a Python version — which is
what made the original name (`Test Ubuntu Python 3.10`) rot in the first place.

Supersedes the "gates on a stable aggregator check" half of
`2026-08-30-ci-reuses-local-scripts.md`. The other half of that decision — CI
invoking `tools/ci/local/*.sh` so developers and CI run the same code — stands
and is preserved by the M10 rewrite.

**As landed (2026-08-31).** `main`'s required status checks are exactly
`format`, `lint-ci`, and `build-and-test`. The list went straight from `["ci"]`
to those three in a single API call at the moment the rewrite was pushed, so
there was never a window in which `main` had no required check — the
"relax now, re-arm later" sequencing originally planned proved unnecessary,
because branch protection evaluates the checks a pull request actually
reports rather than the workflows present on `main`. `CONTRIBUTING.md` names
the three and repeats the renaming obligation, so it is stated where a
contributor will meet it rather than only here.

