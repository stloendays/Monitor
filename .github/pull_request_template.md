## Scope

Describe the single coherent concern of this PR.

## Integration base

- Base branch:
- Base SHA:
- Depends on PR(s):
- I checked the current open-PR dependency graph before making shared-file changes: [ ]

## Contract impact

Check every contract touched:

- [ ] No Tier 0 contract changed
- [ ] `hub_status` / normalized snapshot
- [ ] `row_meta` / task identity
- [ ] CLI flags or machine-readable output
- [ ] Agent/Event protocol
- [ ] Monitor naming / registry identity
- [ ] Release/version/update contract
- [ ] UI normalized model

If a Tier 0 contract changed, explain backward compatibility and versioning:

## Shared integration files

List any touched shared files such as:

- `README.md`
- `hub/monitor_hub.py`
- `cpp/CMakeLists.txt`
- `cpp/src/main.cpp`
- `.github/workflows/*`

I re-read their current base HEAD immediately before the final patch: [ ]

I verified this PR does not remove unrelated behavior introduced by another active PR: [ ]

## Compatibility

- [ ] Existing/legacy input remains supported, or an explicit migration is documented
- [ ] Existing CLI flags remain supported
- [ ] Unknown additive fields/events degrade safely where applicable
- [ ] Stable project/task identity is preserved

## UI / Agent behavior

If applicable:

- [ ] Operational state/errors/required decisions remain visible
- [ ] Explanatory copy is secondary (tooltip/info/detail)
- [ ] UI consumes normalized state rather than project-specific parsing
- [ ] Child-agent authority is bounded by L1/L2/L3 rules
- [ ] Completion/escalation creates a durable notification/event

## Validation

List tests executed and results.

- [ ] Component tests
- [ ] Contract/compatibility tests
- [ ] Relevant Python/C++ parity checks
- [ ] UI smoke/behavior check if UI changed
- [ ] CI passes

## Release

- [ ] This PR does not create a release from a feature branch
- [ ] Any future release will be created only after the commit is integrated into `main`

## Remaining dependencies / known limitations

List anything still intentionally deferred.
