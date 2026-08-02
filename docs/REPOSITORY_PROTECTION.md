# Repository Protection Guide

## Goal

Keep the repository public for viewing, cloning, downloading, and forking while
preventing unauthorized users from changing the original repository.

Public users do not receive write access automatically. The protection model is
completed by GitHub permissions plus a protected `main` branch.

## 1. Repository Visibility

Keep the repository **Public** only after the source tree, Git history, media,
and credentials have been reviewed according to `SECURITY.md`.

Public users may:

- view files and commit history;
- download source archives;
- clone the repository;
- fork it into their own account;
- open Issues and Pull Requests.

Public users may not modify the original repository unless the owner explicitly
grants Write, Maintain, or Admin permission.

## 2. Collaborator Permissions

Open:

```text
Repository
-> Settings
-> Collaborators
```

Recommended policy:

| User type | Permission |
|---|---|
| General public | No collaborator entry required |
| Recruiter / reviewer | No collaborator entry required for a public repo |
| Trusted observer needing private access | Read |
| Active maintainer | Write only when necessary |
| Repository owner | Admin |

Remove stale collaborators, deploy keys, GitHub Apps, and machine accounts.
Never grant Admin or Maintain merely so someone can read public source.

## 3. Protect `main` With A Ruleset

Open:

```text
Repository
-> Settings
-> Rules
-> Rulesets
-> New ruleset
-> New branch ruleset
```

Use:

```text
Ruleset name: Protect main
Enforcement status: Active
Target: Default branch / main
```

Enable:

- Require a Pull Request before merging.
- Require status checks to pass.
- Add required check: `Repository policy`.
- Require conversation resolution before merging.
- Block force pushes.
- Restrict branch deletion.
- Require linear history if squash/rebase-only history is desired.

Recommended Pull Request settings for a single-owner repository:

```text
Required approvals: 0
Dismiss stale approvals: optional
Require review from Code Owners: optional with owner bypass
```

`CODEOWNERS` requests review from `@HaiHeHuoc`. GitHub cannot count the Pull
Request author's own approval. When Code Owner approval is required, either:

- keep an owner-only emergency bypass; or
- add a second trusted reviewer who can approve owner-authored Pull Requests.

Do not place external contributors or normal collaborators on the bypass list.

## 4. Actions Permissions

Open:

```text
Repository
-> Settings
-> Actions
-> General
```

Recommended configuration:

- Allow only required actions and reusable workflows.
- Default workflow permissions: **Read repository contents and packages**.
- Disable **Allow GitHub Actions to create and approve pull requests** unless a
  reviewed automation explicitly needs it.
- Review every workflow that requests `contents: write`, `pull-requests: write`,
  `actions: write`, or an OIDC token.

The repository's policy workflow uses:

```yaml
permissions:
  contents: read
```

and disables persisted checkout credentials.

## 5. Security Features

Open:

```text
Repository
-> Settings
-> Code security and analysis
```

Enable available protections:

- Secret scanning.
- Push protection.
- Dependency graph.
- Dependabot alerts.
- Private vulnerability reporting.

Treat alerts as review input. Do not automatically merge dependency changes
without checking ESP-IDF compatibility, memory impact, and hardware behavior.

## 6. Pull Request Flow

External contribution flow:

```text
public repository
    -> contributor forks
    -> contributor changes their fork
    -> contributor opens Pull Request
    -> repository-policy check runs read-only
    -> CODEOWNERS requests owner review
    -> owner reviews and decides
    -> protected main changes only after allowed merge
```

A fork cannot modify the original repository. A Pull Request is a proposal, not
an applied change.

## 7. Protect Releases And Tags

Create a tag ruleset for release tags when GitHub supports it for the account:

```text
Target pattern: v*
Restrict deletion: enabled
Restrict update: enabled
```

Only the owner or a tightly controlled release automation should create or move
release tags. Never attach `sdkconfig`, credential-bearing firmware, NVS dumps,
private logs, or service-account files to a public release.

## 8. Periodic Access Audit

Before each public release, review:

- collaborators and their permission levels;
- ruleset enforcement and bypass list;
- deploy keys and GitHub Apps;
- Actions workflow permissions;
- repository secrets and environments;
- webhooks;
- open Pull Requests from forks;
- secret-scanning and dependency alerts;
- media and release assets;
- Git history for previously exposed credentials.

## 9. Recovery

If an unauthorized or harmful change reaches `main`:

1. Disable or remove the compromised collaborator, token, App, key, or workflow.
2. Rotate affected credentials immediately.
3. Preserve evidence and identify the last trusted commit.
4. Revert through a reviewed Pull Request when possible.
5. Use force updates only as an owner-controlled emergency action.
6. Re-run secret scanning and verify release tags.
7. Document the incident without publishing active credentials.

Branch protection reduces accidental and unauthorized damage, but repository
owner account security remains critical. Use a unique password, two-factor
authentication or passkeys, and protected recovery methods for the GitHub
account.
