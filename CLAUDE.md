# CLAUDE.md — working conventions

Notes for Claude (and humans) editing this repo. Read [HANDOFF.md](HANDOFF.md)
first for context and the current goal.

## Scope
- This is an **ESPHome external component** — the *device half* of a LoRaWAN
  system. It joins a network and uplinks sensor values.
- It does **not** provision network servers, register devices, mint keys, or
  flush DevNonces. That is server-side and lives in the consuming orchestrator.
  Keys arrive as config (`!secret`). If a change reaches toward a network
  server's management API, it is in the wrong repo.

## Voice
- Concise. No marketing language, no emojis in code, comments, commits, PRs.
- State decisions and tradeoffs directly. Don't hedge.

## Code
- Default to no comments. Add one only when the *why* is non-obvious — a
  hidden constraint, a RadioLib version quirk, a timing invariant. The blocking
  `loop()` and the RX-window timing are exactly the kind of thing to comment.
- No backwards-compat shims or dead-code stubs. Delete it.
- No premature abstraction. Support the chip in front of you (SX1276) before
  generalizing the radio layer.
- Validate config at the schema boundary (`__init__.py`); trust it in C++.

## Dependencies
- Reuse robust upstreams: **RadioLib** for the radio + MAC, **ESPHome** for
  SPI / sensors / preferences. Prefer upstream `jgromes/RadioLib` via
  `lib_deps`; only fall back to an esphome-compile fork if upstream won't build.
- **Pin** the RadioLib version and build to its exact API.

## Discipline
- Spike-first: the gate is a real OTAA join that survives a power cycle (see
  HANDOFF.md). Don't polish config, add regions, or refactor the radio layer
  until the component compiles and joins.
- Never commit `secrets.yaml` or real keys. The example uses `!secret`.
- Re-flash the **app region** on a known device to preserve NVS-stored nonces;
  a full erase resets them (HANDOFF.md, findings #1–2).
