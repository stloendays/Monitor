# AGENTS.md — Monitor Hub repository rules

This file is mandatory for every human or AI agent that changes this repository.

## Product north star

Monitor Hub is a user-facing operations console for long-running monitored work. Users should be able to see:

- what projects and tasks are running;
- how far each task has progressed;
- what problem was detected;
- what a child agent did to investigate or repair it;
- whether the task resumed successfully;
- what final artifacts were produced;
- when the main agent or user must make a decision.

The hub is not the primary project agent and is not just a log viewer. It observes, normalizes state, coordinates bounded recovery, records evidence, and reports outcomes.

Read `docs/PRODUCT_VISION.md` before changing user-facing behavior.

## Mandatory pre-change procedure

Before editing anything:

1. Read this file.
2. Read `docs/DEVELOPMENT_CONTRACT.md`.
3. If changing agent integration, read `docs/AGENT_EVENT_PROTOCOL.md`.
4. Read the component-specific docs relevant to the change.
5. Inspect the live repository topology:
   - current `main` HEAD;
   - open pull requests and their base/head branches;
   - the newest dependency branch containing the capabilities your work depends on.
6. Re-read the current HEAD version of every shared file you intend to modify.

Never assume that `main` is the newest development baseline. This repository may use stacked PRs.

## Integration-baseline rule

Choose the integration base from the live PR dependency graph, not from memory.

- Independent work may branch from `main`.
- Work that depends on an open feature must branch from that feature's current head and target that branch with a stacked PR.
- If the dependency chain advances while you are working, compare the new head before opening or updating your PR.
- Do not write into another agent's branch.
- Do not solve divergence by deleting or overwriting the other feature.

A PR must state its base branch and the base SHA it was developed against.

## Stable contracts

Treat these as public contracts:

- `hub/hub_status.schema.json` and normalized snapshot semantics;
- `table.row_meta`, especially stable `task_id`;
- project/task identity and monitor naming conventions;
- CLI flags and machine-readable CLI output;
- agent/event/command envelopes in `docs/AGENT_EVENT_PROTOCOL.md`;
- release `VERSION`, manifest, asset naming and update behavior;
- the normalized data model consumed by UI implementations.

Default rule: evolve contracts additively and backward-compatibly.

A breaking change requires all of the following:

1. a new explicit contract/schema version;
2. a compatibility adapter or migration path;
3. tests covering the old and new forms;
4. documentation of the transition;
5. an integration plan for all known consumers.

Do not silently rename, remove, reinterpret, or repurpose an existing field or event.

## Shared integration files

These files are high-conflict integration surfaces:

- `README.md`;
- `hub/monitor_hub.py`;
- `cpp/CMakeLists.txt`;
- `cpp/src/main.cpp`;
- `.github/workflows/*`;
- status schemas and protocol docs;
- release/version files.

Rules for shared files:

- fetch/read the current branch HEAD immediately before editing;
- make the smallest patch that preserves unrelated behavior;
- never replace the whole file from stale chat context, an old branch, or a cached copy;
- preserve newly added flags, targets, tests, adapters and workflow steps;
- if two active PRs touch the same shared file, explicitly reconcile both deltas before merge.

## Component boundaries

Keep implementation concerns separated:

- monitor adapters collect/normalize state;
- the C++/Python core evaluates normalized state and health;
- UI renders normalized state and emits user intents;
- UI must not contain project-specific parsers or recovery policy;
- agent gateway dispatches bounded work to child agents;
- child agents do not become the main project agent;
- updater/release code must not mutate a Git development checkout;
- transport mechanisms must not redefine event semantics.

A new UI (Qt, web, etc.) should consume the same normalized contracts rather than inventing a parallel data model.

## UI rules

User-visible operational information must remain visible:

- status;
- progress;
- active problem;
- current recovery action;
- whether user/main-agent action is required.

Explanatory/help copy belongs in tooltips, info controls, or secondary detail panels.

Never hide an error, warning, required decision, failed recovery, or blocked state only inside hover text.

Prefer progressive disclosure:

`Project → Task → Issue → Agent run → raw evidence`.

Raw logs are evidence and diagnostics, not the primary user experience.

## Agent authority

Use the three-level authority model from `docs/AGENT_EVENT_PROTOCOL.md`:

- L1: deterministic, pre-authorized mechanical action;
- L2: child-agent action bounded by an explicit project policy;
- L3: main-agent/user decision required.

A child agent must not expand scope because it found an interesting adjacent task.

On completion or required escalation, Monitor Hub must create a durable notification/event for the main agent; UI color changes alone are not sufficient.

## Branch, PR and release rules

- No direct feature development on `main`.
- One coherent concern per branch/PR.
- Use explicit stacked PRs when one feature depends on another.
- Keep contract changes separate from large implementation rewrites when practical.
- Do not create a release from a feature branch.
- Stable releases are created only from commits already integrated into `main`.
- Never force-push another agent's branch without explicit coordination.

## Definition of done

A change is not done until:

- its intended behavior works;
- relevant existing behavior still works;
- compatibility tests cover any contract it touches;
- documentation is updated;
- shared files were reconciled against their latest base;
- the PR diff contains no accidental rollback of another feature;
- required CI passes;
- the PR clearly states any remaining dependency or migration step.

When in doubt, preserve compatibility and surface the uncertainty in the PR rather than guessing.
