# Design history

Finished plans, bench findings and the agent prompts that executed them. They are kept
because code comments cite their rulings and sections (`D41`, `§3`), and because the
reasons behind a decision live here. They describe the tree **as it was when written**.
For the tree as it is, read [../architecture.md](../architecture.md). The living ledgers
are still in `docs/`: [app-platform-plan.md](../app-platform-plan.md) (the master record
of numbered rulings) and [composer-stack-plan.md](../composer-stack-plan.md) (D43).

## Rulings and designs (built)

| File | Record |
|---|---|
| [behaviour-vocabulary.md](behaviour-vocabulary.md) | D15: what an app may register with the OS |
| [app-theme-editor-plan.md](app-theme-editor-plan.md) | D37: each app's theme editor ships inside its `.orbapp` |
| [sound-plan.md](sound-plan.md) | D39: clock ticks and chimes as theme content |
| [hand-motion-plan.md](hand-motion-plan.md) | D41: every hand and pointer names its source, path, cadence and depth |
| [element-model-plan.md](element-model-plan.md) | D42: one row for everything that moves (superseded for the editor by D43) |
| [hub-app-library-plan.md](hub-app-library-plan.md) | S6: the hub's app library |
| [orb-manager-plan.md](orb-manager-plan.md) | the hub's hardening and capability iterations 0–6 |
| [firmware-versioning-plan.md](firmware-versioning-plan.md) | the version stamp scheme (the reference is [../firmware-versioning.md](../firmware-versioning.md)) |
| [sntp-timezone-plan.md](sntp-timezone-plan.md) | SNTP and the device time zone |
| [theme-identity-plan.md](theme-identity-plan.md) | a theme's name lives in the file, not the filename |
| [theme-preview-plan.md](theme-preview-plan.md) | the reserved preview plane |
| [theme-clock-migrations.md](theme-clock-migrations.md) | the single-slot theme store and the clock through the compositor |
| [theme-delta-mmap-plan.md](theme-delta-mmap-plan.md) | theme delta storage and the mmap load |
| [unified-render-path-design-2026-10-30.md](unified-render-path-design-2026-10-30.md) | one commit path for every dial |
| [usb-sd-msc-plan.md](usb-sd-msc-plan.md) | USB access to the SD card (shipped as MSC; MTP removed) |
| [watch-composer-plan.md](watch-composer-plan.md) | the standalone composer, before D37/D43 |

## Plans not built, or superseded

| File | Status |
|---|---|
| [provisioning-first-plan.md](provisioning-first-plan.md) | provisioning-first boot; supersedes the QR plan below |
| [ap-onboarding-qr-plan.md](ap-onboarding-qr-plan.md) | superseded by provisioning-first |
| [device-pace-model-plan.md](device-pace-model-plan.md) | the editor's device-pace model task |

## Bench findings

| File | Measured |
|---|---|
| [fps-budget-benchmark-plan.md](fps-budget-benchmark-plan.md), [fps-budget-findings-2026-10-10.md](fps-budget-findings-2026-10-10.md), `fps-budget-results-2026-10-10.csv` | flush time against refreshed rows and blended pixels |
| [rows-capacity-2026-10-10.md](rows-capacity-2026-10-10.md), `fps-rows-capacity-results-2026-10-10.csv` | what can animate at 4/8/12/15 fps |
| [render-cost-findings-2026-10-30.md](render-cost-findings-2026-10-30.md) | why a still-plate dial drew at 7 fps |
| [bugfix-batch-2026-10-29.md](bugfix-batch-2026-10-29.md) | six bench bugs after the first-flash provisioning run |

The scripts that produced the 2026-10-10 sweeps (`tools/bench/pt.sh`, `cadence.py`,
`fit.py`, `rowsfit.py`) were removed once the architecture they measured was gone. They
are in git history.

## Agent prompts and handoffs

Session artifacts that drove an implementation. They record the instructions as given:
[p8-agent-prompt.md](p8-agent-prompt.md), [theme-clock-agent-prompt.md](theme-clock-agent-prompt.md),
[theme-delta-mmap-agent-prompt.md](theme-delta-mmap-agent-prompt.md),
[unified-render-path-agent-prompt.md](unified-render-path-agent-prompt.md),
[AGENT_HANDOFF_2026-10-23.md](AGENT_HANDOFF_2026-10-23.md).
