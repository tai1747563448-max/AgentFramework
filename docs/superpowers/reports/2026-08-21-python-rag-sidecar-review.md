# Python RAG Sidecar Review and Fix Report

Date: 2026-08-21

Branch: `feat/python-rag-sidecar`

Reviewed base: `f5c4526ecabc10a20c1cf0b112c8362d05534061`

## Scope

This report covers the independent review of the first local Python RAG
milestone and its focused fix round. The milestone remains a single-agent C++17
Runtime with an internal, fixed-command Python knowledge adapter. It adds no
model-callable Python/shell tool, no new task state, no retry/recovery policy,
and no multi-agent behavior.

## Independent review findings

The first review found no Critical issue and three Important issues:

1. ordinary supported files named `secrets.md`, `credentials.py`, and
   `token.txt` were indexed despite the documented secret-filename exclusion;
2. the C++ adapter accepted forged Python citation metadata, including an
   absolute path, negative lines, a bad hash, and a source ID unrelated to the
   metadata;
3. replay accepted a `ModelCallStarted.request.evidence` different from the
   preceding durable `ContextPrepared.evidence`.

It also found three committed Python files with one extra blank line at EOF.

## Genuine RED evidence

Tests were written before production fixes. After correcting two test-harness
mistakes (an incorrect Python test class name and a mixed C++ initializer), the
unmodified production code produced the intended behavioral failures:

- `test_secret_bearing_filenames_are_never_indexed`: `files_indexed` was 5,
  expected 1;
- `python_rag_adapter_rejects_forged_citation_metadata`: a forged item returned
  success, failing `!result.has_value()`;
- `reducer_binds_model_request_evidence_to_prepared_context`: a mismatched
  request was accepted;
- `jsonl_rejects_model_request_evidence_that_differs_from_context`: the forged
  log loaded successfully.

The harness mistakes are not counted as RED evidence.

## Fixes

### Conservative secret-filename exclusion

The indexer now rejects allowed-extension files when their filename stem has a
`.`, `-`, or `_` separated token in this fixed set:

`secret`, `secrets`, `credential`, `credentials`, `password`, `passwords`,
`passwd`, `token`, `tokens`.

The README example now indexes the trusted `docs` tree instead of the whole
repository. Documentation explicitly states that this filename policy is not a
content secret scanner and that the operator must curate the source tree.

### Python citation binding

The Python adapter now owns and validates its exact metadata schema:

- exact keys: `path`, `start_line`, `end_line`, `sha256`, `score`;
- strict UTF-8 canonical relative POSIX path with no drive, root, backslash,
  control byte, empty component, `.` component, or `..` component;
- positive ordered integer lines;
- finite nonnegative numeric score;
- exactly 64 lowercase hexadecimal SHA-256 characters, recomputed against the
  returned content;
- exact `path#Lx-Ly` source ID, or `path#Lx-Lx-Pn` with a positive part number
  for a split physical line.

The provider-neutral Domain EvidencePack validator remains open to arbitrary
bounded metadata so another future knowledge adapter is not coupled to Python's
citation schema.

### Durable request-evidence binding

`StateReducer` now revalidates `ModelCallStarted.request.evidence` and requires
exact equality with the current prepared context before marking a model call in
flight or incrementing usage. Reducer and JSONL tests cover a valid-but-
mismatched pack and an invalid duplicate request pack. Existing trace fixtures
now record the evidence that their model request actually used.

### Formatting gate

The three extra EOF blank lines were removed. Fixed-range `git diff --check`
from milestone base `1b6c8a4399e3e7a5f163a79acc0f57d6c2077085` is clean apart from Git's local
LF-to-CRLF conversion notices.

## Verification

All verification was local and credential-free. No Provider, GitHub, download,
or other network request was made.

- focused Windows adapter, Reducer, JSONL, and secret-filename tests: passed;
- Windows Python suite: 15 tests passed with one privilege-based symlink skip;
- WSL Python suite: 15/15 passed, including the symlink test;
- WSL Ubuntu g++13 with C++17 and `-Wall -Wextra -Wpedantic -Werror`: adapter
  compiled and 8/8 tests passed, including link regressions and split-line
  citation acceptance;
- existing MSVC Debug build: all targets built;
- existing offline CTest: 19/19 passed;
- completely new offline MSVC Debug configure/build directory: succeeded;
- new-directory offline CTest: 19/19 passed;
- credential-free `run`: exit 2 with exact `AGENT_BASE_URL is required`;
- credential-free `verify-log`: exit 0 and reported validated task
  `task-0000000000000000000000000000000b`, `Completed`, sequence 6;
- `ctest -N`: exactly 19 offline tests; cache records
  `AGENT_ENABLE_LIVE_TESTS=OFF`.

The new CMake configure emitted existing third-party developer/deprecation and
missing-optional-package warnings. It used explicitly supplied local
FetchContent source directories with `FETCHCONTENT_FULLY_DISCONNECTED=ON` and
completed without fetching.

## Remaining evidence limit

The Windows token cannot create a file symlink, so Windows link tests report the
explicit privilege-based skip. The same Python and C++ adapter link paths run
and pass under WSL. This is an environment evidence limit, not a claim that the
Windows symlink branch executed.

## Review gate

The independent fix review closed all three Important findings and the
formatting Minor, with no new Critical or Important issue and a Ready verdict.
It identified one test-isolation Minor: several aggregate adapter bounds could
fail first on an unrelated citation mismatch. The follow-up changed those
fixtures so top-k, duplicate, empty/oversized item, 513-byte source ID, invalid
UTF-8, and total-content cases each have an otherwise valid citation. A second
independent review found no remaining Critical, Important, or Minor issue and
returned Ready: Yes.
