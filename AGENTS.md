# Repository guidance

## Scope and layout

- This repository contains one standalone Windhawk mod for the native Windows 11
  x64 taskbar. Keep it focused on secondary-taskbar ordering.
- `src/secondary-taskbar-pinned-order.wh.cpp` must remain directly importable into
  Windhawk, including metadata and license notices. Avoid local
  header dependencies that would break single-file import.
- `tests/ordering.test.cpp` includes the production planner using
  `SECONDARY_TASKBAR_ORDER_PLANNER_ONLY`; keep that path independent of Windows
  hooks. Its expected-order oracle must remain independent of the planner.
- Keep user-facing behavior, compatibility evidence and upstream credits in
  `README.md`, without an embedded copy in the mod source. Keep exact hook
  signatures, timing constants and layout details beside their implementation
  rather than duplicating them in documentation.
- Generated binaries, logs and snapshots belong under the Git-ignored `build/`.
  Live snapshots can contain window titles and pin data; do not commit them.

## Workstation and verification rules

- Read-only repository/environment inspection and offline compilation are allowed.
- Ask for permission and wait before executing tests, including the offline
  planner tests, or running live diagnostic trials. Explain the exact operation
  and whether it can affect the desktop. Past test approval is not blanket
  authorization for additional scenarios.
- Obtain explicit approval before installing/enabling/disabling the mod, changing
  Windows or Windhawk settings, restarting Explorer, or manipulating windows,
  pins or monitors for a trial. Windhawk's editor Compile action can load/reload
  the mod immediately; it is a live operation, unlike the offline build script.
- Compilation only:

  ```powershell
  pwsh -NoProfile -File .\tools\check-offline.ps1 -BuildDll -BuildPlannerTests
  ```

- After test-execution approval:

  ```powershell
  pwsh -NoProfile -File .\tools\check-offline.ps1 -RunPlannerTests
  ```

- The standalone test executable needs its statically linked C++ runtime;
  Windhawk's dynamic import libraries reference `.whl` files used by its loader.
- For an approved live trial, capture primary order/saved pins and secondary
  membership before and after. The trial helper disables this local mod on a
  failure. Report its final enabled/disabled and logging state. Treat planner
  success, native symbol resolution and visible taskbar behavior as separate
  evidence; update README compatibility claims only for scenarios actually run.
- Do not commit, push or publish unless explicitly requested.

## Ordering and native API constraints

- The live primary pinned-group sequence is authoritative and read-only. Never
  rewrite its order, saved Taskband values or pinned shortcuts.
- Identify apps by native task-group identity, then ordinal case-insensitive
  AppID. Keep profile-specific IDs distinct; do not substitute titles, EXE lists,
  shortcut-directory order or normalized/stripped identity suffixes.
- Sort only the groups Windows already includes on each secondary taskbar. Leave
  monitor/virtual-desktop/minimized-window filtering, grouping and within-app
  window order to Windows. Never create missing pins or placeholders.
- `NextMove` implements stable ordering through leftward moves of pinned groups.
  Apply moves through the local secondary `CTaskListWnd::TryMoveGroup` path so
  Explorer updates both its model and UI. Preserve the before/after primary-order
  and expected-secondary-permutation checks.
- Do not directly reorder DPAs or invoke pin-management/global drag handlers.
  `SetRelativeTaskOrder` is observed, not used to sort: it can create absent
  groups. Public `ITaskbarList3::SetTabOrder` orders application tabs, not these
  taskbar groups.
- Resolve internals through Windhawk symbols. Preserve process/thread, ancestry,
  primary/secondary, vtable, readable-memory and DPA-count checks. Derive field
  offsets from validated getter code; reject unknown layouts rather than using
  guessed or previously observed offsets. Keep required hooks required.

## Threading and lifecycle constraints

- Native mutation hooks should forward normal calls and schedule deferred work,
  not sort while Explorer is mutating its lists. Preserve native-depth, sorting
  and generation guards; discard snapshots when reentrant work changes the epoch.
- Read objects and move groups only on the taskbar thread. The worker discovers
  HWNDs and posts work, without dereferencing taskbar objects. Keep updates
  event-driven with coalescing and bounded settling retries, not idle polling.
- Re-enumerate live taskbars for each pass and retain snapshot COM references.
  Never queue raw taskbar/group/DPA pointers across threads or between passes.
- Before Windhawk removes hook trampolines, cancel timers, remove the subclass
  synchronously and drain outstanding callbacks. Account for nested message
  delivery and HWND destruction/reuse. A timeout is not permission to unload code
  while a callback can still execute it.

## Licensing

The project is GPL-3.0-only; Windhawk metadata uses `@license GPL-3.0`. Retain the
full upstream MIT copyright and permission notices for adapted portions in the
standalone source. Keep the attribution links in README when changing or reusing
code, and check the licenses of any additional sources.
