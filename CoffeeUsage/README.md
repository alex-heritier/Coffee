# CoffeeUsage

A lightweight macOS menu bar app that shows how much of your coding agents' plan limits you have left. A resource-efficient reimplementation of the CodexBar idea, using a C++ core and AppKit shell.
Providers: Codex, Claude, DeepSeek, Grok, Muse Code, OpenRouter.

- **C++ core** (`src/core.cpp`, `src/json.hpp`): credential discovery, request building, JSON parsing, pace math.
- **Thin Swift shell** (`src/app.swift`, via the plain-C `src/bridge.h` / `src/bridge.cpp`): status item, arrowless panel drawn directly with CoreGraphics (no SwiftUI), URLSession.
- Uses a coalesced background scheduler; in the background only the menu bar provider is refreshed. Resource use has not been benchmarked here.

## Build & run

Requires macOS 13 or later and Xcode Command Line Tools. No third-party dependencies. With Node.js and npm available, run these commands from the repository root (no `npm install` needed):

```sh
npm run run:coffeeusage     # build + launch
npm run install:coffeeusage # copy to /Applications and launch
npm run dump:coffeeusage    # print every provider's usage to the terminal
```

For a standalone build without npm, run `sh build.sh` inside `CoffeeUsage/`. The script rebuilds both architectures on each invocation.

## Credentials

| Provider   | Source |
|------------|--------|
| Claude     | Claude Code login (`~/.claude/.credentials.json` or Keychain `Claude Code-credentials`) |
| Codex      | `~/.codex/auth.json` (`$CODEX_HOME`) |
| Grok       | `~/.grok/auth.json` |
| Muse Code  | `~/.config/muse/auth.json` + Keychain `ai.meta.dev.credentials` |
| DeepSeek   | API key → menu → *Set API Key…* (stored in Keychain) |
| OpenRouter | API key → menu → *Set API Key…* (stored in Keychain) |

`CoffeeUsage/build/CoffeeUsage.app/Contents/MacOS/CoffeeUsage --import-env-keys` copies `DEEPSEEK_API_KEY` / `OPENROUTER_API_KEY` from your shell into the Keychain.

CoffeeUsage never refreshes or rotates CLI tokens, because rotating them would log the CLI out. If a login expires, run that CLI once and it renews the login.
Reading another app's Keychain item needs a one-time **Always Allow** — pick **Always Allow**, not **Allow**: Allow approves only the running process, so the next launch or refresh asks again. Background refreshes never show a prompt. Clicking **Refresh** can.
Rebuilding changes the ad-hoc code signature, so macOS may ask again after each rebuild (and DeepSeek/OpenRouter keys look missing until re-entered). Sign with a stable identity to stop that — see below.

## Stable code signing (fewer keychain prompts)

macOS ties every "Always Allow" grant to the app's code signature, and the default ad-hoc signature (`codesign --sign -`) changes on every rebuild. That is why the Meta AI and Claude password prompts keep coming back, and why stored API keys seem to vanish after rebuilding. With a stable self-signed identity, you approve each item once and rebuilds stop invalidating the grants.

One-time setup:

1. Open Keychain Access → Certificate Assistant → Create a Certificate. Name it e.g. `CoffeeUsage Local`, set Identity Type to Self Signed Root and Certificate Type to Code Signing, then create it.
2. Build with that identity from the repository root: `CODESIGN_ID="CoffeeUsage Local" npm run build:coffeeusage`. To re-sign an existing build without recompiling: `CODESIGN_ID="CoffeeUsage Local" npm run resign:coffeeusage`. Set `CODESIGN_ID` on each build to keep the same identity; the default is ad-hoc signing.
3. Launch, click **Refresh**, and choose **Always Allow** for each Keychain item (Claude's login, Muse's login, and your API-key items if asked).

After that, rebuilds keep the same identity and macOS stops asking.

## Refresh policy (battery)

- In the background, only the provider shown in the menu bar is refreshed. `NSBackgroundActivityScheduler` runs it every 5 min, or every 15 min in Low Power Mode, and macOS batches it with other wakeups.
- Opening the popover refreshes any provider whose data is more than 60 s old. ⌘R forces a refresh of everything.
- After a 401/403/429 response, that provider backs off (15 min → 2 h). After waking from sleep, the app refreshes once if its data is stale.

## Notes

- Click a provider tab to show that provider in the menu bar. The choice persists. Overview doesn't change it.
- Each quota shows how much is left and when it resets. Balance-only providers show their credit balance.
- "Plan renews" isn't shown. It's only available from Claude's web dashboard (browser cookies), which this app doesn't read.

CoffeeUsage retains the original `local.usagebar` bundle identifier, `UsageBar API keys` Keychain service, and `~/Library/Application Support/UsageBar` data directory so existing settings, API keys, and learned quota history remain available.

## UI previews

From the repository root, render every tab in light and dark mode with sample data, without provider requests or changes to the selected provider:

```sh
COFFEEUSAGE_SNAPSHOT=/tmp/coffeeusage CoffeeUsage/build/CoffeeUsage.app/Contents/MacOS/CoffeeUsage
```

This writes `/tmp/coffeeusage-0-light.png` through `/tmp/coffeeusage-6-dark.png` and exits. The previews cover quotas, balances, loading, and missing credentials.
