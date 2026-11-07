# Third-party notices

The executables include the Go runtime and the modules listed in [dependencies.json](dependencies.json). Corresponding license notices are preserved in `licenses/`; test/build-only modules may also be listed. Go runtime license: `licenses/Go_LICENSE`.

The embedded management console includes Vue, Vue Router, Element Plus and their dependencies. The inventory is in [frontend-dependencies.json](frontend-dependencies.json); preserved frontend notices are in `internal/platform/assets/ui/third-party-licenses.txt` and served locally at `/ui/third-party-licenses.txt`. Rebuild notices with `frontend/scripts/collect-licenses.mjs` when the frontend dependency lock changes.
