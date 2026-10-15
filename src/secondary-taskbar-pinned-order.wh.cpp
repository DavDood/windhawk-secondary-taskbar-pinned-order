// ==WindhawkMod==
// @id              secondary-taskbar-pinned-order
// @name            Secondary taskbar pinned order
// @description     Order running apps on secondary taskbars by the main taskbar's pins, followed by unpinned apps
// @version         0.1.0
// @author          DavDood
// @github          https://github.com/DavDood
// @homepage        https://github.com/DavDood/windhawk-secondary-taskbar-pinned-order
// @license         GPL-3.0
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -ladvapi32
// ==/WindhawkMod==

// Copyright (C) 2026 DavDood
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the repository or https://www.gnu.org/licenses/gpl-3.0.html.

/*
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
*/

/*
The adapted upstream portions retain the following MIT license notices.
These notices apply to those portions; the combined mod is GPL-3.0-only.

MIT License

Copyright (c) 2026 buedgik
Copyright (c) 2026 2ndSky95

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

// Kept in this file so importing the mod never needs a companion header.
// The offline test includes only this platform-independent planner.
namespace SecondaryTaskbarOrder {
struct Move {
    size_t from;
    size_t to;
};

Move NextMove(const std::vector<size_t>& ranks) {
    std::vector<size_t> order(ranks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ranks[a] < ranks[b];
    });
    size_t to = 0;
    while (to < order.size() && order[to] == to) {
        ++to;
    }
    return {to == order.size() ? to : order[to], to};
}
}  // namespace SecondaryTaskbarOrder

#ifndef SECONDARY_TASKBAR_ORDER_PLANNER_ONLY

#include <windhawk_utils.h>
#include <commctrl.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#if !defined(__x86_64__) || defined(_M_ARM64)
#error This mod currently supports native x64 Explorer only.
#endif

namespace {

constexpr DWORD kPinnedFlag = 1;
constexpr int kMaxGroups = 4096;
constexpr size_t kMaxAppId = 1024;
constexpr UINT kSettleMs = 100;
constexpr UINT kRetryMs = 250;
constexpr int kSettleAttempts = 4;

using GetInt = int(WINAPI*)(void*);
using GetGroup = void*(WINAPI*)(void*);
using GetAppId = PCWSTR(WINAPI*)(void*);
using GetFlags = DWORD(WINAPI*)(void*);
using CreateGroup = void*(WINAPI*)(void*, void*, int);
using MoveGroup = bool(WINAPI*)(void*, void*, UINT);
using TaskEvent = HRESULT(WINAPI*)(void*, void*, void*);
using PinEvent = void(WINAPI*)(void*, void*);
using SetOrder = void(WINAPI*)(void*, void*, int);
using UpdateFlags = HRESULT(WINAPI*)(void*, DWORD, DWORD);

GetInt g_isPrimary;
GetInt g_getButtonGroupCount;
GetGroup g_getGroup;
GetAppId g_getAppId;
GetFlags g_getFlags;
CreateGroup g_createGroupOriginal;
MoveGroup g_moveGroupOriginal;
TaskEvent g_taskCreatedOriginal;
TaskEvent g_taskDestroyedOriginal;
TaskEvent g_inclusionChangedOriginal;
PinEvent g_pinnedOriginal;
PinEvent g_unpinnedOriginal;
SetOrder g_setOrderOriginal;
UpdateFlags g_updateFlagsOriginal;
decltype(&CreateWindowExW) g_createWindowOriginal;

void* g_taskListUIVtable;
void* g_taskListWindowVtable;
void* g_buttonGroupVtable;
void* g_taskGroupVtable;
int g_buttonGroupsOffset = -1;
HMODULE g_taskbarModule;

std::atomic<bool> g_unloading{false};
std::atomic<bool> g_faulted{false};
std::atomic<ULONG64> g_epoch{0};
std::atomic<HWND> g_dispatchWindow{nullptr};
std::atomic<unsigned> g_subclassCalls{0};
HANDLE g_stopEvent;
HANDLE g_wakeEvent;
HANDLE g_worker;
UINT g_syncMessage;
UINT g_cleanupMessage;
int g_timerTag;

// These values are only used on the primary taskbar thread. The dispatcher is
// a subclass of that taskbar, so there is no mod-owned window class to outlive us.
bool g_timerArmed;
int g_attemptsLeft;
bool g_modeLogged;
bool g_readFailureLogged;
thread_local unsigned t_nativeDepth;
thread_local bool t_sorting;
thread_local void* t_automaticMoveTarget;

UINT_PTR TimerId() {
    return reinterpret_cast<UINT_PTR>(&g_timerTag);
}

void Wake() {
    // Handles remain open until Wh_ModUninit, after native hooks are removed.
    if (!g_unloading && !g_faulted && g_wakeEvent) {
        SetEvent(g_wakeEvent);
    }
}

void StopSorting(PCWSTR reason) {
    if (!g_faulted.exchange(true)) {
        Wh_Log(L"Sorting suspended until reload: %s", reason);
    }
}

// A posted timer can be dispatched from a nested message loop. Never inspect
// the lists while one of the observed native mutations is still on the stack.
// The epoch also invalidates snapshots if a native call reenters Explorer.
struct NativeMutation {
    NativeMutation() {
        ++t_nativeDepth;
        ++g_epoch;
    }
    ~NativeMutation() {
        ++g_epoch;
        --t_nativeDepth;
        Wake();
    }
};

bool Readable(const void* address, size_t size) {
    auto p = reinterpret_cast<uintptr_t>(address);
    if (p < 0x10000 || size > UINTPTR_MAX - p) {
        return false;
    }
    const uintptr_t end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &info, sizeof(info)) ||
            info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY))) {
            return false;
        }
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= p) {
            return false;
        }
        p = (std::min)(end, base + info.RegionSize);
    }
    return true;
}

bool HasVtable(void* object, void* vtable) {
    return vtable && Readable(object, sizeof(void*)) &&
           *static_cast<void**>(object) == vtable;
}

bool IsClass(HWND window, PCWSTR expected) {
    WCHAR name[64];
    return GetClassNameW(window, name, ARRAYSIZE(name)) &&
           wcscmp(name, expected) == 0;
}

bool IsOurWindow(HWND window, DWORD thread = 0) {
    DWORD process = 0;
    DWORD actualThread = GetWindowThreadProcessId(window, &process);
    return actualThread && process == GetCurrentProcessId() &&
           (!thread || actualThread == thread);
}

HWND FindPrimaryTray() {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, L"Shell_TrayWnd", nullptr))) {
        if (IsOurWindow(window)) {
            return window;
        }
    }
    return nullptr;
}

// Derive the DPA field from the symbol-resolved getter, as in the MIT reference
// mod. Accept only its known x64 prologue: mov rdx,[rcx+disp32]. No default
// offset, fabricated object calls, instruction patching, or scan for arbitrary
// DPAs. Every real DPA is checked against the getter before it is used.
bool ResolveDpaOffset() {
    const auto* code = reinterpret_cast<const BYTE*>(g_getButtonGroupCount);
    if (!Readable(code, 7) || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x91) {
        Wh_Log(L"Unsupported GetButtonGroupCount prologue");
        return false;
    }
    int offset;
    memcpy(&offset, code + 3, sizeof(offset));
    if (offset <= 0 || offset >= 0x400 || offset % sizeof(void*) != 0) {
        Wh_Log(L"Invalid button-group DPA displacement: %d", offset);
        return false;
    }
    g_buttonGroupsOffset = offset;
    Wh_Log(L"Button-group DPA displacement from ITaskListUI: 0x%X", offset);
    return true;
}

// GetWindowLongPtr(...,0) is CImpWndProc. Look only forward inside the known
// object's small interface prefix, not backwards into neighbouring allocations.
// The first match must also pass the DPA, thread and primary/secondary checks.
void* TaskListUI(HWND list) {
    if (!IsOurWindow(list, GetCurrentThreadId()) ||
        !IsClass(list, L"MSTaskListWClass")) {
        return nullptr;
    }
    auto* object = reinterpret_cast<BYTE*>(GetWindowLongPtrW(list, 0));
    if (!HasVtable(object, g_taskListWindowVtable)) {
        return nullptr;
    }
    for (size_t offset = 0; offset < 0x80; offset += sizeof(void*)) {
        if (HasVtable(object + offset, g_taskListUIVtable)) {
            return object + offset;
        }
    }
    return nullptr;
}

using ObjectRef = std::shared_ptr<IUnknown>;

ObjectRef Retain(void* object) {
    auto* unknown = static_cast<IUnknown*>(object);
    unknown->AddRef();
    return ObjectRef(unknown, [](IUnknown* p) { p->Release(); });
}

struct Button {
    ObjectRef group;
    std::wstring appId;
    bool pinned;
};

struct Taskbar {
    HWND tray;
    HWND list;
    bool primary;
};

struct Snapshot {
    ObjectRef ui;
    std::vector<Button> buttons;
};

bool CopyAppId(PCWSTR value, std::wstring& result) {
    result.clear();
    if (!value) {
        return true;  // Pointer identity can still match a group without an ID.
    }
    for (size_t i = 0; i <= kMaxAppId; ++i) {
        if (!Readable(value + i, sizeof(WCHAR))) {
            return false;
        }
        if (!value[i]) {
            return true;
        }
        if (i == kMaxAppId) {
            return false;
        }
        result.push_back(value[i]);
    }
    return false;
}

bool SameApp(const Button& a, const Button& b) {
    // Do not strip suffixes, trim IDs, resolve EXEs or use window/display names.
    // Windows-generated identities and profile-specific IDs remain distinct.
    return a.group.get() == b.group.get() ||
           (!a.appId.empty() && !b.appId.empty() &&
            CompareStringOrdinal(a.appId.c_str(), static_cast<int>(a.appId.size()),
                                 b.appId.c_str(), static_cast<int>(b.appId.size()),
                                 TRUE) == CSTR_EQUAL);
}

bool ReadTaskbar(const Taskbar& taskbar, Snapshot& snapshot) {
    snapshot = {};
    if (!IsOurWindow(taskbar.tray, GetCurrentThreadId()) ||
        !IsClass(taskbar.tray, taskbar.primary ? L"Shell_TrayWnd"
                                             : L"Shell_SecondaryTrayWnd") ||
        GetAncestor(taskbar.list, GA_ROOT) != taskbar.tray) {
        return false;
    }
    void* ui = TaskListUI(taskbar.list);
    if (!ui || !!g_isPrimary(ui) != taskbar.primary) {
        return false;
    }
    snapshot.ui = Retain(ui);
    int count = g_getButtonGroupCount(ui);
    if (count < 0 || count > kMaxGroups) {
        return false;
    }
    auto** field = reinterpret_cast<void**>(static_cast<BYTE*>(ui) + g_buttonGroupsOffset);
    if (!Readable(field, sizeof(*field))) {
        return false;
    }
    void* dpa = *field;
    if (!dpa) {
        return count == 0;
    }
    // Public comctl32 DPA macros describe {int count; padding; void** items}.
    if (!Readable(dpa, 2 * sizeof(void*)) || *static_cast<int*>(dpa) != count) {
        return false;
    }
    auto** items = *reinterpret_cast<void***>(static_cast<BYTE*>(dpa) + sizeof(void*));
    if (count && !Readable(items, count * sizeof(void*))) {
        return false;
    }
    snapshot.buttons.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (!HasVtable(items[i], g_buttonGroupVtable)) {
            return false;
        }
        void* group = g_getGroup(items[i]);
        if (!HasVtable(group, g_taskGroupVtable)) {
            return false;
        }
        // Native combining/uncombining keeps one button group per task group.
        // Synthetic grouping mods may not: don't choose an ambiguous move target.
        for (const auto& previous : snapshot.buttons) {
            if (previous.group.get() == group) {
                return false;
            }
        }
        Button button{Retain(group), {}, (g_getFlags(group) & kPinnedFlag) != 0};
        if (!CopyAppId(g_getAppId(group), button.appId)) {
            return false;
        }
        snapshot.buttons.push_back(std::move(button));
    }
    return true;
}

std::vector<Taskbar> EnumerateTaskbars() {
    std::vector<Taskbar> result;
    EnumWindows([](HWND tray, LPARAM param) -> BOOL {
        if (!IsOurWindow(tray)) {
            return TRUE;
        }
        bool primary = IsClass(tray, L"Shell_TrayWnd");
        if (!primary && !IsClass(tray, L"Shell_SecondaryTrayWnd")) {
            return TRUE;
        }
        if (!IsOurWindow(tray, GetCurrentThreadId())) {
            StopSorting(L"Taskbars on separate native threads are not supported");
            return FALSE;
        }
        HWND host = primary ? static_cast<HWND>(GetPropW(tray, L"TaskbandHWND"))
                            : FindWindowExW(tray, nullptr, L"WorkerW", nullptr);
        HWND list = host ? FindWindowExW(host, nullptr, L"MSTaskListWClass", nullptr)
                         : nullptr;
        if (list) {
            auto& bars = *reinterpret_cast<std::vector<Taskbar>*>(param);
            Taskbar bar{tray, list, primary};
            if (primary) {
                bars.insert(bars.begin(), bar);
            } else {
                bars.push_back(bar);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

bool MonitorLocalMode() {
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    bool supported = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"MMTaskbarMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) == ERROR_SUCCESS &&
        (mode == 1 || mode == 2);
    if (!supported && !g_modeLogged) {
        Wh_Log(L"Waiting for monitor-local taskbar mode (MMTaskbarMode must be 1 or 2)");
    }
    g_modeLogged = !supported;
    return supported;
}

size_t PinRank(const Button& button, const std::vector<Button>& pins) {
    for (size_t i = 0; i < pins.size(); ++i) {
        if (SameApp(button, pins[i])) {
            return i;
        }
    }
    return pins.size();  // All unpinned entries compare equal; sort stays stable.
}

bool SameOrder(const Snapshot& a, const Snapshot& b) {
    if (a.ui.get() != b.ui.get() || a.buttons.size() != b.buttons.size()) {
        return false;
    }
    for (size_t i = 0; i < a.buttons.size(); ++i) {
        if (a.buttons[i].group.get() != b.buttons[i].group.get() ||
            a.buttons[i].pinned != b.buttons[i].pinned ||
            a.buttons[i].appId != b.buttons[i].appId) {
            return false;
        }
    }
    return true;
}

enum class SyncResult { Done, Retry, MoreWork };

SyncResult Synchronize() {
    if (g_unloading || g_faulted || !MonitorLocalMode()) {
        return SyncResult::Done;
    }
    const ULONG64 epoch = g_epoch;
    auto bars = EnumerateTaskbars();
    if (g_faulted) {
        return SyncResult::Done;
    }
    if (bars.empty() || !bars.front().primary) {
        return SyncResult::Retry;
    }
    Snapshot primary;
    if (!ReadTaskbar(bars.front(), primary)) {
        if (!g_readFailureLogged) {
            Wh_Log(L"Cannot read primary taskbar layout; no secondary moves attempted");
            g_readFailureLogged = true;
        }
        return SyncResult::Retry;
    }
    g_readFailureLogged = false;
    std::vector<Button> pins;
    for (const auto& button : primary.buttons) {
        if (button.pinned) {
            pins.push_back(button);
        }
    }

    const ULONGLONG start = GetTickCount64();
    bool madeProgress = false;
    bool retry = false;
    for (const auto& bar : bars) {
        if (bar.primary) {
            continue;
        }
        // A stable sort is realized using leftward moves of PINNED groups only.
        // Thus unpinned groups never cross each other; windows inside a group
        // aren't enumerated or modified at all.
        for (int moves = 0; moves < kMaxGroups; ++moves) {
            if (g_unloading || g_faulted) {
                return SyncResult::Done;
            }
            if (g_epoch != epoch) {
                return SyncResult::Retry;
            }
            if (madeProgress && GetTickCount64() - start > 30) {
                return SyncResult::MoreWork;
            }
            Snapshot current;
            if (!ReadTaskbar(bar, current)) {
                Wh_Log(L"Secondary taskbar %p unavailable or has unsupported groups", bar.tray);
                retry = true;
                break;
            }
            std::vector<size_t> ranks;
            ranks.reserve(current.buttons.size());
            for (const auto& button : current.buttons) {
                ranks.push_back(PinRank(button, pins));
            }
            auto [from, to] = SecondaryTaskbarOrder::NextMove(ranks);
            if (to == current.buttons.size()) {
                break;
            }
            if (from <= to || PinRank(current.buttons[from], pins) == pins.size()) {
                StopSorting(L"Ordering plan did not select a leftward pinned-group move");
                return SyncResult::Done;
            }

            // Revalidate the source of authority immediately before each move.
            Snapshot before;
            if (!ReadTaskbar(bars.front(), before) || !SameOrder(primary, before) ||
                g_epoch != epoch || TaskListUI(bar.list) != current.ui.get() ||
                g_isPrimary(current.ui.get())) {
                return SyncResult::Retry;
            }
            if (g_unloading || g_faulted) {
                return SyncResult::Done;
            }
            void* group = current.buttons[from].group.get();
            t_automaticMoveTarget = current.ui.get();
            // This is the local ITaskListUI move, NOT the task-band drag/pin
            // manager. Explorer updates its DPA and emits the UI notification.
            // We don't edit the array ourselves or invoke SetRelativeTaskOrder,
            // which can create a missing group's launcher on some builds.
            bool moved = g_moveGroupOriginal(current.ui.get(), group, static_cast<UINT>(to));
            t_automaticMoveTarget = nullptr;
            if (g_unloading || g_faulted) {
                return SyncResult::Done;
            }
            if (g_epoch != epoch) {
                // A group/monitor changed during a reentrant native call. All
                // group and UI refs are retained until these snapshots expire.
                return SyncResult::Retry;
            }
            Snapshot primaryAfter;
            if (!ReadTaskbar(bars.front(), primaryAfter)) {
                return SyncResult::Retry;
            }
            if (!SameOrder(primary, primaryAfter)) {
                StopSorting(L"Primary order changed during a local move; unsupported native behaviour");
                return SyncResult::Done;
            }
            std::rotate(current.buttons.begin() + to,
                        current.buttons.begin() + from,
                        current.buttons.begin() + from + 1);
            Snapshot after;
            if (!ReadTaskbar(bar, after)) {
                return SyncResult::Retry;
            }
            if (!moved || !SameOrder(current, after)) {
                StopSorting(L"TryMoveGroup refused the move or produced an unexpected order");
                return SyncResult::Done;
            }
            Wh_Log(L"Secondary %p: group %p moved %zu -> %zu (%zu main pins)",
                   bar.tray, group, from, to, pins.size());
            madeProgress = true;
        }
    }
    return retry ? SyncResult::Retry : SyncResult::Done;
}

void ArmTimer(HWND window, UINT delay) {
    if (g_unloading || g_faulted) {
        return;
    }
    g_timerArmed = SetTimer(window, TimerId(), delay, nullptr) != 0;
    if (!g_timerArmed) {
        Wh_Log(L"Cannot schedule taskbar sync: %lu", GetLastError());
    }
}

LRESULT CALLBACK TraySubclass(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam, DWORD_PTR cookie) {
    struct ActiveCall {
        ActiveCall() { ++g_subclassCalls; }
        ~ActiveCall() { --g_subclassCalls; }
    } activeCall;
    if (message == WM_NCDESTROY) {
        KillTimer(window, TimerId());
        g_timerArmed = false;
        HWND expected = window;
        g_dispatchWindow.compare_exchange_strong(expected, nullptr);
        ++g_epoch;
        Wake();
        // WindhawkUtils removes its subclass wrapper on WM_NCDESTROY.
    } else if (message == g_cleanupMessage && wParam == cookie) {
        KillTimer(window, TimerId());
        g_timerArmed = false;
        g_attemptsLeft = 0;
        return 0;
    } else if (message == g_syncMessage && wParam == cookie) {
        if (g_unloading) {
            return 0;
        }
        // Publish on the HWND's own thread. Publishing from the worker after
        // SetWindowSubclass returns could resurrect a handle destroyed meanwhile.
        g_dispatchWindow = window;
        g_attemptsLeft = kSettleAttempts;
        ArmTimer(window, kSettleMs);
        return 0;
    } else if (message == WM_TIMER && wParam == TimerId()) {
        if (!g_timerArmed) {
            return 0;  // A timer message queued before KillTimer.
        }
        KillTimer(window, TimerId());
        g_timerArmed = false;
        if (g_unloading || g_faulted || g_attemptsLeft <= 0) {
            return 0;
        }
        if (t_nativeDepth || t_sorting || GetCapture() ||
            GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0) {
            ArmTimer(window, kRetryMs);  // Only while an event is pending.
            return 0;
        }
        --g_attemptsLeft;
        t_sorting = true;
        SyncResult result = SyncResult::Done;
        try {
            result = Synchronize();
        } catch (...) {
            // Allocation failures must not escape across Explorer's WndProc.
            t_automaticMoveTarget = nullptr;
            StopSorting(L"Exception while preparing or applying an ordering pass");
        }
        t_sorting = false;
        if (result == SyncResult::MoreWork) {
            // A large taskbar is finished in bounded chunks. Successful moves
            // are progress, not failed initialization attempts.
            g_attemptsLeft = kSettleAttempts;
            ArmTimer(window, kSettleMs);
        } else if (result == SyncResult::Retry && g_attemptsLeft > 0) {
            ArmTimer(window, kRetryMs);
        } else if (result == SyncResult::Retry) {
            Wh_Log(L"Sync deferred until the next native taskbar event");
        }
        return 0;
    }
    LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE ||
        message == WM_DEVICECHANGE) {
        ++g_epoch;
        Wake();
    }
    return result;
}

void DetachDispatcher(HWND installed, DWORD_PTR cookie) {
    if (installed && IsOurWindow(installed) && IsClass(installed, L"Shell_TrayWnd")) {
        // Synchronous removal is intentional: a timeout followed by unloading
        // could leave a window callback pointing into an unloaded mod DLL.
        SendMessageW(installed, g_cleanupMessage, cookie, 0);
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(installed, TraySubclass);
    }
    // Removal can run inside a nested SendMessage while an outer timer callback
    // is still sorting. Drain that callback before attaching to a replacement
    // taskbar (possibly on another thread) or allowing Windhawk to unload us.
    while (g_subclassCalls) {
        Sleep(1);
    }
    if (installed && IsOurWindow(installed)) {
        SendMessageW(installed, WM_NULL, 0, 0);
    }
}

// The worker only discovers HWNDs, installs/removes the dispatcher subclass and
// posts a cookie-tagged message. It NEVER reads taskbar objects or calls methods
// on them. An auto-reset event coalesces bursts; idle time has no polling.
DWORD WINAPI DispatchWorker(void*) {
    DWORD_PTR cookie = static_cast<DWORD_PTR>(GetTickCount64());
    // Track installation independently of the callback's published live HWND:
    // shutdown may begin between installing the subclass and its first message.
    HWND attached = nullptr;
    HANDLE events[] = {g_stopEvent, g_wakeEvent};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1 || g_unloading) {
            break;
        }
        HWND tray = FindPrimaryTray();
        if (attached && (attached != tray || g_dispatchWindow != attached)) {
            DetachDispatcher(attached, cookie);
            attached = nullptr;
            g_dispatchWindow = nullptr;
        }
        if (!tray) {
            continue;  // Creation hooks will wake us when Explorer is ready.
        }
        if (!attached) {
            // WM_NCDESTROY might have cleared the HWND while its callback is
            // still unwinding. It must finish before the replacement uses state.
            DetachDispatcher(nullptr, cookie);
            ++cookie;
            if (!WindhawkUtils::SetWindowSubclassFromAnyThread(tray, TraySubclass, cookie)) {
                Wh_Log(L"Cannot attach taskbar dispatcher: %lu", GetLastError());
                continue;
            }
            attached = tray;
            SendMessageW(tray, g_syncMessage, cookie, 0);
            Wh_Log(L"Dispatcher attached to primary HWND %p, thread %lu", tray,
                   GetWindowThreadProcessId(tray, nullptr));
        }
        if (!g_unloading && !PostMessageW(tray, g_syncMessage, cookie, 0)) {
            Wh_Log(L"Cannot post taskbar sync: %lu", GetLastError());
        }
    }
    g_dispatchWindow = nullptr;
    DetachDispatcher(attached, cookie);
    return 0;
}

void* WINAPI CreateGroupHook(void* self, void* group, int index) {
    NativeMutation mutation;
    return g_createGroupOriginal(self, group, index);
}

bool WINAPI MoveGroupHook(void* self, void* group, UINT index) {
    if (t_automaticMoveTarget && self != t_automaticMoveTarget) {
        StopSorting(L"A local move attempted to propagate to another taskbar");
        return false;
    }
    NativeMutation mutation;
    return g_moveGroupOriginal(self, group, index);
}

HRESULT WINAPI TaskCreatedHook(void* self, void* group, void* item) {
    NativeMutation mutation;
    return g_taskCreatedOriginal(self, group, item);
}

HRESULT WINAPI TaskDestroyedHook(void* self, void* group, void* item) {
    NativeMutation mutation;
    return g_taskDestroyedOriginal(self, group, item);
}

HRESULT WINAPI InclusionChangedHook(void* self, void* group, void* item) {
    NativeMutation mutation;
    return g_inclusionChangedOriginal(self, group, item);
}

void WINAPI PinnedHook(void* self, void* group) {
    NativeMutation mutation;
    g_pinnedOriginal(self, group);
}

void WINAPI UnpinnedHook(void* self, void* group) {
    NativeMutation mutation;
    g_unpinnedOriginal(self, group);
}

void WINAPI SetOrderHook(void* self, void* group, int index) {
    if (t_automaticMoveTarget && self != t_automaticMoveTarget) {
        StopSorting(L"A local move attempted to set another taskbar's order");
        return;
    }
    NativeMutation mutation;
    g_setOrderOriginal(self, group, index);
}

HRESULT WINAPI UpdateFlagsHook(void* self, DWORD mask, DWORD value) {
    if (!(mask & kPinnedFlag)) {
        return g_updateFlagsOriginal(self, mask, value);
    }
    NativeMutation mutation;
    return g_updateFlagsOriginal(self, mask, value);
}

HWND WINAPI CreateWindowHook(DWORD exStyle, LPCWSTR className, LPCWSTR name,
                            DWORD style, int x, int y, int width, int height,
                            HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    HWND window = g_createWindowOriginal(exStyle, className, name, style, x, y,
                                        width, height, parent, menu, instance, param);
    // Inspect the resulting class rather than assuming className isn't an atom.
    if (window && (IsClass(window, L"Shell_TrayWnd") ||
                   IsClass(window, L"Shell_SecondaryTrayWnd") ||
                   IsClass(window, L"MSTaskListWClass"))) {
        ++g_epoch;
        Wake();
    }
    return window;
}

bool SupportedWindows() {
    auto rtlGetVersion = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtlGetVersion || rtlGetVersion(&version) != 0) {
        Wh_Log(L"Cannot determine Windows build");
        return false;
    }
    Wh_Log(L"Windows %lu.%lu build %lu; native x64 taskbar required",
           version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    return version.dwMajorVersion == 10 &&
           (version.dwBuildNumber == 26100 || version.dwBuildNumber == 26200);
}

bool ResolveSymbols() {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskListWnd::`vftable'{for `ITaskListUI'})"}, &g_taskListUIVtable},
        {{LR"(const CTaskListWnd::`vftable'{for `CImpWndProc'})"}, &g_taskListWindowVtable},
        {{LR"(const CTaskBtnGroup::`vftable'{for `ITaskBtnGroup'})"}, &g_buttonGroupVtable},
        {{LR"(const CTaskGroup::`vftable'{for `ITaskGroup'})"}, &g_taskGroupVtable},
        {{LR"(public: virtual int __cdecl CTaskListWnd::IsOnPrimaryTaskband(void))"}, &g_isPrimary},
        {{LR"(public: virtual int __cdecl CTaskListWnd::GetButtonGroupCount(void))"}, &g_getButtonGroupCount},
        {{LR"(public: virtual struct ITaskGroup * __cdecl CTaskBtnGroup::GetGroup(void))"}, &g_getGroup},
        {{LR"(public: virtual unsigned short const * __cdecl CTaskGroup::GetAppID(void))"}, &g_getAppId},
        {{LR"(public: virtual unsigned long __cdecl CTaskGroup::GetFlags(void)const )"}, &g_getFlags},
        {{LR"(protected: struct ITaskBtnGroup * __cdecl CTaskListWnd::_CreateTBGroup(struct ITaskGroup *,int))"},
         &g_createGroupOriginal, CreateGroupHook},
        {{LR"(public: virtual bool __cdecl CTaskListWnd::TryMoveGroup(struct ITaskGroup *,unsigned int))"},
         &g_moveGroupOriginal, MoveGroupHook},
        {{LR"(public: virtual long __cdecl CTaskListWnd::TaskCreated(struct ITaskGroup *,struct ITaskItem *))"},
         &g_taskCreatedOriginal, TaskCreatedHook},
        {{LR"(public: virtual long __cdecl CTaskListWnd::TaskDestroyed(struct ITaskGroup *,struct ITaskItem *))"},
         &g_taskDestroyedOriginal, TaskDestroyedHook},
        {{LR"(public: virtual long __cdecl CTaskListWnd::TaskInclusionChanged(struct ITaskGroup *,struct ITaskItem *))"},
         &g_inclusionChangedOriginal, InclusionChangedHook},
        {{LR"(public: virtual void __cdecl CTaskListWnd::HandleTaskGroupPinned(struct ITaskGroup *))"},
         &g_pinnedOriginal, PinnedHook},
        {{LR"(public: virtual void __cdecl CTaskListWnd::HandleTaskGroupUnpinned(struct ITaskGroup *))"},
         &g_unpinnedOriginal, UnpinnedHook},
        {{LR"(public: virtual void __cdecl CTaskListWnd::SetRelativeTaskOrder(struct ITaskGroup *,int))"},
         &g_setOrderOriginal, SetOrderHook},
        {{LR"(public: virtual long __cdecl CTaskGroup::UpdateFlags(unsigned long,unsigned long))"},
         &g_updateFlagsOriginal, UpdateFlagsHook},
    };
    return WindhawkUtils::HookSymbols(g_taskbarModule, hooks, ARRAYSIZE(hooks)) &&
           ResolveDpaOffset();
}

void CloseResources() {
    if (g_worker) {
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_taskbarModule) {
        FreeLibrary(g_taskbarModule);
        g_taskbarModule = nullptr;
    }
}

}  // namespace

BOOL Wh_ModInit() {
    if (!SupportedWindows()) {
        Wh_Log(L"Unsupported Windows build; intended targets are 26100 and 26200");
        return FALSE;
    }
    g_taskbarModule = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_taskbarModule || !ResolveSymbols()) {
        Wh_Log(L"Required taskbar symbols/layout unavailable; initialization refused");
        CloseResources();
        return FALSE;
    }
    g_syncMessage = RegisterWindowMessageW(L"Windhawk.SecondaryTaskbarPinnedOrder.Sync.v1");
    g_cleanupMessage = RegisterWindowMessageW(L"Windhawk.SecondaryTaskbarPinnedOrder.Cleanup.v1");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_syncMessage || !g_cleanupMessage || !g_stopEvent || !g_wakeEvent ||
        !WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowHook, &g_createWindowOriginal)) {
        Wh_Log(L"Cannot initialize taskbar dispatcher: %lu", GetLastError());
        CloseResources();
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    g_worker = CreateThread(nullptr, 0, DispatchWorker, nullptr, 0, nullptr);
    if (!g_worker) {
        StopSorting(L"Cannot start taskbar dispatcher thread");
        return;
    }
    Wake();  // Sort already-open windows; no new window event is required.
}

void Wh_ModBeforeUninit() {
    g_unloading = true;
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_worker) {
        // Worker removes the subclass and its timer synchronously while the
        // original function trampolines still exist. No callback survives this.
        WaitForSingleObject(g_worker, INFINITE);
    }
}

void Wh_ModUninit() {
    CloseResources();
}

#endif  // SECONDARY_TASKBAR_ORDER_PLANNER_ONLY
