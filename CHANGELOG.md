# Changelog

This project follows semantic versioning. Stable releases are tagged as
`vMAJOR.MINOR.PATCH`; unfinished work is kept on an explicitly named
experimental branch and is not used by the stable installer.

## 0.15.0 - 2026-10-21

### Added

- Guarded deletion of selected Creo model-tree features.
- Safe resizing of existing sheet-metal planar-wall features.

### Changed

- Preserve the model that is already active when the first MCP command binds
  to a newly opened Creo session.
- Retain the current V15 stable resident bridge and its rollback-compatible
  installation path.

## 0.14.0 - 2026-10-20

### Added

- Portable V14 Creo workflows, installation helpers, company standards, and
  migration documentation.
