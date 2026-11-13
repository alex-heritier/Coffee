# Coffee

A tiny macOS menu bar app that keeps your Mac awake for a chosen duration.

Requires macOS 13 or later and Xcode Command Line Tools to build. No dependencies.

```sh
sh build.sh
open build/Coffee.app
```

Click the coffee cup in the menu bar and choose 15 minutes, 30 minutes, 1 hour, 2 hours, or a custom number of minutes. The cup fills while active, and the menu shows the time remaining. Choosing another duration starts a fresh timer. **Stop** or **Quit Coffee** ends the session.

Coffee runs `/usr/bin/caffeinate -dimsu -t <seconds> -w <Coffee PID>`. The timeout ends the session automatically, and watching Coffee's process releases the sleep assertions if the app exits. The `-s` system sleep assertion applies only on AC power, as defined by macOS.

To install, copy `build/Coffee.app` to your Applications folder.
