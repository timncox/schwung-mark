---
status: active
last_touched: 2026-07-18
deploy: scripts/deploy.sh
---

# Mark

Five-track Schwung Shadow UI live looper for Ableton Move, distributed as the
`mark` overtake module. The native engine is in `src/mark_core.c`; the Move UI
and browser mixer are in `src/ui_overtake.js` and `src/web_ui.html`.

## Work safely

- Run `make test` after engine, session, state, or UI-protocol changes.
- Run the native simulator under AddressSanitizer and UBSan before release.
- Keep the render path non-allocating; session file I/O belongs on its worker.
- Session failures must preserve the current in-memory performance.
- `make arm` stages and cross-compiles the module archive with Docker.
- `scripts/deploy.sh` writes to Move hardware. Do not run it without explicit
  deployment authorization.

See `CLAUDE.md` for the current engine model, UI conventions, session layout,
and hardware-specific constraints.
