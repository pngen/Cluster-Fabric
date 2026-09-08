# Contributing to Cluster Fabric

Cluster Fabric is an accelerator-infrastructure component with a deliberately narrow
contract. Contributions are welcome when they respect that contract.

## Ground rules

1. **UNKNOWN is first-class.** Never let absence of evidence become a positive answer.
   Never let a default-constructed value silently mean "fine".
2. **Hard validity before ranking.** Validate identity, authority, generation, epoch,
   references, and lifecycle before any comparison, ordering, or aggregation.
3. **Every mutation is transactional.** Validate, apply under an undo journal, verify
   invariants, persist atomically, then publish. A refusal must leave the cluster exactly
   as it was.
4. **No hidden state.** Every mutation carries identity, authority, generation, epoch,
   lifecycle, provenance, and freshness. If a field is unknown, it says so.
5. **Respect the boundary.** Do not add scheduling, placement decisions, fabric drivers,
   capacity brokering, reservation, or cross-cluster state to this repository. See
   `docs/boundaries.md`.

## Building and testing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Debug and sanitizer builds are expected to pass as well:

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build-asan -G Ninja -DCLUSTER_FABRIC_ENABLE_SANITIZERS=ON
```

## Test rules

* No timeouts. A test either reaches its condition or fails and reports the condition it
  did not reach. There is no `CTest` `TIMEOUT`, no shell timeout wrapper, and no watchdog
  that converts a hang into a pass.
* No `abort()` or assertion dialogs. Tests report and return.
* Real behavior is preferred over mocks: real OS processes, real sockets, real files.
* Every randomized test is seeded, and the seed is printed on failure.
* New behavior needs a test that would fail without the change.

## Style

* C++20, namespace `cluster_fabric`, four-space indent, 100-column limit.
* `[[nodiscard]]` on value-returning queries; `noexcept` where honest.
* First-party warnings are errors (`/W4 /WX`). Do not globally suppress warnings.
* No third-party dependencies in the product or the test suite.
* Public headers are the contract: changing one requires updating its documentation and
  the changelog.

## Commits

* One coherent change per commit, imperative subject line, body explaining the *why*.
* No AI attribution and no `Co-authored-by` trailers.
* Keep the tree clean: no build outputs, no temporary files, no editor droppings.

## Reporting a vulnerability

See `SECURITY.md`. Do not open a public issue for a security defect.
