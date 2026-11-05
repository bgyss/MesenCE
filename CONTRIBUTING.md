# Contributing to this fork

This repository is an independent fork of [MesenCE](https://github.com/nesdev-org/MesenCE). It's
run as its own project and an open experiment: what happens when an emulator is developed with LLM
assistance, backed by automated testing and formal verification. Changes are made and merged here
directly. Nothing here is prepared for, or obliged to go, upstream.

## Licensing

The emulator is licensed under the **GNU GPL v3** (see [LICENSE](LICENSE)). Contributions to this
fork are made under the same license. Upstream copyright notices are kept.

## LLM-assisted code

LLM-assisted and LLM-generated code is welcome. What matters is **evidence**, not who or what
typed the code. A change to emulation behaviour should come with whichever of these apply:

- **A reproduction** of the problem: a failing test, scenario, test ROM, sanitizer report or model
  counterexample.
- **A source for the intended behaviour:** hardware documentation (NESdev wiki, Pan Docs, …),
  hardware test ROMs, or a reference emulator. Say which, and how strong it is.
- **A regression test** that fails before the change and passes after it, wired into a `mise`
  task.
- **The existing checks still passing:**
  - `mise run test:ci` (MesenTests regression suite);
  - `mise run test:cpu`, `test:gb-cpu`, `test:gb-irq`, `test:nes-dma`;
  - `mise run test:savestate` for state-related changes;
  - `mise run tla` when the modelled code (DMA scheduler, GB interrupt logic) changes. Update the
    matching spec too.
- **An entry in the findings log**: [docs/research/nes-verification-findings.md](docs/research/nes-verification-findings.md),
  [docs/research/gb-verification-findings.md](docs/research/gb-verification-findings.md), or a new
  log for another system. Record it even when the result is "not a bug" or "retracted".

Claims should be checked, not asserted: run the tool, cite the source, and label judgment calls
as judgment calls.

## Development environment

Use the Nix dev shell (`direnv allow`, or `nix develop`) and the `mise` tasks. See
[CLAUDE.md](CLAUDE.md) for the full list of commands and an architecture overview.

## Style

Keep the existing style. It's enforced with clang-format (C++) and dotnet format (C#):

- clang-format: `find ./ -iname '*.h' -o -iname '*.cpp' | xargs clang-format -i`, or `mise run format`
- dotnet format: `dotnet format`

Naming conventions:

- ExampleFunction
- exampleVariable
- _exampleMemberVariable

## Guidance

- **Performance.** Avoid adding work or branches to emulation hot paths. Check maximum FPS with
  F9 (unlocked framerate) and F10 (FPS display).
- **Warnings.** MSVC builds treat warnings as errors.
- **Commit messages.** Explain the context and the evidence, and point at the test, ROM or
  finding that covers the change.
