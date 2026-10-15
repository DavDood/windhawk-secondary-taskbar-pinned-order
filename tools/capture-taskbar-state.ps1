# Copyright (c) 2026 DavDood. GPL-3.0-only; see ../LICENSE.
# Read-only live diagnostic; requires the owner's permission.
# Uses Windows PowerShell 5.1 for the installed UI Automation framework assemblies.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputPath)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class TaskbarSnapshotNative {
    public delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rectangle);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct MonitorInfo {
        public int Size;
        public Rect Monitor, Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Device;
    }
    public static IntPtr[] Taskbars() {
        var windows = new List<IntPtr>();
        EnumWindows(delegate(IntPtr w, IntPtr p) {
            var name = new StringBuilder(128);
            GetClassName(w, name, name.Capacity);
            if (name.ToString() == "Shell_TrayWnd" || name.ToString() == "Shell_SecondaryTrayWnd") windows.Add(w);
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
}
'@

function Read-RegistryValues([string]$SubKey) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey, $false)
    $values = [ordered]@{}
    if ($null -eq $key) { return $values }
    try {
        foreach ($name in ($key.GetValueNames() | Sort-Object)) {
            $value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($value -is [byte[]]) { $value = [Convert]::ToBase64String($value) }
            $values[$name] = [ordered]@{ kind = $key.GetValueKind($name).ToString(); value = $value }
        }
    } finally { $key.Dispose() }
    return $values
}

$bars = @()
foreach ($window in [TaskbarSnapshotNative]::Taskbars()) {
    $class = New-Object Text.StringBuilder 128
    [void][TaskbarSnapshotNative]::GetClassName($window, $class, $class.Capacity)
    $process = [uint32]0
    $thread = [TaskbarSnapshotNative]::GetWindowThreadProcessId($window, [ref]$process)
    $rect = New-Object TaskbarSnapshotNative+Rect
    [void][TaskbarSnapshotNative]::GetWindowRect($window, [ref]$rect)
    $monitor = New-Object TaskbarSnapshotNative+MonitorInfo
    $monitor.Size = [Runtime.InteropServices.Marshal]::SizeOf($monitor)
    [void][TaskbarSnapshotNative]::GetMonitorInfo([TaskbarSnapshotNative]::MonitorFromWindow($window, 2), [ref]$monitor)
    $elements = @()
    $automationError = $null
    try {
        $root = [Windows.Automation.AutomationElement]::FromHandle($window)
        $descendants = $root.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
        foreach ($element in $descendants) {
            try {
                $info = $element.Current
                $r = $info.BoundingRectangle
                $elements += [ordered]@{
                    name = $info.Name; class = $info.ClassName; automationId = $info.AutomationId
                    type = $info.ControlType.ProgrammaticName; offscreen = $info.IsOffscreen
                    bounds = @($r.X, $r.Y, $r.Width, $r.Height)
                }
            } catch { $elements += @{ error = $_.Exception.Message } }
        }
    } catch { $automationError = $_.Exception.Message }
    $bars += [ordered]@{
        handle = $window.ToInt64(); class = $class.ToString(); process = $process; thread = $thread
        bounds = @($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
        monitor = $monitor.Device; monitorPrimary = [bool]($monitor.Flags -band 1)
        monitorBounds = @($monitor.Monitor.Left, $monitor.Monitor.Top, $monitor.Monitor.Right, $monitor.Monitor.Bottom)
        elements = $elements; automationError = $automationError
    }
}

$pinsPath = Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
$shortcuts = @()
if (Test-Path -LiteralPath $pinsPath) {
    foreach ($file in (Get-ChildItem -LiteralPath $pinsPath -File | Sort-Object Name)) {
        $shortcuts += [ordered]@{ name = $file.Name; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
}
$advanced = Read-RegistryValues 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
$settings = [ordered]@{}
foreach ($name in @('MMTaskbarEnabled', 'MMTaskbarMode', 'TaskbarGlomLevel', 'MMTaskbarGlomLevel', 'VirtualDesktopAltTabFilter', 'VirtualDesktopTaskbarFilter')) {
    $settings[$name] = $advanced[$name]
}
$snapshot = [ordered]@{
    captured = [DateTime]::UtcNow.ToString('o')
    taskbars = $bars
    taskband = Read-RegistryValues 'Software\Microsoft\Windows\CurrentVersion\Explorer\Taskband'
    shortcuts = $shortcuts
    settings = $settings
}
$parent = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Snapshot output directory is missing: $parent" }
if (Test-Path -LiteralPath $OutputPath) { throw "Refusing to overwrite snapshot: $OutputPath" }
$snapshot | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
"Captured $($bars.Count) taskbars and $($shortcuts.Count) pin files to $OutputPath"
