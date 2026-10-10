<#
.SYNOPSIS
Build HeavenBench and its tests natively with MSVC, Ninja, and pinned vcpkg dependencies.
.EXAMPLE
pwsh -File scripts/build_windows.ps1 -Jobs 2
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$VcpkgRoot = '',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Jobs = 2
)
. "$PSScriptRoot/windows_common.ps1"
$repoRoot = Split-Path $PSScriptRoot -Parent
Import-HeavenMsvcEnvironment
if (-not $BuildDir) { $BuildDir = Get-HeavenWindowsBuildDirectory $repoRoot $Configuration }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $BuildDir 'toolchains/vcpkg' }
$VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)
$manifest = Get-Content -Raw (Join-Path $repoRoot 'packaging/windows/vcpkg.json') | ConvertFrom-Json
$revision = $manifest.'builtin-baseline'

# Fetch shared pins through Git Bash, then configure only in native PowerShell.
Push-Location $repoRoot
try { Invoke-Checked (Get-HeavenGitBash) @('--noprofile', '--norc', './scripts/fetch_dependencies.sh') }
finally { Pop-Location }
$sdlSource = Initialize-HeavenSdlSource $BuildDir

if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git'))) {
    if ((Test-Path -LiteralPath $VcpkgRoot) -and @(Get-ChildItem -Force $VcpkgRoot).Count) {
        throw "Refusing to overwrite non-Git vcpkg directory: $VcpkgRoot"
    }
    New-Item -ItemType Directory -Force $VcpkgRoot | Out-Null
    Invoke-Checked git.exe @('init', '--quiet', $VcpkgRoot)
}
$head = & git.exe -C $VcpkgRoot rev-parse --verify HEAD 2>$null
if ($LASTEXITCODE -eq 0) {
    if ($head -ne $revision) { throw "vcpkg is at $head; expected $revision. Choose a fresh -VcpkgRoot." }
    Invoke-Checked git.exe @('-C', $VcpkgRoot, 'diff', '--quiet', 'HEAD', '--')
} else {
    if (@(Get-ChildItem -Force $VcpkgRoot | Where-Object Name -ne '.git').Count) {
        throw 'Interrupted vcpkg checkout contains local files; preserve them before retrying.'
    }
    Invoke-Checked git.exe @('-C', $VcpkgRoot, 'fetch', '--quiet', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $revision)
    Invoke-Checked git.exe @('-C', $VcpkgRoot, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
}
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_ROOT = $VcpkgRoot
Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
$installed = Join-Path $BuildDir 'vcpkg_installed'
$triplet = Join-Path $installed 'x64-windows-static'
$common = @(
    '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration",
    '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>',
    # Let CMake append its MSVC platform and exception flags. Clear only the
    # generic flag cache entries left by the previous helper when reconfiguring.
    '-UCMAKE_CXX_FLAGS', '-UCMAKE_C_FLAGS',
    '-DCMAKE_CXX_FLAGS_INIT=/utf-8 /Zc:__cplusplus /Zc:preprocessor /bigobj',
    '-DCMAKE_C_FLAGS_INIT=/utf-8', '-DUSE_STATIC_CRT=ON',
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_MANIFEST_DIR=$repoRoot/packaging/windows",
    "-DVCPKG_INSTALLED_DIR=$installed", '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
    '-DVCPKG_HOST_TRIPLET=x64-windows', '-DVCPKG_APPLOCAL_DEPS=ON',
    "-DPython3_EXECUTABLE=$((Get-Command python.exe).Source)"
)
Invoke-Checked cmake.exe (@('-S', "$repoRoot/benchmark", '-B', $BuildDir) + $common + @(
    '-DHEAVEN_TARGET_PLATFORM=windows', '-DHEAVEN_CPU_ARCH=portable',
    "-DHEAVEN_SDL_SOURCE_DIR=$sdlSource",
    '-DHEAVEN_BUNDLED_ASSET_DIR=resources', "-DGLM_INCLUDE_DIR=$triplet/include",
    "-DVulkan_INCLUDE_DIR=$triplet/include", "-DVulkan_LIBRARY=$triplet/lib/vulkan-1.lib",
    '-DFILAMENT_WINDOWS_CI_BUILD=ON'
))
Invoke-Checked cmake.exe @('--build', $BuildDir, '--parallel', "$Jobs", '--target',
    'heaven_benchmark', 'heaven_asset_packer', 'heaven_benchmark_tests',
    'heaven_profiler_trace_tests', 'heaven_benchmark_smoke')
$toolsDir = Join-Path $BuildDir 'tools'
Invoke-Checked cmake.exe (@('-S', $repoRoot, '-B', $toolsDir) + $common)
Invoke-Checked cmake.exe @('--build', $toolsDir, '--parallel', "$Jobs")

# Native tools and host tests use the same executable-adjacent resources as the
# portable ZIP. Copy the contents so incremental builds keep the same layout.
$resources = Join-Path $BuildDir 'resources'
New-Item -ItemType Directory -Force $resources | Out-Null
Get-ChildItem -LiteralPath "$repoRoot/benchmark/assets" -Force |
    Copy-Item -Destination $resources -Recurse -Force
Write-Host "Windows build complete: $BuildDir"
