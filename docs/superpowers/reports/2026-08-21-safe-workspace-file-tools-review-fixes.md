# Safe workspace file tools: review-fix report

Date: 2026-08-21

Branch: `feat/safe-workspace-tools`

Reviewed milestone base: `810df98e94506e578672801766f4dbcabb7dbc5c`

## Accepted findings and fixes

1. Malformed UTF-8 in a tool argument value or object key could throw while
   serializing the complete argument tree. The gateway now validates and counts
   the compact JSON representation incrementally, before route decoding, and
   returns the bounded model-visible `invalid_arguments` result. The exact
   2 MiB serialized boundary is locked by tests.
2. Recursive list/search collected every child in a large directory before
   enforcing the 2,000-entry traversal budget. Both operations now consume the
   shared entry budget while advancing an error-code directory iterator and
   retain the first truncation reason.
3. Large but valid list/read output could be converted into a tool error after
   the operation succeeded. List now removes trailing sorted entries and read
   removes only complete trailing lines until the serialized JSON fits 64 KiB;
   an individual line that cannot fit still returns `limit_exceeded`.
4. A linked workspace root could be canonicalized before the root itself was
   checked, and an in-workspace configured runtime root could be expressed
   through a different filesystem alias. Root resolution now rejects a supplied
   root whose normalized absolute path differs from its physical canonical
   path, and runtime protection compares canonical physical paths.
5. The schema contract test now compares all five ordered definitions exactly,
   including descriptions, property names, required arrays, integer bounds,
   `additionalProperties`, and the write mode enum.

## TDD evidence

- Malformed UTF-8 genuine RED: the gateway returned an outer Runtime error
  instead of a model-visible `ToolResult`; the new value/key cases pass after
  incremental validation.
- Traversal genuine RED: recursive list of 2,001 children reported the later
  result limit instead of stopping at the entry budget; it now reports
  `entry_budget`.
- Serialized-output genuine RED: the large Unicode list and escaped-control
  multi-line read returned `is_error=true`; both now return bounded successful
  JSON with explicit continuation metadata.
- The root-link regression could not produce a dynamic RED on this Windows
  host because creating a directory symlink was denied. The test prints an
  explicit `SKIP`, and the static path-order/canonical-alias issue was fixed.
- Exact-schema strengthening is a contract test improvement, not a production
  behavior fix; it passed against the intended definitions.

## Verification

- Focused executables: `workspace_primitives_tests`,
  `workspace_tool_gateway_tests`, and `runtime_integration_tests` passed.
- Clean-first MSVC Debug build completed successfully.
- Offline CTest: 11/11 passed; no live/provider test was registered or run.
- Credential-free `run`: exit 2 with exactly `AGENT_BASE_URL is required`.
- Credential-free `verify-log`: exit 0, task status `Completed`, sequence 6.
- `git diff --check` passed. No provider credential or live network request was
  used, and GitHub was not accessed.

## Evidence limitation

The current Windows account cannot create file or directory symlinks, so the
three link-creation regression branches report explicit skips. Ordinary path,
hard-link, canonical-path, protected-root, integration, and replay tests pass;
this report does not claim executed symlink coverage on this host.

## Re-review fix round 1

The scoped re-review of `63e6643c` found one remaining Important input case:
non-finite `double` values still reached JSON conversion and escaped as an
outer Runtime failure. A focused regression produced a genuine RED at
`invalid_double.has_value()`. The gateway now rejects `NaN`, positive infinity,
and negative infinity before conversion; all return a model-visible
`invalid_arguments` tool result.

All re-review Minors were also closed:

- list/search result shrinking retains an existing scan/result truncation
  reason and uses `output_bytes` only when no earlier reason exists;
- a combined `max_results` plus output-size regression locks that precedence;
- 64/65-level and 10,000/10,001-node argument boundaries are tested exactly;
- report trailing whitespace was removed and the commit-range diff check is
  rerun during final verification.
