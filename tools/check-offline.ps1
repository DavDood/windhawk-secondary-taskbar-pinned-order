# Copyright (c) 2026 DavDood. GPL-3.0-only; see ../LICENSE.
# Compilation is permitted; running tests needs permission.
# This script never invokes Windhawk itself or loads a mod DLL.
[CmdletBinding()]
param(
    [string]$WindhawkPath = "$env:ProgramFiles\Windhawk",
    [string]$EngineVersion = '1.7.3',
    [switch]$BuildDll,
    [switch]$BuildPlannerTests,
    [switch]$RunPlannerTests
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$compilerBin = Join-Path $WindhawkPath 'Compiler\bin'
$compiler = Join-Path $compilerBin 'clang++.exe'
$source = Join-Path $root 'src\secondary-taskbar-pinned-order.wh.cpp'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "Windhawk compiler not found: $compiler"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Mod source not found: $source"
}

$common = @('-std=c++23', '-target', 'x86_64-w64-mingw32', '-Wall', '-Wextra')
$modFlags = @(
    '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008',
    '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-DWH_MOD_ID=L"secondary-taskbar-pinned-order"', '-DWH_MOD_VERSION=L"0.1.0"',
    '-include', 'windhawk_api.h',
    '-Wno-pragma-pack', '-Wno-pragma-system-header-outside-header'
)

'Checking full mod syntax/types (real Windhawk API declarations, no DLL loading)...'
& $compiler @common @modFlags '-fsyntax-only' $source
if ($LASTEXITCODE -ne 0) { throw "Mod syntax check failed: exit $LASTEXITCODE" }

if ($BuildDll -or $BuildPlannerTests -or $RunPlannerTests) {
    $build = Join-Path $root 'build'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Build parent directory does not exist: $root"
    }
    if (-not (Test-Path -LiteralPath $build)) {
        New-Item -ItemType Directory -Path $build | Out-Null
    }
    if (-not (Test-Path -LiteralPath $build -PathType Container)) {
        throw "Build path is not a directory: $build"
    }
}

if ($BuildDll) {
    $engineLib = Join-Path $WindhawkPath "Engine\$EngineVersion\64\windhawk.lib"
    if (-not (Test-Path -LiteralPath $engineLib -PathType Leaf)) {
        throw "Windhawk engine import library not found: $engineLib"
    }
    $dll = Join-Path $build 'secondary-taskbar-pinned-order.dll'
    'Compiling/linking the mod DLL offline (the output will NOT be loaded)...'
    & $compiler @common @modFlags '-O2' '-shared' '-mguard=cf-nochecks' `
        '-Wl,--export-all-symbols' $source $engineLib '-lcomctl32' '-ladvapi32' '-o' $dll
    if ($LASTEXITCODE -ne 0) { throw "Mod DLL build failed: exit $LASTEXITCODE" }
}

if ($BuildPlannerTests -or $RunPlannerTests) {
    $testSource = Join-Path $root 'tests\ordering.test.cpp'
    $testExe = Join-Path $build 'ordering.test.exe'
    'Building the pure ordering planner checks...'
    # Windhawk's dynamic C++ import libraries name .whl runtime files intended
    # for its mod loader. A standalone console test must not depend on those.
    & $compiler @common '-O2' '-static' $testSource '-o' $testExe
    if ($LASTEXITCODE -ne 0) { throw "Planner build failed: exit $LASTEXITCODE" }
}

if ($RunPlannerTests) {
    'Running the standalone ordering planner checks...'
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Planner checks failed: exit $LASTEXITCODE" }
}

'Requested offline checks completed. No mod was installed or loaded into Explorer.'
