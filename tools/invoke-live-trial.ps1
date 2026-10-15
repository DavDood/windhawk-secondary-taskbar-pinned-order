#requires -Version 7.0
# Copyright (c) 2026 DavDood. GPL-3.0-only; see ../LICENSE.
# Explicitly approved, administrator-only live trial. Uses Windhawk 1.7.3's
# normal source/library/registry registration; never injects a DLL directly.
# No Explorer restart, window movement, simulated input, or pin writes.
[CmdletBinding()]
param(
    [ValidateSet('InstallAndTrial', 'Disable')][string]$Action = 'InstallAndTrial',
    [Parameter(Mandatory = $true)][string]$OwnerSid,
    [switch]$ReuseInstalled,
    [ValidateRange(10, 180)][int]$WaitSeconds = 90
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($identity.User.Value -ne $OwnerSid) { throw 'Elevation changed the user; refusing to use another user profile.' }
if (-not [Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This trial requires administrator rights for Windhawk registration.'
}
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$artifacts = Join-Path $root 'build\live-trial'
if (-not (Test-Path -LiteralPath $artifacts -PathType Container)) { throw 'Create the local trial artifact directory first.' }
$modId = 'local@secondary-taskbar-pinned-order'
$registryPath = "SOFTWARE\Windhawk\Engine\Mods\$modId"
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$resultPath = Join-Path $artifacts "result-$stamp.json"
$result = [ordered]@{ action = $Action; started = [DateTime]::UtcNow.ToString('o'); modId = $modId; enabled = $false }
$logger = $null
$registered = $false

function Set-ModDword([string]$Name, [int]$Value) {
    $key = $registry.OpenSubKey($registryPath, $true)
    if ($null -eq $key) { throw 'The local mod is not registered.' }
    try { $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::DWord); $key.Flush() }
    finally { $key.Dispose() }
}

function Capture([string]$Label) {
    $file = Join-Path $artifacts "$Label-$stamp.json"
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot 'capture-taskbar-state.ps1') -OutputPath $file
    if ($LASTEXITCODE -ne 0) { throw "Taskbar snapshot failed: $Label" }
    return $file
}

function Read-LiveLog([string]$File) {
    $stream = [IO.FileStream]::new($File, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function AppButtonIds($Snapshot, [bool]$Primary) {
    $class = if ($Primary) { 'Shell_TrayWnd' } else { 'Shell_SecondaryTrayWnd' }
    foreach ($bar in @($Snapshot.taskbars | Where-Object { $_.class -eq $class })) {
        @($bar.elements | Where-Object { $_.class -eq 'Taskbar.TaskListButtonAutomationPeer' } |
            Sort-Object { $_.bounds[0] } | ForEach-Object { $_.automationId })
    }
}

try {
    if ($Action -eq 'Disable') {
        Set-ModDword 'Disabled' 1
        Set-ModDword 'LoggingEnabled' 0
        Set-ModDword 'DebugLoggingEnabled' 0
        $result.outcome = 'disabled'
    } else {
        $existing = $registry.OpenSubKey($registryPath, $false)
        $existingName = $null
        if ($null -ne $existing) {
            try {
                if (-not $ReuseInstalled) { throw 'A local mod with this ID already exists; refusing to replace it.' }
                if ($existing.GetValue('Disabled', 0) -ne 1) { throw 'Reuse requires the existing local mod to be disabled.' }
                $existingName = [string]$existing.GetValue('LibraryFileName')
                if (-not $existingName.StartsWith("${modId}_") -or [IO.Path]::GetFileName($existingName) -ne $existingName) {
                    throw 'Unexpected existing local DLL name.'
                }
            } finally { $existing.Dispose() }
        } elseif ($ReuseInstalled) { throw 'No installed local mod is available to reuse.' }
        if (Get-Process -Name DbgViewMini -ErrorAction SilentlyContinue) {
            throw 'An existing Windhawk log collector is running; do not compete with it.'
        }
        $source = Join-Path $root 'src\secondary-taskbar-pinned-order.wh.cpp'
        $sourceText = Get-Content -LiteralPath $source -Raw
        if ($sourceText -notmatch '(?m)^// @id\s+secondary-taskbar-pinned-order\s*$') { throw 'Unexpected source mod ID.' }
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $version = '0.1.0'
        $name = if ($ReuseInstalled) { $existingName } else { "${modId}_${version}_$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()).dll" }
        $stagedDll = Join-Path $artifacts $name
        $compiler = 'C:\Program Files\Windhawk\Compiler\bin\clang++.exe'
        $engineLib = 'C:\Program Files\Windhawk\Engine\1.7.3\64\windhawk.lib'
        $mods = 'C:\ProgramData\Windhawk\Engine\Mods\64'
        $sources = 'C:\ProgramData\Windhawk\ModsSource'
        $installedDll = Join-Path $mods $name
        $installedSource = Join-Path $sources "$modId.wh.cpp"
        foreach ($folder in @($mods, $sources)) {
            if (-not (Test-Path -LiteralPath $folder -PathType Container)) { throw "Windhawk directory missing: $folder" }
        }
        if ($ReuseInstalled) {
            if ((Get-FileHash -LiteralPath $installedSource -Algorithm SHA256).Hash -cne $sourceHash) {
                throw 'Installed source differs from the repository; refusing to reuse its DLL.'
            }
        } elseif (Test-Path -LiteralPath $installedSource) { throw 'Existing local source must be investigated before replacement.' }
        foreach ($runtime in @('libc++.whl', 'libunwind.whl', 'windhawk-mod-shim.dll')) {
            if (-not (Test-Path -LiteralPath (Join-Path $mods $runtime) -PathType Leaf)) { throw "Windhawk runtime missing: $runtime" }
        }
        $flags = @(
            '-std=c++23', '-O2', '-shared', '-target', 'x86_64-w64-mingw32',
            '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
            '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0',
            '-DWH_MOD', '-DWH_MOD_ID=L"local@secondary-taskbar-pinned-order"', '-DWH_MOD_VERSION=L"0.1.0"',
            '-include', 'windhawk_api.h', '-mguard=cf-nochecks', '-Wl,--export-all-symbols',
            $source, $engineLib, '-lcomctl32', '-ladvapi32', '-o', $stagedDll
        )
        if (-not $ReuseInstalled) {
            & $compiler @flags 2>&1 | Tee-Object -FilePath (Join-Path $artifacts "compile-$stamp.log")
            if ($LASTEXITCODE -ne 0) { throw 'Local-mod compilation failed.' }
        }
        $result.sourceSha256 = $sourceHash
        $result.libraryFileName = $name
        $result.dllSha256 = (Get-FileHash -LiteralPath $(if ($ReuseInstalled) { $installedDll } else { $stagedDll }) -Algorithm SHA256).Hash

        # Capture a fresh baseline immediately before installation.
        $beforePath = @(Capture 'before-enable')[-1]
        $before = Get-Content -LiteralPath $beforePath -Raw | ConvertFrom-Json
        if (@($before.taskbars).Count -lt 2) { throw 'No secondary taskbar is available for this trial.' }
        if (@($before.taskbars | Where-Object { $_.automationError }).Count) { throw 'UI Automation baseline failed.' }
        $explorerId = [int]$before.taskbars[0].process
        $result.explorerId = $explorerId
        $result.before = $beforePath

        if (-not $ReuseInstalled) {
            Copy-Item -LiteralPath $stagedDll -Destination $installedDll -ErrorAction Stop
            Copy-Item -LiteralPath $source -Destination $installedSource -ErrorAction Stop
        }
        $key = $registry.CreateSubKey($registryPath)
        $registered = $true
        try {
            $key.SetValue('Disabled', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
            $key.SetValue('Include', 'explorer.exe', [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('Exclude', '', [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('Architecture', 'x86-64', [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('Version', $version, [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('LoggingEnabled', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
            $key.SetValue('DebugLoggingEnabled', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
            $settings = $key.CreateSubKey('Settings'); $settings.Dispose()
            $key.SetValue('SettingsChangeTime', [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() -band 0x7fffffff), [Microsoft.Win32.RegistryValueKind]::DWord)
            $key.SetValue('LibraryFileName', $name, [Microsoft.Win32.RegistryValueKind]::String)
            $key.Flush()
        } finally { $key.Dispose() }

        $log = Join-Path $artifacts "windhawk-$stamp.log"
        $loggerError = Join-Path $artifacts "logger-$stamp.stderr.log"
        $result.log = $log
        $logger = Start-Process -FilePath 'C:\Program Files\Windhawk\UI\resources\app\extensions\windhawk\files\DbgViewMini.exe' `
            -ArgumentList @('--pattern', '"[WH] *"', '--no-buffering') -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError $loggerError
        Start-Sleep -Milliseconds 750
        if ($logger.HasExited) { throw 'Windhawk log collector exited before activation.' }
        Set-ModDword 'Disabled' 0
        $result.enabled = $true
        $result.enabledAt = [DateTime]::UtcNow.ToString('o')
        $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
        $readyAt = $null
        while ([DateTime]::UtcNow -lt $deadline) {
            if (-not (Get-Process -Id $explorerId -ErrorAction SilentlyContinue)) { throw 'The original Explorer process exited during activation.' }
            if ($logger.HasExited) { throw 'The log collector exited during activation.' }
            $text = Read-LiveLog $log
            if ($text -match 'Sorting suspended until reload|initialization refused|Unsupported GetButtonGroupCount|Invalid button-group DPA|Cannot read primary taskbar|Secondary taskbar .*unsupported groups|Cannot start taskbar dispatcher') {
                throw 'A mod validation guard reported a failure; see the captured log.'
            }
            if ($null -eq $readyAt -and $text -match 'Dispatcher attached to primary HWND') { $readyAt = [DateTime]::UtcNow }
            if ($null -ne $readyAt -and ([DateTime]::UtcNow - $readyAt).TotalSeconds -ge 3) { break }
            Start-Sleep -Milliseconds 250
        }
        if ($null -eq $readyAt) { throw 'No successful dispatcher activation was logged before the trial timeout.' }

        $afterPath = @(Capture 'after-enable')[-1]
        $after = Get-Content -LiteralPath $afterPath -Raw | ConvertFrom-Json
        $result.after = $afterPath
        $result.taskbandUnchanged = ($before.taskband | ConvertTo-Json -Depth 16 -Compress) -ceq ($after.taskband | ConvertTo-Json -Depth 16 -Compress)
        $result.pinFilesUnchanged = ($before.shortcuts | ConvertTo-Json -Depth 16 -Compress) -ceq ($after.shortcuts | ConvertTo-Json -Depth 16 -Compress)
        $result.primaryButtonsUnchanged = (@(AppButtonIds $before $true) | ConvertTo-Json -Compress) -ceq (@(AppButtonIds $after $true) | ConvertTo-Json -Compress)
        $result.secondaryMembershipUnchanged = (@(AppButtonIds $before $false) | Sort-Object | ConvertTo-Json -Compress) -ceq (@(AppButtonIds $after $false) | Sort-Object | ConvertTo-Json -Compress)
        $result.secondaryOrderChanged = (@(AppButtonIds $before $false) | ConvertTo-Json -Compress) -cne (@(AppButtonIds $after $false) | ConvertTo-Json -Compress)
        if (-not $result.taskbandUnchanged -or -not $result.pinFilesUnchanged -or -not $result.primaryButtonsUnchanged -or -not $result.secondaryMembershipUnchanged) {
            throw 'Baseline invariants changed during the trial; disabling for investigation.'
        }
        $result.outcome = 'loaded-baseline-invariants-preserved'
        # End diagnostic verbosity without disabling a successful initial trial.
        Set-ModDword 'DebugLoggingEnabled' 0
        Set-ModDword 'LoggingEnabled' 0
    }
} catch {
    $result.outcome = 'failed'
    $result.error = $_.Exception.Message
    if ($registered) {
        try {
            Set-ModDword 'Disabled' 1
            Set-ModDword 'DebugLoggingEnabled' 0
            Set-ModDword 'LoggingEnabled' 0
            $result.enabled = $false
            Start-Sleep -Seconds 2
        } catch { $result.disableError = $_.Exception.Message }
    }
} finally {
    if ($null -ne $logger -and -not $logger.HasExited) {
        $logger.Kill()
        $logger.WaitForExit()
    }
    $result.finished = [DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resultPath -Encoding utf8
    $registry.Dispose()
    $result | ConvertTo-Json -Depth 10
}
if ($result.outcome -eq 'failed') { exit 1 }
