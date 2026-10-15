# Secondary taskbar pinned order

A standalone Windhawk mod that orders running applications on secondary-monitor
taskbars using the **main taskbar's current pinned-app order**.

**Status: experimental.** Loading with already-open windows, secondary reordering,
and primary-pin preservation were verified. The broader window/monitor/pin-change
and lifecycle testing is still pending; this is not a full compatibility claim.

## Behavior

On each secondary taskbar:

1. Applications pinned on the main taskbar come first, in the main pins' relative order.
2. Unpinned applications follow, preserving their current relative order.
3. Only groups that Windows already includes on that taskbar are reordered.

For example, with these main pins:

> Outlook → VS Code → Claude Desktop → OpenCode Desktop → Cursor

If the external monitor has Cursor, Claude Desktop, VS Code, and an unpinned app,
its taskbar becomes:

> **VS Code → Claude Desktop → Cursor → unpinned app**

Outlook and OpenCode don't get empty slots or launcher-only buttons. Opening the
windows in a different sequence, or moving them to that monitor in a different
sequence, should produce the same pinned-app ordering. With two unpinned apps,
their existing relative order is retained.

The pins are read from Explorer's live main taskbar, not from this example or a
configuration file. Pinning, unpinning, and rearranging the main pins schedule a
fresh ordering pass. The mod neither saves a pin list nor calls pin-management
APIs. The main taskbar is read-only to the ordering algorithm.

Windows continues to decide which windows belong on each taskbar and virtual
desktop. Minimized windows count exactly as Windows normally counts them. Native
grouping, combining, labels, previews, activation, and the order of windows inside
each application group are left to Explorer. A change can show a native movement
animation; synchronization is deferred briefly rather than performed in the
middle of a native taskbar operation or drag.

## How it works

The mod reads primary pinned flags and application identities from Explorer's
taskbar groups, then stably orders the existing secondary groups. It moves whole
groups through the native `CTaskListWnd::TryMoveGroup` path so Explorer updates
both its internal order and visible UI. Native taskbar events schedule deferred
updates on the taskbar thread; there is no idle polling.

This requires private Explorer interfaces resolved through Windhawk's symbol
facilities. The public taskbar APIs do not expose arbitrary per-monitor application
group ordering, which is why compatibility depends on Windows symbols and layouts.

## Compatibility and verification status

Still unverified: other opening/movement sequences; live pin/unpin and primary
rearrangement; minimize/restore and virtual-desktop changes; combining thresholds,
previews and activation; Explorer restart, monitor reconnection and unload stress.

Required Windows configuration:

- **Show my taskbar on all displays** enabled.
- **Show my taskbar apps on** set to **Taskbar where window is open**
  (`MMTaskbarMode = 2`), or **Main taskbar and taskbar where window is open** (`1`).
- The native Windows 11 taskbar, rather than an ExplorerPatcher/StartAllBack
  replacement.

The mod reads these settings; it does not change them. In “All taskbars” mode
(`0`) it stays inactive, since Windows deliberately shows a different membership
set there.

## Offline build checks

From the repository, with PowerShell 7 and Windhawk installed in its default location:

```powershell
# Compiler syntax/type check only; no DLL output and no injection.
pwsh -NoProfile -File .\tools\check-offline.ps1

# Compile/link the mod DLL and build the test executable, without running either.
pwsh -NoProfile -File .\tools\check-offline.ps1 -BuildDll -BuildPlannerTests

# Compile and RUN the platform-independent planner tests (authorized and passed).
# Writes build\ordering.test.exe, then runs that console executable.
pwsh -NoProfile -File .\tools\check-offline.ps1 -RunPlannerTests
```

For a different installation, append `-WindhawkPath "D:\Apps\Windhawk"`.
For a different engine import library, append `-EngineVersion "<version>"` (the
default is `1.7.3`). The syntax check uses the real Windhawk API declarations, not
editor stubs. `-BuildDll` also links against `windhawk.lib`, `comctl32` and
`advapi32`, producing `build\secondary-taskbar-pinned-order.dll`. These outputs
are ignored by Git. This does not register the DLL with Windhawk, load it, resolve
Windows symbols, or validate private taskbar ABI behavior. Import the source
through Windhawk for installation; do not manually inject the build output.

`-BuildPlannerTests` produces a console executable but does not run it.
`-RunPlannerTests` explicitly builds **and runs** it. The executable contains only
the pure ordering planner and test code, not Explorer hooks. Its C++ runtime is
linked statically so the standalone process does not depend on Windhawk's renamed
`.whl` runtime files or a modified `PATH`. **3,829,849 generated input combinations
passed**: every pair of ordered subsets of six abstract apps, plus additional
equal-rank/no-pin checks. These model checks do not exercise Explorer integration.

## Import, build, install, and enable

Windhawk's editor compile action can immediately load/reload a local mod in
Explorer; it is not the offline syntax check above.

1. Open Windhawk and use **Create a new mod**. Enable its advanced/developer UI
   if that option is hidden in your installed release.
2. Replace the generated source with the **entire** contents of
   `src/secondary-taskbar-pinned-order.wh.cpp`, including its metadata.
3. Use the editor's **Compile** action to build the local mod. This creates a
   local/custom Windhawk entry; importing does not publish to the mod catalog.
4. Enable that entry if it is not enabled automatically. Windhawk resolves the
   required `taskbar.dll` symbols; first-time symbol downloads may take time.
5. Check the mod's Windhawk log for the Windows build, the DPA displacement and
   `Dispatcher attached ...`. An unsupported symbol/layout causes refusal or a
   skipped pass. `Sorting suspended until reload: ...` means automatic moves have
   stopped; capture the reason before changing anything further.
6. Confirm that secondary buttons follow the main pins and the main taskbar's
   order remains unchanged. Enabling is intended to reorder already-open windows
   without an Explorer restart.

There are no mod settings, app lists, companion services, or pin replication
components to configure.

## Disable or remove

Toggle the local mod off in Windhawk. The mod stops scheduling work, cancels its
timer, removes its taskbar subclass, drains callbacks and releases its references.
Windhawk removes the native hooks. Remove the local mod through Windhawk to delete
its installed entry.

Disabling does not undo already-completed secondary moves; Windows resumes
managing the existing order. No old order is restored, since such a snapshot could
contain closed windows or overwrite subsequent user changes. The mod creates no
saved pin configuration or separate order file to delete. An Explorer restart is
not part of normal disable/remove instructions.

## Limitations and other mods

- **Private Windows APIs:** symbol names, layouts and move notifications can
  change even within the supported build families. Required symbols, vtables,
  thread ownership, DPA counts and move results are checked. These checks reduce
  risk; they are not a substitute for testing the target Windows update.
- The first release accepts one known x64 `GetButtonGroupCount` instruction form.
  It refuses other forms rather than guessing an offset. All taskbars must use
  the normal primary taskbar thread and window hierarchy.
- A group with no usable AppID can match a primary pin by the shared native group
  identity. Otherwise it stays in the unpinned portion. Changed/profile-specific
  AppIDs are distinct applications. Duplicate matching pin identities use the
  first pin's rank.
- The mod preserves the **current** relative order of unpinned groups. It does not
  maintain a historical order for an unpinned app after Windows removes/recreates
  its button, or reverse rearrangements made by Windows or another mod.
- A manual secondary rearrangement that contradicts the primary pin order is
  corrected after the drag. User-initiated primary rearrangements still work
  normally, including Windows' normal handling of that drag.
- **Taskbar App Memory / Independent taskbar per monitor / grouping or ordering
  mods:** concurrent use is not supported or tested. They may give conflicting
  placement rules, change membership, synthesize identities or intercept the
  same native methods. This mod does not depend on any of them. Duplicate button
  groups for the same task group are rejected rather than moved ambiguously.
- Symbol/layout failures are logged. A failed or unexpected native move suspends
  sorting until the mod is reloaded. Taskbar reconstruction gets bounded settling
  retries; later native events trigger another attempt. There is no idle polling.
- Cleanup waits for Explorer's taskbar thread rather than unloading a callback
  still executing there. If Explorer is hung, disabling can wait for that thread.

## Credits and license

Copyright (c) 2026 DavDood. This project is licensed under the **GNU General Public
License version 3 only** (`GPL-3.0-only`); see [LICENSE](LICENSE). Windhawk metadata
uses the label `GPL-3.0`.

Adapted portions retain their original MIT notices in the standalone source:

- **buedgik — Taskbar App Memory 0.2.2**
  ([source, revision 75889517](https://github.com/ramensoftware/windhawk-mods/blob/75889517b3f88a2c1ab9d77938ed76a3fad560b1/mods/taskbar-app-memory.wh.cpp)):
  taskbar/interface discovery, checked memory reads, pinned flags and group/AppID
  access patterns. Its remembered-order persistence, menu and insertion hook are
  not included.
- **2ndSky95 — Independent taskbar per monitor 1.5.0**
  ([source, revision 004342fd](https://github.com/ramensoftware/windhawk-mods/blob/004342fdf226b83a905abffd4a880249e2c47679/mods/taskbar-independent-per-monitor.wh.cpp)):
  validated x64 getter-displacement extraction and taskbar layout/lifecycle
  patterns. Its pin replication, filtering and pin storage are not included.

The mod uses the installed [Windhawk SDK](https://github.com/ramensoftware/windhawk/wiki/Creating-a-new-mod);
SDK sources are not vendored in this repository.
