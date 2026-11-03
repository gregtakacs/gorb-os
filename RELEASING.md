# Releasing Orb Manager

This page covers the release workflow and its signing setup. The build steps that come
before a tag (firmware bundle, shelf apps, `make check`) are in
[BUILDING.md § Cutting a release](BUILDING.md#cutting-a-release).

A release is the hub for three platforms as a single executable, with the current
firmware, the shelf apps and the default theme built in:

| File | For | Signed with |
|---|---|---|
| `orb-manager-<tag>-windows-amd64.zip` | Windows 10/11, x64 | Azure Artifact Signing |
| `orb-manager-<tag>-macos.dmg` | macOS 12+, Apple silicon and Intel (universal) | Developer ID, notarized and stapled |
| `orb-manager-<tag>-linux-amd64.tar.gz` | Linux x86-64 | none (the sha256sums cover it) |
| `orb-sdk-<os version>.tar.gz` | app developers ([apps/README.md](apps/README.md)) | none |

`.github/workflows/release.yml` builds and signs all three on GitHub Actions. It does
the signing only when the secrets below exist. Without them it still ships: the
Windows exes go out unsigned and the Mac app is ad-hoc signed, and the release notes
tell users how to get past the warning.

## Cutting a release

1. Commit everything. The firmware bundle must be at the OS version (`make firmware`),
   and the shelf apps must be re-baked (`tools/make_factory_apps.py`). These are the
   usual flows.
2. Tag and push. The tag must equal `version.txt`. For a hub-only rebuild of the same
   OS, add a suffix, e.g. `v1.2.6-2`.
   ```sh
   git tag v1.2.6 && git push origin v1.2.6
   ```
3. Wait for the run under Actions → release. Notarization is usually the slowest
   step. It ends with a **draft** release that has the three downloads, `sha256sums.txt`,
   and notes listing the bundled app versions.
4. Read the draft over, then press Publish. Nothing is public until you do.

To build and sign without releasing, use Actions → release → Run workflow. The files
land on the run's page.

To try the packaging locally, run `make dist` in `tools/orb-manager`. It builds the
Linux and Windows archives on any host, plus the Mac .dmg when run on a Mac. Nothing
local is signed.

## One-time setup: Apple (macOS signing and notarization)

You need the Apple Developer Program ($99/year). Enroll as an individual or as an
organization at developer.apple.com/programs.

1. **Developer ID Application certificate.** On a Mac, open Xcode → Settings →
   Accounts → your team → Manage Certificates → + → *Developer ID Application*. Only
   the account holder can create it. Without Xcode: make a CSR in Keychain Access
   (Certificate Assistant → Request a Certificate from a CA), upload it at
   developer.apple.com → Certificates → +, then double-click the downloaded .cer.
2. **Export it.** In Keychain Access, open My Certificates, right-click
   *Developer ID Application: …*, choose Export, and save it as a .p12 with a strong
   password. Make sure the private key comes along: expand the row to check.
3. **Find the identity string:**
   ```sh
   security find-identity -v -p codesigning
   # "Developer ID Application: Your Name (TEAMID1234)"
   ```
4. **Notary API key.** In App Store Connect, go to Users and Access → Integrations →
   App Store Connect API → Team Keys and press +. Give it the role *Developer*.
   Download the `AuthKey_XXXX.p8`; Apple lets you download it only once. Note the
   **Key ID** and the **Issuer ID** shown above the table.

Secrets:

| Secret | Value |
|---|---|
| `MACOS_CERT_P12` | `base64 -i cert.p12` output |
| `MACOS_CERT_PASSWORD` | the .p12 export password |
| `MACOS_SIGN_IDENTITY` | the full identity string from step 3 |
| `APPLE_API_KEY_P8` | `base64 -i AuthKey_XXXX.p8` output |
| `APPLE_API_KEY_ID` | Key ID |
| `APPLE_API_ISSUER` | Issuer ID |

The workflow signs only if both `MACOS_CERT_P12` and `APPLE_API_KEY_P8` are set. It
signs the bundle (hardened runtime, secure timestamp), then the .dmg. It
notarizes the .dmg, staples the ticket, and fails the run if Gatekeeper rejects the
result.

The Developer ID certificate lasts 5 years. When it expires, replace `MACOS_CERT_P12`.
Apps you already shipped keep working, because the signature carries a timestamp.

## One-time setup: Azure Artifact Signing (Windows)

Microsoft renamed this service; older docs call it *Trusted Signing*. It costs about
$10/month on the Basic tier. It is a cloud signing service, so there is no key file or
hardware token to look after. It is open to organizations and to individual developers
in supported countries (check the current eligibility page). Identity validation for
an individual means a government ID check and can take a few days.

1. **Azure subscription.** Use portal.azure.com. Pay-as-you-go is fine.
2. **Register the provider.** Go to Subscriptions → yours → Resource providers and
   register `Microsoft.CodeSigning`.
3. **Create an Artifact Signing account.** Search the portal for "Artifact Signing"
   (or "Trusted Signing") and choose Create. Pick a region; it determines the
   **endpoint**, e.g. East US is `https://eus.codesigning.azure.net/`. Note the
   **account name**.
4. **Identity validation.** In the account, go to Identity validation → New →
   *Public* (individual or organization) and follow the ID check. Wait for
   *Completed*.
5. **Certificate profile.** In the account, go to Certificate profiles → Create →
   *Public Trust* and pick the validated identity. Note the **profile name**.
6. **An identity for CI.** In Microsoft Entra ID, go to App registrations → New
   registration (e.g. `orb-manager-signing`). Note the **Application (client) ID** and
   **Directory (tenant) ID**. Then go to Certificates & secrets → New client secret and
   copy its **value**.
7. **Let it sign.** On the signing account, go to Access control (IAM) → Add role
   assignment. Choose *Artifact Signing Certificate Profile Signer* (in older portals:
   *Trusted Signing Certificate Profile Signer*) and assign it to the app registration
   from step 6.

Secrets:

| Secret | Value |
|---|---|
| `AZURE_TENANT_ID` | Directory (tenant) ID |
| `AZURE_CLIENT_ID` | Application (client) ID |
| `AZURE_CLIENT_SECRET` | the client secret value |
| `AZURE_SIGNING_ENDPOINT` | e.g. `https://eus.codesigning.azure.net/` |
| `AZURE_SIGNING_ACCOUNT` | account name |
| `AZURE_CERT_PROFILE` | certificate profile name |

The workflow signs only if `AZURE_CLIENT_SECRET` is set. It then checks the
signature on the runner and fails the run if it does not verify.

The client secret expires, after 24 months at most. Put a renewal date in your
calendar. When it lapses, the Windows job fails loudly; it does not ship unsigned.
SmartScreen reputation builds up over the first downloads. A brand-new identity can
still show "Windows protected your PC" for a short while, even when signed.

## Adding the secrets

In GitHub, go to the repo → Settings → Secrets and variables → Actions → New
repository secret. Or use the CLI:

```sh
base64 -i cert.p12 | gh secret set MACOS_CERT_P12
gh secret set MACOS_CERT_PASSWORD      # prompts for the value
gh secret set MACOS_SIGN_IDENTITY --body 'Developer ID Application: Your Name (TEAMID1234)'
base64 -i AuthKey_XXXX.p8 | gh secret set APPLE_API_KEY_P8
gh secret set APPLE_API_KEY_ID --body XXXX
gh secret set APPLE_API_ISSUER --body xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
gh secret set AZURE_TENANT_ID --body …   # and the other five the same way
```

Then run the workflow once by hand (Run workflow) and check that each job's signing
step ran, before you push a tag.

## What is in the box

- **The executable, and nothing else to run.** It has the web UI, the editor runtime
  (React, Babel, 36 fonts), the firmware bundle and the shelf compiled in, so it needs
  no network to flash or edit. The shelf is every app in `tools/orb-manager/apps` plus
  the default theme in `themes/`. On first run `seed.go` copies it into the user's
  library: `%AppData%\orb-manager` on Windows, `~/Library/Application Support/orb-manager`
  on macOS, `~/.config/orb-manager` on Linux. A ledger there (`.seeded.json`) makes the
  rules hold: a file the owner deleted is never put back; a later release replaces only
  a copy nobody changed (apps only at a higher version); a file the owner had or edited
  is never touched. Deleting the folder brings the shipped set back.
- **README.txt.** First-run notes per OS (`tools/orb-manager/packaging/<os>/README.txt`).
- **LICENSE.txt and THIRD_PARTY_NOTICES.txt.** The second covers everything the
  release redistributes: Go modules, React/ReactDOM/Babel, the fonts (all OFL-1.1),
  and the firmware components in the embedded image (ESP-IDF, esp_tinyusb/TinyUSB,
  esp_lcd_co5300, LVGL, wasm3). Regenerate it when a dependency changes, with `python3 packaging/notices.py >
  packaging/THIRD_PARTY_NOTICES.txt`.
- **Linux extras.** `install.sh` (per-user install with a menu entry), a `.desktop`
  file, and `60-orb.rules`, which grants the logged-in user USB serial access without
  needing the dialout group.

Not done yet: auto-update, Windows ARM64 and Linux ARM64 builds, a Windows installer
(.msi), and Linux distro packages (.deb, Flatpak). The archive layout is the same on
every OS, so adding any of these later is a packaging change, not a hub change.
