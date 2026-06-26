# Contributing to RPM

Thank you for your interest in contributing to RPM, the RISC-V Performance
Model. This document describes how to get set up, the conventions we follow, and
the process for submitting changes.

By contributing to this project, you agree that your contributions will be
licensed under the [Apache License, Version 2.0](LICENSE).

## Code of Conduct

This project and everyone participating in it is governed by our
[Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to
uphold this code.

## Getting Started

Clone the repository and its submodules, then build:

```bash
git clone <repository-url> rpm
cd rpm
git submodule update --init --recursive
bash scripts/build_scripts/build_all.sh
```

See the [README](README.md) for full build, configuration, and simulation
instructions, and for the directory layout.

## Development Workflow

1. Create a topic branch off `main` for your change.
2. Make your change, following the coding conventions below.
3. Build and run the relevant tests (see the **Tests** section of the README).
4. Open a pull request against `main` with a clear description of the change and
   its motivation.

## Coding Conventions

### Formatting and linting

C++ code is formatted with `clang-format` and linted with `clang-tidy`. The
configurations live in [`.clang-format`](.clang-format) and
[`.clang-tidy`](.clang-tidy) at the repository root. Please run the formatter
before submitting:

```bash
bash ci/check_format.sh
```

### License headers

Every source file must carry an SPDX license header. Use the form appropriate
to the file's comment syntax.

For C++ source and header files (`.cpp`, `.hpp`, `.h`):

```cpp
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.
```

For Python, shell, CMake, and YAML files:

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.
```

For shell and Python scripts that begin with a `#!` shebang line, place the
header immediately after the shebang. Keep the year current for new files; do
not edit the year on files you only modify.

## Submitting Changes

* Keep pull requests focused; unrelated changes belong in separate PRs.
* Write clear commit messages that explain *why* a change is made.
* Ensure the build passes and tests are green before requesting review.
* Be responsive to review feedback.

## Reporting Issues

* For functional bugs and feature requests, open a GitHub issue with enough
  detail to reproduce or understand the request.
* For security vulnerabilities, **do not** open a public issue — follow the
  process in [SECURITY.md](SECURITY.md).

Thank you for contributing!
