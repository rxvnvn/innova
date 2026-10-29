# Moved files

| old path | new path | reason | reference updates | commit |
|---|---|---|---|---|
| `README` | `doc/project-status.md` | the extension-less `README` was a 2026-10-25 project-status note that shadowed `README.md`; its first line was literally `README.md` | no build/packaging reference existed (`grep` over `*.sh`, `*.nsi`, `*.yml`, `*.pro`); `share/setup.nsi` ships `doc/README_windows.txt`, not this file | 2a7cce14 |
| `DEPLOYMENT_READINESS_3f05a82e.md` | `doc/reports/deployment-readiness-3f05a82e.md` | a one-off deployment-readiness report did not belong in the repository root | no references found | 2a7cce14 |

## Known consequence

`PROJECT_CANONICAL.md` (owner-managed, not tracked in this repository) mentions `README` in its
documentation-reconciliation notes. That governance note now points at a path that no longer exists; the
content lives in `doc/project-status.md`. The governance document was not edited by this pass.
