# Contributing to Lumora

Thanks for helping. This file covers how to build, what a change needs before it is merged,
and the licence your contribution is made under.

## Licence of contributions

Lumora is licensed under the **GNU General Public License v3.0 or later**
(`GPL-3.0-or-later`, see [LICENSE](LICENSE)). By opening a pull request you agree that your
contribution is licensed under the same terms, and you confirm that you wrote it or have the
right to submit it under them. Do not submit code copied from projects whose licence is
incompatible with GPL-3.0-or-later.

New third-party dependencies must have a GPL-3.0-compatible licence (MIT, BSD, Apache-2.0,
zlib, LGPL, ...). They must be pinned with a SHA-256 hash in the CMake files and listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

## Workflow

- `main` is protected: changes land through pull requests, never by force-pushing.
- Branch from `main` and name the branch after the change (`renderer/ibl`, `fix/hub-crash`).
- Keep commits small and focused. The subject line is `Area: what changed`, under ~72
  characters (for example `Renderer: cascaded sun shadows`). The body explains *why* and
  anything a reviewer could not see from the diff.
- Rebase on `main` before asking for review; keep history linear (no merge commits).

## Building and testing

The requirements, build commands and presets are in [README.md](README.md#build). Before
opening a pull request:

```bash
cmake --preset debug && cmake --build --preset debug
```

```bash
LUMORA_TEST_VALIDATION=1 ctest --preset debug
```

- All tests must pass, with Vulkan validation on and no validation errors in the log.
- New behaviour comes with tests: unit tests for engine code, GPU tests that render and
  check real pixels for rendering changes, UI tests for editor/Hub behaviour.
- Never fake results. GPU tests skip (they do not pass) without a Vulkan 1.4 device; do not
  turn skips into passes, and do not add placeholder implementations or benchmarks.
- Fix compiler warnings in code you touch. ASan/UBSan builds
  (`-DLUMORA_ENABLE_SANITIZERS=ON`) must stay clean.

## Code style

- C++23, 4-space indentation, lines up to ~120 characters; follow the surrounding code.
- `engine/` and `runtime/` never include Qt; the application layer is a client of the engine.
- Errors are returned as `Result<T>` (`core/Error.hpp`), not thrown, except at the Vulkan
  boundary.
- Comments say why, not what. Public functions get a short doc comment when the name is not
  enough.
- Validate Vulkan capabilities before using a feature; never silently downgrade.

## Reporting bugs and security issues

Open an issue with the steps to reproduce, the log (`~/.local/state/Lumora/logs/lumora.log`),
your GPU and driver (`lumora-editor --headless --info`), and the Lumora version.

Do not report security problems (for example in the release feed, the installer or live
sessions) in a public issue. Use GitHub's *Report a vulnerability* (private advisory) on the
repository instead.
