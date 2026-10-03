# Temporary CI security containment

The container-based `build.yml` matrix and label-triggered Guix PR builds are
**temporarily disabled for all pull requests**, including same-repository PRs.
A `guix-build` label does not authorize executing a PR head with a publishing
token. Feature-branch pushes no longer run those publisher-backed matrices.
PR formatting/metadata checks remain; `ci-trust-policy.yml` adds a read-only
policy check. This is **not** replacement compiler/unit/functional/Guix coverage.
Branch protection requiring the old PR build jobs must be reviewed by a repo
administrator; this change does not silently make skipped builds successful.

## Trusted paths retained

- `build.yml`: pushes to `main` and `v*` tags retain the existing build/lint/test
  matrix (subject to the existing `SKIP_*` variables).
- Guix: `v*` tag pushes and the weekly schedule on `main` remain; a `main` push
  runs Guix when `RUN_GUIX_ON_ALL_PUSH == 'true'`. That variable no longer admits
  arbitrary feature branches. Guix's privileged Docker execution remains only
  on this trusted path, with a read-only token.
- Windows: existing `v*` pushes and manual builds remain read-only. They upload
  workflow artifacts, not releases or packages.
- Publication trust assumes maintainers protect `main` and release-tag creation.
  These workflows cannot establish that a tag's commit has been reviewed.

## Permission and source boundary

All workflows default to `contents: read`. Package writes are granted only to
container-publisher jobs and their necessary reusable-workflow call sites.
Publisher callees independently reject PR events and non-release branches.
No source/dependency/test/lint job inherits package-write permission. Checkouts
use `persist-credentials: false`; build callees select the event SHA, not a
PR-head override. Reusable workflow permissions may only be reduced downstream.

Guix provenance is generated in a separate job that downloads same-run artifacts
and never checks out or executes repository code; only that job gets
`id-token: write` and `attestations: write`. Metadata jobs retain narrowly scoped
PR-comment/label writes. Conflict prediction explicitly checks out the default
branch; merge checks treat PR refs as quoted data. Both metadata workflows no
longer trigger on review submission (a PR-associated workflow ref); they retain
`pull_request_target` updates, and merge checks also retain push updates.

Multi-architecture CI manifests consume the exact per-architecture build digests.
The manifest output comes from Buildx's creation metadata and is validated and
read back by digest before being handed to all downstream consumers. Guix also
uses the builder digest, not a mutable tag. Cache/compatibility tags still exist,
but are not authoritative inputs to these jobs. Dockerfile base images and
third-party action version tags are not newly digest/SHA-pinned by this scope.

## Docker Hub

The inherited release workflow targeted `dashpay/dashd`. Its release trigger,
login, secrets, and publishing steps have been removed. A manually dispatchable,
unconditionally skipped placeholder documents the hold. No Smartiecoin
replacement destination was invented; enabling Docker Hub publication requires
an explicit destination/credential review.

## Re-enabling PR build coverage

Use `pull_request` with a read-only token, no repository secrets, isolated caches,
and a trusted immutable builder image. Do not build/publish a PR Dockerfile in a
write-enabled job, restore PR-generated executable state in a publisher, or
promote PR artifacts to release assets without a separate trust review. Do not
restore `pull_request_target` head checkout or label-based publishing bypasses.

Run the scoped policy tests with Python 3 and PyYAML 6.0.3:

```sh
python3 test/lint/test_ci_trust_policy.py -v
```

The dedicated PR workflow installs that parser explicitly. These structural
regressions do not replace actionlint, shell validation, or real hosted builds.
