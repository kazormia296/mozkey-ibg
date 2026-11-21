# Code signing policy

> **Mozkey IbG note:** this document is the code signing policy of the
> upstream project, [koyasi777/mozkey](https://github.com/koyasi777/mozkey),
> carried here unchanged for reference. It does not describe Mozkey IbG.
> Mozkey IbG is a separate product with its own identity, published only from
> [kazormia296/mozkey-ibg releases](https://github.com/kazormia296/mozkey-ibg/releases).
> Its signing status is described in [README.md](README.md); it has not
> applied to SignPath Foundation, and nothing below is a commitment by or about
> Mozkey IbG.

> **Current status:** Mozkey's publicly distributed Windows MSI releases are unsigned. The project is preparing to apply for the SignPath Foundation open-source code signing program. SignPath Foundation has not approved or signed Mozkey releases as of publication of this policy.

## Project and release location

- Project: [Mozkey](https://github.com/koyasi777/mozkey)
- Source repository: [koyasi777/mozkey](https://github.com/koyasi777/mozkey)
- Official downloads: [GitHub Releases](https://github.com/koyasi777/mozkey/releases)
- Upstream project: [google/mozc](https://github.com/google/mozc)

Mozkey is an independently maintained, experimental, local-first Japanese input method based on the open-source Mozc project. It is not an official Google product and is not affiliated with Google.

## Intended signing provider and scope

If the application is approved, the intended provider attribution is:

**Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).**

This statement describes the proposed signing arrangement and **does not mean that current releases are signed**.

Subject to SignPath Foundation's approval of the project and signing scope, Mozkey intends to sign eligible **Windows MSI release installers** built from identified public source revisions through a verifiable GitHub Actions release workflow. Signing the installer does not imply that all executables and libraries included inside the MSI are individually signed. Upstream project binaries will not be submitted for individual signing under Mozkey's signing arrangement unless explicitly permitted.

For any approved signing workflow:

- The source repository, commit or release revision, build workflow, and resulting artifact will be traceable.
- Signing requests will be restricted to approved release artifacts and require explicit manual approval.
- Signed artifacts will not be modified after signing.
- Release notes will identify the signature status and published verification information.

## Project roles

Mozkey is currently a single-maintainer project. The proposed responsibilities are:

- **Authors / committers:** [koyasi777](https://github.com/koyasi777), responsible for repository changes and release preparation.
- **Reviewers:** [koyasi777](https://github.com/koyasi777), responsible for reviewing external pull requests before merging.
- **Signing approver:** [koyasi777](https://github.com/koyasi777), responsible for reviewing and explicitly approving each production signing request, **if this role arrangement is accepted by SignPath Foundation**.

Contributors with repository or SignPath signing access are required to use multi-factor authentication for the corresponding accounts. Changes affecting release workflows, dependencies, installers, and signing configuration receive particular scrutiny before signing.

## Privacy and system changes

Mozkey is designed for offline, local-first Japanese input. In normal IME operation, typed text and conversion requests are intended to remain on the user's machine rather than being transmitted to external services. The optional bundled Zenz inference path runs locally and communicates with the bundled inference runtime over the loopback interface (`127.0.0.1`).

For implementation details, security checks, and scope limitations, see [Secure Offline Guarantee](https://github.com/koyasi777/mozkey/blob/main/docs/security/offline_guarantee.md). For bundled runtime and model notices, see [Third-party notices](https://github.com/koyasi777/mozkey/blob/main/THIRD_PARTY_NOTICES.md).

The Windows installer may add outbound Windows Firewall blocking rules for installed Mozkey/Mozc runtime executables as an offline-hardening measure. Its uninstaller is designed to remove those rules. Release-specific installer behavior and privacy controls should be verified against the actual released package.

## Third-party notices

Mozkey uses open-source components and linguistic resources from multiple projects. Source information, license and attribution notices, and notes on bundled components and generated dictionary data are maintained in [Third-party notices](https://github.com/koyasi777/mozkey/blob/main/THIRD_PARTY_NOTICES.md) and related package notices. The signing process does not alter any third party's rights or applicable distribution terms.

## Release verification and incident response

Users should obtain release packages from the [official GitHub Releases page](https://github.com/koyasi777/mozkey/releases). For signed releases, release notes will include the signed MSI's SHA-256, source revision, signing status, and guidance for verifying its Authenticode signature.

Suspected release compromise, unexpected signing, and security or policy concerns will be investigated. Affected artifacts may be withdrawn and corrected releases published when necessary.

## References

- [SignPath Foundation conditions](https://signpath.org/terms.html)
- [Mozkey security and release checklist](https://github.com/koyasi777/mozkey/blob/main/docs/security/release_checklist.md)
- [Mozkey third-party notices](https://github.com/koyasi777/mozkey/blob/main/THIRD_PARTY_NOTICES.md)
