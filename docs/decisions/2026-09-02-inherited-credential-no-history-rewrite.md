# The inherited Jenkins credential is deleted, not purged from history

`tools/jenkins/README.md:136` contains a plaintext password for an upstream
`natron-ci` gforge account, and L108-111 documents retrieving an SSH private
key. Both arrived with the fork; neither was ever used by this project.

**Decision:** remove them by deleting `tools/jenkins/` (M12.P2.T1). No history
rewrite.

Rationale. The credential belongs to an account this fork does not control and
cannot rotate, it predates the fork point, and it remains in
`NatronGitHub/Natron`'s public history regardless of what happens here — so
rewriting our history buys no actual secrecy. Against that, `git filter-repo`
would invalidate every SHA on `main` and on all live branches, and the plan
branch records code SHAs in its commit messages by design
(PLAN-FORMAT.md §9), so every one of those references would dangle. The cost is
real and the security gain is zero.

The remediation that would matter is upstream's to perform: the account is
theirs. Worth reporting to `NatronGitHub/Natron` separately from this cleanup.
