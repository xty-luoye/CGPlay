# QuickLook Health and Recovery

The health probe is intentionally outside the Explorer hook and foreground/Space handling code.

## Checks

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_quicklook_health.ps1 -AppRoot "$env:LOCALAPPDATA\Programs\CGPlay" -RequireRunEntry
~~~

Controlled startup probe:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_quicklook_health.ps1 -AppRoot "$env:LOCALAPPDATA\Programs\CGPlay" -StartIfStopped -RequireRunning -RequireSingleInstance -RequireRunEntry
~~~

The report records executable version/hash, matching process IDs, duplicate-instance status, Run target, and recovery suggestions.

## Manual Recovery

1. Close duplicate `CGPlayQuickLook.exe` processes.
2. Start the executable from the installed CGPlay directory.
3. Rerun the strict health probe.
4. If the executable or Run target is missing, repair or reinstall instead of editing Explorer hook behavior.

Do not change Explorer/preview foreground detection or Space-consume rules as part of lifecycle recovery.
