#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Scoped CI containment regressions; requires PyYAML (see ci-trust-policy.yml).

Run: python3 test/lint/test_ci_trust_policy.py -v
This checks effective YAML, not comments. It is not a general Actions auditor.
"""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github/workflows"


def load(name):
    # BaseLoader retains Actions' `on` key and boolean/expression spellings.
    return yaml.load((WORKFLOWS / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)


class CITrustPolicyTest(unittest.TestCase):
    def test_build_entrypoints_do_not_accept_pull_requests(self):
        for name in ("build.yml", "guix-build.yml"):
            with self.subTest(workflow=name):
                events = load(name)["on"]
                self.assertNotIn("pull_request_target", events)
                self.assertNotIn("pull_request", events)
                self.assertEqual(events["push"]["branches"], ["main"])
                self.assertEqual(events["push"]["tags"], ["v*"])

    def test_read_only_defaults_and_explicit_write_allowlist(self):
        allowed = {
            ("build.yml", "container"): {"packages"},
            ("build.yml", "container-slim"): {"packages"},
            ("build-container.yml", "build-amd64"): {"packages"},
            ("build-container.yml", "build-arm64"): {"packages"},
            ("build-container.yml", "create-manifest"): {"packages"},
            ("guix-build.yml", "build-image"): {"packages"},
            ("guix-build.yml", "attest"): {"id-token", "attestations"},
            ("label-merge-conflicts.yml", "main"): {"pull-requests"},
            ("merge-check.yml", "check_merge"): {"pull-requests"},
            ("predict-conflicts.yml", "predict_conflicts"): {"pull-requests"},
        }
        for path in sorted(WORKFLOWS.glob("*.yml")):
            workflow = load(path.name)
            with self.subTest(workflow=path.name):
                self.assertEqual(workflow.get("permissions"), {"contents": "read"})
                for job_id, job in workflow["jobs"].items():
                    permissions = job.get("permissions", workflow["permissions"])
                    self.assertIsInstance(permissions, dict)
                    writes = {key for key, value in permissions.items() if value == "write"}
                    self.assertEqual(writes, allowed.get((path.name, job_id), set()))
                    if "packages" in writes and "uses" not in job:
                        condition = " ".join(job.get("if", "").split())
                        if path.name == "guix-build.yml":
                            expected = (
                                "(github.event_name == 'push' && (startsWith(github.ref, 'refs/tags/v') || "
                                "(github.ref == 'refs/heads/main' && vars.RUN_GUIX_ON_ALL_PUSH == 'true'))) || "
                                "(github.event_name == 'schedule' && github.ref == 'refs/heads/main')"
                            )
                        else:
                            expected = (
                                "github.event_name == 'push' && (github.ref == 'refs/heads/main' || "
                                "startsWith(github.ref, 'refs/tags/v'))"
                            )
                        # Fail closed if someone appends an OR-label or PR bypass.
                        self.assertEqual(condition, expected)

    def test_checkouts_never_persist_credentials(self):
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for job in load(path.name)["jobs"].values():
                for step in job.get("steps", []):
                    if step.get("uses", "").startswith("actions/checkout@"):
                        with self.subTest(workflow=path.name, step=step.get("name")):
                            self.assertEqual(step.get("with", {}).get("persist-credentials"), "false")

    def test_privileged_metadata_uses_trusted_source(self):
        workflow = load("predict-conflicts.yml")
        for name in ("predict-conflicts.yml", "merge-check.yml"):
            self.assertNotIn("pull_request_review", load(name)["on"])
        checkout = next(step for step in workflow["jobs"]["predict_conflicts"]["steps"]
                        if step.get("uses", "").startswith("actions/checkout@"))
        self.assertEqual(checkout["with"]["ref"], "${{ github.event.repository.default_branch }}")
        # Build callees never select a PR head, even if a new caller is added.
        for name in ("build-container.yml", "build-depends.yml", "build-src.yml",
                     "cache-depends-sources.yml", "lint.yml", "guix-build.yml"):
            for job in load(name)["jobs"].values():
                for step in job.get("steps", []):
                    if step.get("uses", "").startswith("actions/checkout@"):
                        self.assertNotIn("pull_request.head", step.get("with", {}).get("ref", ""))

    def test_consumers_use_exact_published_image_digests(self):
        workflow = load("build-container.yml")
        self.assertEqual(workflow["on"]["workflow_call"]["outputs"]["path"]["value"],
                         "${{ jobs.create-manifest.outputs.path }}")
        manifest = workflow["jobs"]["create-manifest"]
        self.assertEqual(manifest["outputs"]["path"], "${{ steps.manifest.outputs.path }}")
        script = next(step["run"] for step in manifest["steps"] if step.get("id") == "manifest")
        self.assertIn('"${REPO}@${AMD64_DIGEST}"', script)
        self.assertIn('"${REPO}@${ARM64_DIGEST}"', script)
        self.assertIn('--metadata-file "$RUNNER_TEMP/manifest.json"', script)
        self.assertIn('"path=${REPO}@${DIGEST}"', script)
        self.assertIn('[[ "$DIGEST" =~ ^sha256:[0-9a-f]{64}$ ]]', script)
        guix = load("guix-build.yml")
        self.assertIn("@${{ steps.build.outputs.digest }}", guix["jobs"]["build-image"]["outputs"]["image-path"])
        build = guix["jobs"]["build"]
        script = next(step["run"] for step in build["steps"] if step.get("name") == "Run Guix build")
        self.assertIn("${{ needs.build-image.outputs.image-path }}", script)
        self.assertNotIn("dashcore-guix-builder:", script)

    def test_legacy_dockerhub_publication_is_inert(self):
        workflow = load("release_docker_hub.yml")
        self.assertEqual(set(workflow["on"]), {"workflow_dispatch"})
        self.assertEqual(workflow["jobs"]["release"].get("if"), "${{ false }}")
        text = (WORKFLOWS / "release_docker_hub.yml").read_text(encoding="utf-8")
        self.assertNotIn("secrets.", text)
        self.assertNotIn("docker/login-action", text)
        self.assertNotIn("docker/build-push-action", text)


if __name__ == "__main__":
    unittest.main()
