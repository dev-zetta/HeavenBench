<#
.SYNOPSIS
Build, test, package and verify the Windows release using the same local and CI pipeline.
.EXAMPLE
pwsh -File scripts/ci_windows.ps1 -Jobs 2
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 64)][int]$Jobs = 2,
    [string]$BuildDir = '',
    [string]$OutputDir = '',
    [switch]$PackageOnly,
    [ValidateSet('', 'fsr3', 'dlss', 'both')][string]$ExperimentalProviders = '',
    [string]$SourceManifest = ''
)
. "$PSScriptRoot/windows_common.ps1"
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) { $BuildDir = Get-HeavenWindowsBuildDirectory $repoRoot }
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'build/packages/windows' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$version = Get-HeavenPackageVersion $repoRoot

# A local invocation may already have asset overrides or GPU-smoke opt-in set.
# Restore its process environment even if a build or verification step fails.
$savedEnvironment = @{}
foreach ($name in @('PATH', 'HEAVEN_USER_DIR', 'HEAVEN_DATA', 'HEAVEN_DEFAULT_DATA',
                    'HEAVEN_RESOURCE_DIR', 'HEAVEN_SMOKE', 'HEAVEN_DLSS_PLUGIN_DIR', 'HEAVEN_DLSS_PROJECT_ID')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$verificationRoot = $null
$verified = $false
try {
    Import-HeavenMsvcEnvironment
    if (-not $PackageOnly) {
        Invoke-Checked (Join-Path $PSHOME 'pwsh.exe') @('-NoLogo', '-NoProfile', '-File',
            "$repoRoot/tests/windows_pipeline_test.ps1")
        Invoke-Checked (Join-Path $PSHOME 'pwsh.exe') @('-NoLogo', '-NoProfile', '-File',
            "$repoRoot/tests/windows_host_preflight_test.ps1")
        Invoke-Checked python.exe @("$repoRoot/tests/collect_licenses_test.py")
        # Catch script/platform defects before the cold native dependency build.
        # The complete tools CTest group still runs after compilation.
        foreach ($fixture in @('visual_audit_test.py', 'docker_cache_test.py',
                               'docker_build_test.py', 'dependency_bootstrap_test.py',
                               'asset_builder_test.py', 'repository_hygiene_test.py',
                               'assemble_scene_test.py')) {
            Invoke-Checked python.exe @("$repoRoot/tests/$fixture")
        }
        & "$PSScriptRoot/build_windows.ps1" -BuildDir $BuildDir -Configuration Release -Jobs $Jobs -HostPreflight
        [Environment]::SetEnvironmentVariable('HEAVEN_RESOURCE_DIR', $null, 'Process')
        Invoke-Checked ctest.exe @('--test-dir', $BuildDir, '--output-on-failure', '--no-tests=error',
            '--timeout', '300', '-E', '^heaven_benchmark_smoke$')
        Invoke-Checked ctest.exe @('--test-dir', (Join-Path $BuildDir 'tools'),
            '--output-on-failure', '--no-tests=error', '--timeout', '120')
        $env:HEAVEN_SMOKE = '0'
        Invoke-Checked ctest.exe @('--test-dir', $BuildDir, '--output-on-failure', '--no-tests=error',
            '--timeout', '30', '-R', '^heaven_benchmark_smoke$')
    }
    $packageArguments = @{ BuildDir = $BuildDir; OutputDir = $OutputDir; Version = $version }
    if ($ExperimentalProviders) {
        if (-not $PackageOnly) { throw 'Experimental SDK packages must use the already tested build via -PackageOnly.' }
        $packageArguments.ExperimentalProviders = $ExperimentalProviders
        $packageArguments.SourceManifest = $SourceManifest
        $env:HEAVEN_DLSS_PLUGIN_DIR = $null
        $env:HEAVEN_DLSS_PROJECT_ID = $null
    } elseif ($SourceManifest) { throw 'SourceManifest requires ExperimentalProviders.' }
    & "$PSScriptRoot/package_windows.ps1" @packageArguments

    # Select this invocation's package explicitly; older releases may coexist.
    $artifactSuffix = ''
    if ($ExperimentalProviders) {
        $source = Get-Content -Raw -LiteralPath $SourceManifest | ConvertFrom-Json
        $artifactSuffix = "-experimental-$ExperimentalProviders-$($source.source_sha256.Substring(0, 12))"
    }
    $archive = Join-Path $OutputDir "HeavenBench-$version$artifactSuffix-windows-x64.zip"
    $verificationRoot = Join-Path ([IO.Path]::GetTempPath()) ('heavenbench-package-' + [Guid]::NewGuid().ToString('N'))
    $destination = Join-Path $verificationRoot 'unpacked'
    Expand-Archive -LiteralPath $archive -DestinationPath $destination
    $packages = @(Get-ChildItem -LiteralPath $destination -Directory)
    if ($packages.Count -ne 1) { throw 'Expected exactly one extracted package.' }
    $package = $packages[0]
    if ($ExperimentalProviders) {
        $metadata = Get-Content -Raw -LiteralPath (Join-Path $package.FullName 'experimental-frame-generation.json') | ConvertFrom-Json
        if (-not $metadata.experimental -or $metadata.providers_built -ne $ExperimentalProviders -or
            $metadata.source_archive_sha256 -ne $source.source_sha256) { throw 'Experimental package metadata does not match selected SDK/source.' }
        $binaryHash = (Get-FileHash -LiteralPath (Join-Path $package.FullName 'heaven_benchmark.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($binaryHash -cne $metadata.binary_sha256) { throw 'Experimental package binary differs from metadata.' }
        if (Get-ChildItem -LiteralPath $package.FullName -Recurse -File | Where-Object Name -match '^(sl\..*|nvngx.*)\.dll$') {
            throw 'Experimental package includes a bring-your-own NVIDIA runtime DLL.'
        }
        $manifest = Get-Content -Raw -LiteralPath (Join-Path $package.FullName 'package-manifest.json') | ConvertFrom-Json
        foreach ($entry in $manifest.files) {
            $digest = (Get-FileHash -LiteralPath (Join-Path $package.FullName $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($digest -cne $entry.sha256) { throw "Experimental package manifest mismatch: $($entry.path)" }
        }
    }

    # Only Windows system DLLs and files beside the EXEs may satisfy imports.
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:HEAVEN_USER_DIR = Join-Path $verificationRoot 'writable user data'
    foreach ($name in @('HEAVEN_DATA', 'HEAVEN_DEFAULT_DATA', 'HEAVEN_RESOURCE_DIR')) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    Push-Location $verificationRoot
    try {
        Invoke-Checked (Join-Path $package.FullName 'heaven_benchmark.exe') @('--help')
        $packer = Join-Path $package.FullName 'heaven_asset_packer.exe'
        Invoke-Checked $packer @('--help')
        $packerTarget = Join-Path $verificationRoot 'must-not-exist.zip'
        Invoke-Checked $packer @('--data', 'absent assets.zip', '--pack-assets', $packerTarget) -ExpectedExitCode 2
        if (Test-Path -LiteralPath $packerTarget) { throw 'Failed packer unexpectedly created an output ZIP.' }
        $launcher = Join-Path $package.FullName 'Launch-HeavenBench.cmd'
        Invoke-Checked $launcher @('--help')
        if (-not (Test-Path -LiteralPath $env:HEAVEN_USER_DIR -PathType Container)) {
            throw 'Launcher did not create the writable user directory.'
        }
        $effective = Join-Path $verificationRoot 'effective config.json'
        Invoke-Checked $launcher @('--write-config', $effective)
        $config = Get-Content -Raw -LiteralPath $effective | ConvertFrom-Json
        if ($ExperimentalProviders -and $config.frame_generation -ne 'off') {
            throw 'Experimental packages must keep frame generation off by default.'
        }
        if ($config.data_dir -ne (Join-Path $package.FullName 'assets')) {
            throw 'Launcher did not select its adjacent assets directory.'
        }
        $env:HEAVEN_DATA = 'environment-assets.zip'
        Invoke-Checked $launcher @('--write-config', $effective)
        if ((Get-Content -Raw -LiteralPath $effective | ConvertFrom-Json).data_dir -ne $env:HEAVEN_DATA) {
            throw 'Launcher did not honor HEAVEN_DATA.'
        }
        Invoke-Checked $launcher @('--data', 'cli-assets.zip', '--write-config', $effective)
        if ((Get-Content -Raw -LiteralPath $effective | ConvertFrom-Json).data_dir -ne 'cli-assets.zip') {
            throw 'Explicit --data did not take precedence over HEAVEN_DATA.'
        }
    } finally { Pop-Location }
    $verified = $true
    if ($ExperimentalProviders) { Write-Host "Experimental Windows package relocation verified: $archive" }
    else { Write-Host "Windows release verified: $archive" }
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    if ($verificationRoot -and (Test-Path -LiteralPath $verificationRoot)) {
        if ($verified) { Remove-Item -LiteralPath $verificationRoot -Recurse -Force }
        else { Write-Warning "Package verification evidence retained at $verificationRoot" }
    }
}
