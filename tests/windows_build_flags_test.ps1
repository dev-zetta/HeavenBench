# Exercise the real build helper's CMake arguments without downloads or MSVC.
param([string]$BuildScript = '', [switch]$InstallPrefixContract)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $BuildScript) { $BuildScript = Join-Path $repoRoot 'scripts/build_windows.ps1' }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven windows flags ' + [Guid]::NewGuid().ToString('N'))
$savedEnvironment = @{}
foreach ($name in @('VCPKG_DISABLE_METRICS', 'VCPKG_ROOT')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    New-Item -ItemType Directory "$temporary/scripts", "$temporary/packaging/windows", "$temporary/vcpkg/.git", "$temporary/benchmark/assets/sky" | Out-Null
    $resourceSource = Join-Path $temporary 'benchmark/assets/sky/moon_disk.png'
    Set-Content -LiteralPath $resourceSource -Value 'fixture moon resource' -Encoding utf8
    Copy-Item -LiteralPath $BuildScript -Destination "$temporary/scripts/build_windows.ps1"
    Copy-Item -LiteralPath "$repoRoot/scripts/windows_common.ps1" -Destination "$temporary/scripts"
    $global:HeavenBuildFlagRevision = 'fixture-vcpkg-revision'
    @{ 'builtin-baseline' = $global:HeavenBuildFlagRevision } | ConvertTo-Json |
        Set-Content -LiteralPath "$temporary/packaging/windows/vcpkg.json"
    $global:HeavenBuildFlagCalls = [Collections.Generic.List[object]]::new()
    # Only discovery and command execution are replaced. The checked-in helper
    # constructs both renderer and extraction-tool configure commands unchanged.
    Add-Content -LiteralPath "$temporary/scripts/windows_common.ps1" -Value @'
function Import-HeavenMsvcEnvironment {}
function Get-HeavenGitBash { return 'fixture-bash.exe' }
function Initialize-HeavenSdlSource { param([string]$BuildDir) return (Join-Path $BuildDir 'fixture SDL') }
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments = @(), [int]$ExpectedExitCode = 0)
    $global:HeavenBuildFlagCalls.Add(@{ command = $Command; arguments = $Arguments })
}
'@
    function global:git.exe { $global:LASTEXITCODE = 0; return $global:HeavenBuildFlagRevision }
    function global:python.exe {}
    & "$temporary/scripts/build_windows.ps1" -BuildDir "$temporary/build with spaces" -VcpkgRoot "$temporary/vcpkg" -Jobs 3
    $resourceTarget = Join-Path $temporary 'build with spaces/resources/sky/moon_disk.png'
    if (-not (Test-Path -LiteralPath $resourceTarget -PathType Leaf) -or
        (Get-FileHash -LiteralPath $resourceSource).Hash -ne (Get-FileHash -LiteralPath $resourceTarget).Hash) {
        throw 'Native build did not stage executable-adjacent resource bytes.'
    }
    $configures = @($global:HeavenBuildFlagCalls | Where-Object {
        [IO.Path]::GetFileName($_.command) -eq 'cmake.exe' -and $_.arguments -contains '-S'
    })
    if ($configures.Count -ne 2) { throw 'Renderer and extraction-tools configure commands must both be checked.' }
    $requiredFlags = @(
        '-UCMAKE_CXX_FLAGS', '-UCMAKE_C_FLAGS',
        '-DCMAKE_CXX_FLAGS_INIT=/utf-8 /Zc:__cplusplus /Zc:preprocessor /bigobj',
        '-DCMAKE_C_FLAGS_INIT=/utf-8',
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
    )
    foreach ($configure in $configures) {
        $arguments = $configure.arguments
        $expectedInstall = [IO.Path]::GetFullPath((Join-Path $temporary 'build with spaces')) + '/install'
        if ($arguments -notcontains "-DCMAKE_INSTALL_PREFIX=$expectedInstall") {
            throw 'Renderer and extraction tools need the same explicit absolute install prefix.'
        }
        if (@($arguments | Where-Object { $_ -match '^-DCMAKE_C(X{2})?_FLAGS=' }).Count) {
            throw 'Build helper replaces CMake platform defaults with generic compiler flags.'
        }
        foreach ($required in $requiredFlags) {
            if ($arguments -notcontains $required) { throw "Missing compiler initialization contract: $required" }
        }
    }
    $rendererBuilds = @($global:HeavenBuildFlagCalls | Where-Object {
        [IO.Path]::GetFileName($_.command) -eq 'cmake.exe' -and $_.arguments -contains '--target'
    })
    if ($rendererBuilds.Count -ne 1) { throw 'Expected one explicit renderer target build.' }
    foreach ($target in @('heaven_benchmark', 'heaven_asset_packer', 'heaven_benchmark_tests',
                          'heaven_profiler_trace_tests', 'heaven_benchmark_smoke')) {
        if ($rendererBuilds[0].arguments -notcontains $target) { throw "Missing required CI target: $target" }
    }
    Set-Content -LiteralPath $resourceSource -Value 'updated fixture moon resource' -Encoding utf8
    & "$temporary/scripts/build_windows.ps1" -BuildDir "$temporary/build with spaces" -VcpkgRoot "$temporary/vcpkg" -Jobs 3
    if ((Get-FileHash -LiteralPath $resourceSource).Hash -ne (Get-FileHash -LiteralPath $resourceTarget).Hash -or
        (Test-Path -LiteralPath (Join-Path $temporary 'build with spaces/resources/assets'))) {
        throw 'Incremental native build did not refresh the existing resource layout.'
    }
    $global:HeavenBuildFlagCalls.Clear()
    $configureDirectory = Join-Path $temporary 'configure only with spaces'
    & "$temporary/scripts/build_windows.ps1" -BuildDir $configureDirectory -VcpkgRoot "$temporary/vcpkg" -Jobs 3 -ConfigureOnly
    $configures = @($global:HeavenBuildFlagCalls | Where-Object {
        [IO.Path]::GetFileName($_.command) -eq 'cmake.exe' -and $_.arguments -contains '-S'
    })
    if ($configures.Count -ne 1 -or $configures[0].arguments -notcontains "$temporary/benchmark") {
        throw 'Configure-only must configure exactly the renderer through the canonical command.'
    }
    foreach ($required in $requiredFlags) {
        if ($configures[0].arguments -notcontains $required) { throw "Configure-only lost compiler initialization contract: $required" }
    }
    if (@($global:HeavenBuildFlagCalls | Where-Object { $_.arguments -contains '--build' }).Count -or
        (Test-Path -LiteralPath (Join-Path $configureDirectory 'resources')) -or
        (Test-Path -LiteralPath (Join-Path $configureDirectory 'tools'))) {
        throw 'Configure-only performed compilation, extraction-tool configuration or resource staging.'
    }
    if (-not $InstallPrefixContract) {
        Write-Host 'Windows build flag fixtures passed: compiler defaults, absolute install prefix, resources and configure-only isolation.'
        return
    }
    # The normal preflight runs before dependencies are fetched. This explicit
    # source-aware mode runs after canonical configuration verifies those pins.
    $nativeCmake = (Get-Command $(if ($IsWindows) { 'cmake.exe' } else { 'cmake' }) -CommandType Application | Select-Object -First 1).Source
    $prefixArguments = @($configures[0].arguments | Where-Object { $_ -like '-DCMAKE_INSTALL_PREFIX=*' })
    if ($prefixArguments.Count -ne 1) { throw 'Configure-only needs one explicit install prefix.' }
    $prefix = $prefixArguments[0].Substring('-DCMAKE_INSTALL_PREFIX='.Length)
    if (-not [IO.Path]::IsPathFullyQualified($prefix) -or $prefix -ne "$configureDirectory/install") {
        throw 'Configure-only install prefix must be absolute and belong to its build directory.'
    }

    # Execute the exact pinned cache mutation and libzip path calculations in
    # their production order, without configuring compilers or GPU dependencies.
    $glslang = Get-Content -Raw -LiteralPath "$repoRoot/lib/filament/third_party/glslang/tnt/CMakeLists.txt"
    $defaultPrefix = [regex]::Match($glslang, '(?ms)^if\(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND WIN32\)\r?\n.*?^endif\(\)')
    $libzip = Get-Content -Raw -LiteralPath "$repoRoot/lib/libzip/CMakeLists.txt"
    $relativePaths = [regex]::Matches($libzip, '(?m)^file\(RELATIVE_PATH pc_relative_(?:bindir|libdir|includedir) \$\{CMAKE_INSTALL_PREFIX\} \$\{CMAKE_INSTALL_FULL_(?:BINDIR|LIBDIR|INCLUDEDIR)\}\)\r?$')
    if (-not $defaultPrefix.Success -or $relativePaths.Count -ne 3) {
        throw 'Pinned Windows install-prefix reproduction inputs changed; review the fixture.'
    }
    $source = Join-Path $temporary 'install prefix source'
    New-Item -ItemType Directory $source | Out-Null
    $project = @'
cmake_minimum_required(VERSION 3.19)
project(HeavenWindowsInstallPrefixFixture LANGUAGES NONE)
set(WIN32 TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
include(GNUInstallDirs)
'@
    $project += "`n" + (($relativePaths | ForEach-Object { $_.Value }) -join "`n")
    $project += "`n" + $defaultPrefix.Value + "`n"
    Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Value $project -Encoding utf8
    foreach ($case in @('default-prefix-negative', 'explicit-prefix')) {
        $build = Join-Path $temporary $case
        foreach ($pass in @(1, 2)) {
            $arguments = @('-S', $source, '-B', $build)
            # Later SDK configurations reuse the cache without repeating the helper's flags.
            if ($case -eq 'explicit-prefix' -and $pass -eq 1) { $arguments += $prefixArguments[0] }
            $output = & $nativeCmake @arguments 2>&1
            $code = $LASTEXITCODE
            $output | ForEach-Object { Write-Host $_ }
            Write-Host "Install-prefix CMake fixture: $case pass=$pass exit=$code"
            if ($case -eq 'default-prefix-negative' -and $pass -eq 2) {
                if ($code -eq 0 -or ($output -join "`n") -notmatch 'RELATIVE_PATH must be passed a full path to the directory: install') {
                    throw 'Old default-prefix two-pass negative did not reproduce the native libzip failure.'
                }
            } elseif ($code -ne 0) { throw "Two-pass install-prefix configuration failed: $case pass=$pass" }
        }
        $cache = Get-Content -Raw -LiteralPath (Join-Path $build 'CMakeCache.txt')
        $expectedPrefix = if ($case -eq 'explicit-prefix') { $prefix.Replace('\', '/') } else { 'install' }
        if ($cache -notmatch ('(?m)^CMAKE_INSTALL_PREFIX:(?:PATH|STRING)=' + [regex]::Escape($expectedPrefix) + '\r?$')) {
            throw "Unexpected cached install prefix after two passes: $case"
        }
    }
    Write-Host 'Windows build flag fixtures passed: compiler defaults, resources, configure-only isolation and actual two-pass install-prefix positive/negative.'
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    Remove-Item Function:git.exe, Function:python.exe -ErrorAction SilentlyContinue
    Remove-Variable HeavenBuildFlagCalls, HeavenBuildFlagRevision -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
