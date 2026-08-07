# Code signing

Release binaries are signed by [SignPath.io](https://signpath.io) with a
certificate provided by the [SignPath Foundation](https://signpath.org) for Open
Source projects. This document is the setup and maintenance side of that: what
has to exist outside the repository, and why the build is shaped the way it is.

For the policy that users and contributors are entitled to see, read
[Code signing policy](../README.md#code-signing-policy) in the README.

## What gets signed, and what does not

`XiaomiViewer.exe` and `xmbridge.dll`, and nothing else.

Both are built from this repository, which is the condition that matters: the
Foundation's terms allow a project to sign only its own binaries. The FFmpeg DLLs
that ship alongside them are upstream builds, so they travel in the release
archive unsigned. That is explicitly permitted, and signing them with this
project's certificate would not be.

The archive itself is not signed. Windows checks the signature on the executable
it is about to run, not on the zip it came out of, so the signature is applied to
the binaries before they are packaged — which is the entire reason
`scripts/package.ps1` can stage and archive as two separate steps.

## Why the build has to happen on GitHub

SignPath does not sign whatever a build script sends it. Its GitHub connector
independently checks that the artifact was produced by a GitHub Actions workflow
in the linked repository, that the origin metadata came from GitHub rather than
from the build, and — for Open Source subscriptions — that every job leading up
to the signing request ran on a GitHub-hosted runner. A signature therefore says
something specific: this binary is an automated build of this source commit.

None of that can be reproduced by signing on a developer machine, which is why
releases are built by `.github/workflows/release.yml` and not locally.

## One-time setup

### 1. Apply to the SignPath Foundation

Apply at <https://signpath.org/apply>. Before applying, check the project against
the [conditions for Open Source projects](https://signpath.org/terms) — the ones
this repository already satisfies are noted below, but the reputation and
maintenance criteria are judged by the Foundation, not by a checklist.

### 2. Install the SignPath GitHub App

Grant it access to the repository. This is what lets SignPath verify the origin
of a signing request.

### 3. Link the trusted build system

In the SignPath organization, add the predefined **GitHub.com** trusted build
system and link it to the project.

### 4. Create the artifact configuration

The artifact submitted for signing is a zip, because `actions/upload-artifact`
always produces one, so the root element is `<zip-file>`. Paste this as a custom
artifact configuration:

```xml
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <parameters>
    <parameter name="version" required="true" />
  </parameters>
  <zip-file>
    <pe-file-set product-name="Xiaomi Camera Viewer"
                 product-version="${version}"
                 file-version="${version}"
                 company-name="Xiaomi Camera Viewer contributors"
                 copyright="Copyright (c) 2026 Xiaomi Camera Viewer contributors"
                 original-filename="${file.name}">
      <include path="XiaomiViewer.exe" />
      <include path="xmbridge.dll" />
      <for-each>
        <authenticode-sign />
      </for-each>
    </pe-file-set>
  </zip-file>
</artifact-configuration>
```

The attributes on `pe-file-set` are metadata restrictions, and they are not
decoration: the Foundation requires signed binaries to carry an enforced product
name and a single product version across the release. SignPath refuses to sign
anything whose version resource disagrees with them. `${version}` is supplied by
the workflow from the project version, so a release cannot be signed under a
version other than the one it was built as.

If any of these strings change, they have to change in three places at once — here,
in the `XV_*` variables in `CMakeLists.txt`, and in the values
`scripts/check-metadata.ps1` reads back from the built binaries.

### 5. Configure the repository

Under **Settings → Secrets and variables → Actions**:

| Kind | Name | Value |
| --- | --- | --- |
| Secret | `SIGNPATH_API_TOKEN` | API token of a user with submitter permission on the signing policy |
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath organization ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | SignPath project slug |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | Signing policy slug, typically `release-signing` |
| Variable | `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` | Optional. Leave unset to use the project's default configuration |

The release workflow checks all of these before it starts building, so a missing
one fails in seconds rather than after a signing request that cannot be submitted.

## Cutting a release

1. Bump `VERSION` in the `project()` call in `CMakeLists.txt`. It is the only
   place a version is written by hand; the executable's version resource, the
   application manifest, the bridge DLL and the zip names are all derived from it.
2. Commit, then tag: `git tag v0.3.0 && git push origin v0.3.0`. The workflow
   refuses to continue if the tag and the project version disagree.
3. Approve the signing request in SignPath. Every Open Source signing request
   needs a human, and the workflow waits up to 90 minutes for one.
4. Review the draft release the workflow created, and publish it.

To exercise everything except the signature — build, tests, metadata checks,
packaging — run the workflow manually from the Actions tab. A dispatch run signs
nothing and publishes nothing.

## Verifying a signature

```powershell
Get-AuthenticodeSignature .\XiaomiViewer.exe | Format-List Status, SignerCertificate
```

The subject should name the SignPath Foundation, and `Status` should be `Valid`.
The release workflow runs the same check on the packaged binaries before it
publishes anything, via `scripts/check-metadata.ps1 -RequireSignature`.

## Conditions this repository already satisfies

Recorded so that a future change does not quietly break eligibility:

- **OSI-approved licence with no dual licensing.** MIT, see `LICENSE`.
- **No proprietary components.** Dear ImGui (MIT), nlohmann/json (MIT), FFmpeg
  (LGPL 2.1) and go2rtc-derived protocol code, all noted in
  `THIRD-PARTY-NOTICES.md`.
- **Version metadata on every signed binary.** `cmake/Version.h.in` feeds
  `src/app/app.rc`, and `bridge/versioninfo.rc.in` is compiled by `windres` into
  a `.syso` for the Go DLL, because Go emits no version resource of its own.
- **One product version across the build.** Enforced by
  `scripts/check-metadata.ps1` on every CI run, not just at release time.
- **A verifiable build.** Every dependency is pinned by content — the ImGui
  commit, and SHA-256 hashes for the FFmpeg and nlohmann/json downloads — so the
  same commit does not silently link different upstream code on a different day.
- **Manual approval for every release.** A property of the signing policy, and
  the reason the workflow's timeout is measured in hours.
