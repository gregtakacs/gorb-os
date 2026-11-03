# Contributing to gorb-os

Thanks for helping. This page covers how changes get in. [BUILDING.md](BUILDING.md)
covers how to build and check them.

## Where things go

| You have | Do this |
|---|---|
| a bug | open an issue with the **Bug report** form |
| an idea or a change you want to make | open an issue with the **Feature request** form first, so we can agree on the shape before you write it |
| an app you wrote | share it in [Discussions](https://github.com/gregtakacs/gorb-os/discussions) under Show and tell. It does not need to live in this repo (see [apps/README.md](apps/README.md)) |
| a question | ask in Discussions |
| a security problem | follow [SECURITY.md](SECURITY.md); please do not open a public issue |

Small fixes (a typo, a broken link, an obvious one-line bug) can go straight to a pull
request.

## Making a change

1. Fork the repo and branch from `main`.
2. Make the change, with its tests (below).
3. Run `make check`. It needs no board. CI runs the same checks, plus a firmware build.
4. Open a pull request. The template asks what changed, why, and how you checked it.
   CI must be green before it merges.

Keep a pull request to one change. A refactor and a feature in one PR are hard to review
and hard to revert.

### Commit messages

Match the history: a subject line that says what changed, then a body that says why.

```
Hub: the shelf is compiled in and seeds the user library

A release is one executable now. The old exe-dir shelf meant a downloaded
hub with no folder beside it answered an empty library...
```

Scope prefixes in use: `Hub:`, `Editor:`, `Clock:`, `Stage:`, `Apps:`, `Release:`,
`Plan:`, and `fix(hub):` style for fixes. Do not add `Co-Authored-By`, `Signed-off-by`,
or tool-credit trailers.

## The rules the tree enforces

These are checked by `make check`, so you will hear about them anyway. Knowing them up
front saves a round trip.

- **One stamp names one set of bytes.** If your change alters an app's `.orbapp`, bump
  `apps/<slug>/version.txt` and run `make apps`. The clock's theme editor ships inside
  `clock.orbapp`, so an editor change is a clock change. Firmware changes bump the root
  `version.txt`. A version is `vMAJOR.MINOR.PATCH`.
- **Formats move together.** Each file format has a Python writer in `tools/`, a C reader
  in `main/`, and a host suite that runs the shipped C against the Python. Change all
  three in one commit, or the suite fails.
- **Host suites run the shipped code.** A `tools/host_*_test.py` suite lifts functions
  verbatim out of `main/` and compiles them with gcc. When you change one of those
  functions, its suite may need a matching stub or vector. That is the suite doing its
  job, so update it rather than skip it.
- **The SDK matches the firmware.** `apps/sdk/binds.json` is generated from the OS's bind
  table. Run `python3 apps/sdk/orbsdk.py binds --write` after adding or changing an app
  import. `apps/sdk/clk_types.h` must stay a byte copy of `main/art/clk_types.h`.
- **The hub ships committed artifacts.** The firmware bundle, shelf apps, default theme
  and vendored editor runtime are all committed, so a plain `go build` makes a working,
  offline hub. Re-generate them with their own targets (BUILDING.md). Do not edit them
  by hand.

## Testing on hardware

Most changes are covered by `make check`. If your change touches what the orb draws,
its timing, the network or the USB path, say in the PR what you ran on a board (the
board, firmware version, and what you saw). The bench scripts in `tools/bench/` and
`tools/fbgrab.py` (a frame grab off the glass) help with that. Not having a board is
fine: say so, and a maintainer will bench it.

## Design records

Bigger decisions are numbered rulings (`D19`, `D43`, …) and sections (`§43`), cited from
code comments:

- [docs/app-platform-plan.md](docs/app-platform-plan.md) is the master ledger of the
  OS/app split, the broker, grants and storage.
- [docs/composer-stack-plan.md](docs/composer-stack-plan.md) covers the theme editor and
  the panel preview.
- [docs/architecture.md](docs/architecture.md) is the map of the tree.
- [docs/history/](docs/history/) holds the finished plans and bench findings the code
  still cites.

When a change overturns a recorded decision, say so in the PR and update the record in
the same PR.

## Licence

gorb-os is MIT licensed ([LICENSE](LICENSE)). By contributing you agree that your
contribution is released under the same licence.
