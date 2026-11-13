# Coffee

Two independent native macOS menu bar apps in one repository. Both require macOS 13 or later and Xcode Command Line Tools, with no third-party dependencies.

| App | Purpose | Implementation |
| --- | --- | --- |
| [Coffee](Coffee/README.md) | Keep your Mac awake for a chosen duration using `caffeinate`. | Swift and AppKit |
| [CoffeeUsage](CoffeeUsage/README.md) | Track coding-agent usage with a resource-efficient implementation of the CodexBar idea. | C++ core, Swift shell / AppKit |

## Build and run

Requires Node.js and npm to run the commands below. No `npm install` is needed. From the repository root:

```sh
npm run build           # build both apps
npm run build:coffee    # build Coffee
npm run build:coffeeusage # build CoffeeUsage
npm run run:coffee      # build and launch Coffee
npm run run:coffeeusage # build and launch CoffeeUsage
```

App bundles are built into each app's own directory:

- `Coffee/build/Coffee.app`
- `CoffeeUsage/build/CoffeeUsage.app`

Copy either bundle to `/Applications` to install it. See each app's README for its standalone build commands and usage details.

## Layout

```text
Coffee/       # source, assets, metadata, and build script
CoffeeUsage/  # source, assets, metadata, and build script
package.json  # build and app commands
```

Each app owns its build and can be developed independently. Generated `build/` directories are ignored. CoffeeUsage preserves its original UsageBar storage identifiers to retain existing user data.
