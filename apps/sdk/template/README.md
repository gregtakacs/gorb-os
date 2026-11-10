# @TITLE@

A [gorb-os](https://github.com/gregtakacs/gorb-os) app.

## Build

You need `python3`, `make`, `curl`, and `clang` with `lld` (wasm-ld):

| OS | Install |
|---|---|
| Debian/Ubuntu | `sudo apt install clang lld make curl` |
| macOS | `brew install llvm lld` then `export ORB_CLANG=$(brew --prefix llvm)/bin/clang` |
| Windows | use WSL and the Debian/Ubuntu line |

```sh
make                          # fetch the SDK named in sdk.txt, write build/@SLUG@.orbapp
make install ORB=192.168.1.50 # upload to an orb on your LAN and install (it reboots)
```

You can also import `build/@SLUG@.orbapp` in Orb Manager: open **Apps**, then
**Import**, and install it from there.

## Layout

| File | What it is |
|---|---|
| `app.json` | the manifest: slug, title, caps, settings, beat |
| `version.txt` | this app's release, `vMAJOR.MINOR.PATCH`; bump it whenever the bytes change |
| `sdk.txt` | the gorb-os release whose SDK this app builds against |
| `*.c` | the sources; every `.c` file here is compiled |
| `editor/_fixtures/` | only once the app has a theme editor: one design per past schema, see apps/README.md |

The SDK's `orb.h` documents every call the OS offers. The SDK guide is
[apps/README.md](https://github.com/gregtakacs/gorb-os/blob/main/apps/README.md) in gorb-os.

## Release

Tag a commit with the version in `version.txt` (`git tag v0.1.0 && git push --tags`).
The workflow checks the migrations of every design in `editor/_fixtures/` before it packs,
and attaches the `.orbapp` to a GitHub release.

To release to GitHub and to the site with one tag push, set the repository **secret**
`ORB_SITE_TOKEN` (a developer token from the site's `/dev` page, scoped to this app's slug,
shown once when created) and optionally the **variable** `ORB_SITE` (default `orb-os.net`):
a `v*` tag then also queues the `.orbapp` on the site, and an unset secret leaves that step
skipped — the release stays GitHub-only, exactly as before. If the site refuses the upload
the job fails with the GitHub release already out; fix and re-push the tag, and since the
same version with new bytes is refused, bump `version.txt`.
