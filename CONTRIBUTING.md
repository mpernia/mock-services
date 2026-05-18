# Contributing to mock-services

First off, thanks for taking the time to contribute!

## Code of Conduct

This project and everyone participating in it is governed by the [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

## How to contribute

### Report bugs

Open an [issue](https://github.com/mpernia/mock-services/issues/new?template=bug_report.md) with:

- A clear title and description
- Steps to reproduce
- Expected vs actual behavior
- Environment (OS, compiler, Conan version)

### Suggest features

Open an [issue](https://github.com/mpernia/mock-services/issues/new?template=feature_request.md) describing:

- What you're trying to achieve
- Why the current API doesn't cover it
- A sketch of the proposed API (if you have one)

### Submit code

1. Fork the repository
2. Create a feature branch (`git checkout -b feat/my-change`)
3. Commit your changes following [Conventional Commits](https://www.conventionalcommits.org/)
4. Push and open a Pull Request

## Development setup

```bash
# Clone
git clone https://github.com/mpernia/mock-services.git
cd mock-services

# Install dependencies and build
conan install . --build=missing -c tools.cmake.cmaketoolchain:generator="Unix Makefiles"
cmake --build --preset conan-release

# Run tests
ctest --preset conan-release --output-on-failure

# Or use the Makefile
make
make test
```

## Coding standards

- **C++11** — the library targets C++11 for maximum compatibility
- **No comments** — code should speak for itself. Names carry intent.
- **No cryptic abbreviations** — single-letter variables and shortened names are not accepted
- **Immutability by default** — prefer `const` unless mutation is required
- **RAII** — resources are owned by scoped objects, not manually managed
- **Typed constants** — use `enum class` and typed namespaces (like `method::get`, `status::ok`) instead of raw values
- **English** — code, identifiers, commits, and documentation in English

## Pull Request checklist

Before submitting:

- [ ] Build passes (`make build`)
- [ ] Tests pass (`make test`)
- [ ] New behavior has tests
- [ ] Commit messages follow Conventional Commits
- [ ] No commented-out code
- [ ] No cryptic variable names

## Commit style

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: add user authentication to FTP server
       ^-- scope can be omitted for global changes

fix(mqtt): handle empty topic filter

docs: update README with new API

refactor: extract TempDir to shared utility

test: add edge case for SFTP permission denied

chore: update CI workflow
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `style`.

## Questions?

Open a [Discussion](https://github.com/mpernia/mock-services/discussions) or ask in the issue tracker.

Thank you for contributing!
