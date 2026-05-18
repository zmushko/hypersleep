# Changelog

All notable changes to Renatum will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial project skeleton
- Architecture documentation
- CLI specification
- Configuration format
- Recovery procedures documentation
- librnotify integration via git submodule (third_party/librnotify)
- LMDB-based index schema design
- Content-addressable storage layout
- systemd unit file with hardening
- Apache-2.0 license

### TODO before v0.1.0
- [ ] Implement `store.c` (CAS read/write, fsync)
- [ ] Implement `index.c` (LMDB sub-DBs, schema migration)
- [ ] Implement `debounce.c` (per-path event coalescing)
- [ ] Wire up `watcher.c` to actual librnotify API
- [ ] Implement `config.c` parser
- [ ] Implement `log.c` (stderr/syslog/file backends)
- [ ] Implement `cmd_status`, `cmd_log`, `cmd_show`, `cmd_diff`
- [ ] Implement `cmd_restore` with pre-snapshot safety
- [ ] Implement `cmd_verify` (re-hash all blobs)
- [ ] Stress tests in `tests/stress/`
- [ ] AT_NOFOLLOW audit on every path open
- [ ] IN_Q_OVERFLOW rescan implementation
