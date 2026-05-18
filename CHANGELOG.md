# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- REST mock server with route matching, when/then_return declarative API,
  typed constants (method, status, content_type), and request history
- SOAP mock server (1.1/1.2) with action matching, when/then_return,
  and built-in fault factory
- FTP mock server (RFC 959) with user authentication, permission control,
  virtual filesystem, and session tracking
- MQTT 3.1.1 mock broker with topic subscription, wildcard matching
  (single-level +, multi-level #), and message history
- SFTP v3 mock server (via libssh) with user authentication, permission
  control, and activity logging
- Standalone examples for each server
- Conan 2.x packaging and CMake integration
- GitHub Actions CI (Linux x86_64 + Windows i686)
- GitHub Actions release workflow with tar.gz/zip artifacts
- Top-level Makefile with build, test, examples, install, release, clean targets
- Shared infrastructure: TempDir, platform socket abstraction, thread group,
  path utilities
- RAII wrappers for SFTP resources (sftp_attributes, FILE*, DIR*)
- Consistent PIMPL lifecycle across all servers (start/stop/base_url, RAII,
  non-copyable)

### Changed

- All server implementations use self-documenting naming throughout
- PIMPL renamed from `impl` to `implementation`
- FTP and MQTT thread lifecycle consolidated into shared `thread_group`
- SFTP server split: handler functions extracted to `sftp_handlers.cpp`
- Deprecated `ssh_pki_generate` replaced with `ssh_pki_generate_key`

### Removed

- Dead `routes_` vector in rest_server implementation
- All comments from production code and tests
- Duplicated TempDir, socket helpers, and path utilities

### Fixed

- FTP thread race condition (dangling `this` in client_loop)
- `sys_stat` compatibility issue on Linux builds
