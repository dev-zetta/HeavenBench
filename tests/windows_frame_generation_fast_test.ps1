# Execute the checked-in workflow's fast-step body with native commands mocked.
# No downloads, CMake build, SDK DLLs, MSVC or GPU are used by this fixture.
param([string]$Workflow = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Workflow) { $Workflow = Join-Path $repoRoot '.github/workflows/frame-generation.yml' }
$text = Get-Content -Raw -LiteralPath $Workflow
$step = [regex]::Match($text, '(?ms)^      - name: Fast native DLSS SDK compile and host contracts\r?\n.*?^        run: \|\r?\n(?<body>.*?)(?=^      - )')
if (-not $step.Success) { throw 'Fast DLSS step body is missing.' }
if ($step.Value -notmatch "if: env.FRAME_GENERATION_PROVIDERS != 'fsr3'") { throw 'FSR-only runs must skip the fast DLSS step.' }
$body = ($step.Groups['body'].Value -split '\r?\n' | ForEach-Object { $_ -replace '^          ', '' }) -join "`n"
$baseline = $text.IndexOf('      - name: Prepare native baseline and verified SDK inputs')
$integration = $text.IndexOf('      - name: Compile optional native paths and run host contracts')
if ($baseline -le $step.Index -or $integration -le $baseline -or
    $text.Substring($integration) -notmatch "'heaven_dlss_frame_token_tests'") {
    throw 'Fast DLSS step replaced, followed or weakened the full integration job.'
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven fast dlss ' + [Guid]::NewGuid().ToString('N'))
$saved = @{}
foreach ($name in @('FRAME_GENERATION_PROVIDERS', 'HEAVEN_SMOKE', 'HEAVEN_DLSS_PLUGIN_DIR', 'HEAVEN_DLSS_PROJECT_ID')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$location = Get-Location
try {
    New-Item -ItemType Directory "$temporary/scripts", "$temporary/benchmark" | Out-Null
    Copy-Item -LiteralPath "$repoRoot/scripts/windows_common.ps1" -Destination "$temporary/scripts"
    Add-Content -LiteralPath "$temporary/scripts/windows_common.ps1" -Value @'
function Import-HeavenMsvcEnvironment {}
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments = @(), [int]$ExpectedExitCode = 0)
    $name = [IO.Path]::GetFileName($Command)
    $global:HeavenFastCalls.Add(@{ command = $name; arguments = $Arguments })
    if ($Arguments -contains '-InstallPrefixContract' -and $global:HeavenFastCase -eq 'prefix') {
        throw 'fixture two-pass prefix contract failure'
    }
    if ($name -eq 'python.exe') {
        if ($global:HeavenFastCase -eq 'fetch') { throw 'fixture source verification failure' }
        $headers = $Arguments[[Array]::IndexOf($Arguments, '--output') + 1]
        New-Item -ItemType Directory -Force $headers | Out-Null
        Set-Content (Join-Path $headers 'heavenbench-source.json') '{}'
        Set-Content (Join-Path $headers 'license.txt') 'fixture framework license'
    }
    if ($name -eq 'cmake.exe' -and $Arguments -contains '-S') {
        $build = $Arguments[[Array]::IndexOf($Arguments, '-B') + 1]
        $enabled = $Arguments -contains '-DHEAVEN_ENABLE_DLSS=ON'
        if (-not $enabled -and $global:HeavenFastCase -eq 'restore') { throw 'fixture restoration failure' }
        $value = if ($enabled) { 'ON' } else { 'OFF' }
        Set-Content (Join-Path $build 'CMakeCache.txt') "HEAVEN_ENABLE_DLSS:BOOL=$value`nHEAVEN_ENABLE_FSR3:BOOL=OFF"
        if ($enabled -and $global:HeavenFastCase -eq 'configure') { throw 'fixture optional configure failure' }
    }
    if ($name -eq 'cmake.exe' -and $Arguments -contains '--build' -and $global:HeavenFastCase -eq 'build') {
        throw 'fixture optional compiler failure'
    }
    if ($name -eq 'ctest.exe') {
        if ($Arguments -contains '--show-only=json-v1') {
            $names = @('heaven_dlss_contract_tests', 'heaven_dlss_frame_token_tests',
                'heaven_dlss_runtime_bridge_tests', 'heaven_dlss_lifecycle_contract',
                'heaven_dlss_frame_token_negative_contract')
            if ($global:HeavenFastCase -eq 'missing') { $names = $names | Where-Object { $_ -ne 'heaven_dlss_frame_token_negative_contract' } }
            return @{ tests = @($names | ForEach-Object { @{ name = $_ } }) } | ConvertTo-Json -Depth 4
        }
        if ($global:HeavenFastCase -eq 'tests') { throw 'fixture optional host failure' }
    }
}
'@
    Set-Content -LiteralPath "$temporary/scripts/build_windows.ps1" -Value @'
param([string]$BuildDir, [int]$Jobs, [switch]$ConfigureOnly)
$global:HeavenFastCalls.Add(@{ command = 'canonical-configure'; arguments = @($BuildDir, $Jobs, [bool]$ConfigureOnly) })
if (-not $ConfigureOnly -or $Jobs -ne 2) { throw 'Fast step bypassed canonical configure-only.' }
New-Item -ItemType Directory -Force $BuildDir | Out-Null
Set-Content (Join-Path $BuildDir 'CMakeCache.txt') "HEAVEN_ENABLE_DLSS:BOOL=OFF`nHEAVEN_ENABLE_FSR3:BOOL=OFF"
'@
    function global:git.exe { $global:LASTEXITCODE = 0; return 'fixture-source-head' }
    foreach ($case in @('success', 'dlss', 'prefix', 'fetch', 'configure', 'build', 'tests', 'missing', 'restore')) {
        $working = Join-Path $temporary $case
        New-Item -ItemType Directory -Force "$working/scripts", "$working/benchmark" | Out-Null
        Copy-Item -LiteralPath "$temporary/scripts/windows_common.ps1", "$temporary/scripts/build_windows.ps1" -Destination "$working/scripts"
        Set-Location $working
        $global:HeavenFastCase = $case
        $global:HeavenFastCalls = [Collections.Generic.List[object]]::new()
        $env:FRAME_GENERATION_PROVIDERS = if ($case -eq 'dlss') { 'dlss' } else { 'both' }
        $caught = $false
        try { & ([scriptblock]::Create($body)) } catch { $caught = $true }
        $expectedFailure = $case -notin @('success', 'dlss')
        if ($caught -ne $expectedFailure) { throw "Wrong fast-step failure outcome: $case" }
        $calls = @($global:HeavenFastCalls.ToArray())
        $configure = @($calls | Where-Object { $_.command -eq 'canonical-configure' })
        if ($configure.Count -ne 1) { throw "Fast step did not reuse canonical configuration: $case" }
        $prefixContract = @($calls | Where-Object { $_.command -eq 'pwsh.exe' -and $_.arguments -contains '-InstallPrefixContract' })
        if ($prefixContract.Count -ne 1 -or
            $prefixContract[0].arguments -notcontains 'tests/windows_build_flags_test.ps1' -or
            [Array]::IndexOf($calls, $prefixContract[0]) -le [Array]::IndexOf($calls, $configure[0])) {
            throw "Two-pass prefix contract must run after canonical dependency configuration: $case"
        }
        $sdkConfigures = @($calls | Where-Object { $_.command -eq 'cmake.exe' -and $_.arguments -contains '-DHEAVEN_ENABLE_DLSS=ON' })
        if (@($sdkConfigures | Where-Object {
            [Array]::IndexOf($calls, $_) -lt [Array]::IndexOf($calls, $prefixContract[0])
        }).Count) { throw "Optional SDK configure bypassed the two-pass prefix contract: $case" }
        $restore = @($calls | Where-Object { $_.command -eq 'cmake.exe' -and $_.arguments -contains '-DHEAVEN_ENABLE_DLSS=OFF' })
        if ($restore.Count -ne 1 -or $restore[0].arguments -notcontains '-DHEAVEN_ENABLE_FSR3=OFF') {
            throw "Fast step failed to restore both SDK-off flags: $case"
        }
        $cache = Get-Content -Raw (Join-Path $working 'build/frame-generation/windows-msvc/CMakeCache.txt')
        if ($case -ne 'restore' -and $cache -notmatch 'HEAVEN_ENABLE_DLSS:BOOL=OFF') { throw "Optional SDK state leaked: $case" }
        $builds = @($calls | Where-Object { $_.command -eq 'cmake.exe' -and $_.arguments -contains '--build' })
        if ($case -notin @('prefix', 'fetch', 'configure')) {
            if ($builds.Count -ne 1) { throw "Wrong fast compile count: $case" }
            $targets = $builds[0].arguments[([Array]::IndexOf($builds[0].arguments, '--target') + 1)..($builds[0].arguments.Count - 1)]
            if (($targets -join ',') -cne 'heaven_dlss_contract_tests,heaven_dlss_frame_token_tests,heaven_dlss_runtime_bridge_tests') {
                throw "Fast compile included Filament or omitted a host target: $case"
            }
        }
        $executions = @($calls | Where-Object { $_.command -eq 'ctest.exe' -and $_.arguments -contains '--output-on-failure' })
        if ($case -in @('success', 'dlss', 'tests', 'restore')) {
            if ($executions.Count -ne 1 -or $executions[0].arguments -notcontains '--no-tests=error') { throw "Missing checked fast host execution: $case" }
            $pattern = $executions[0].arguments[[Array]::IndexOf($executions[0].arguments, '-R') + 1]
            foreach ($name in @('heaven_dlss_contract_tests', 'heaven_dlss_frame_token_tests',
                'heaven_dlss_runtime_bridge_tests', 'heaven_dlss_lifecycle_contract', 'heaven_dlss_frame_token_negative_contract')) {
                if ($name -notmatch $pattern) { throw "Fast host filter omitted $name" }
            }
            if ('heaven_benchmark_smoke' -match $pattern) { throw 'Fast host filter selected graphical smoke.' }
        } elseif ($executions.Count) { throw "Fast failure continued to host tests: $case" }
    }
    Write-Host 'Fast Windows DLSS workflow fixtures passed: canonical configuration, two-pass prefix gate, three independent targets, required host discovery, failure paths and SDK-off restoration.'
} finally {
    Set-Location $location
    foreach ($entry in $saved.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
    Remove-Item Function:git.exe -ErrorAction SilentlyContinue
    Remove-Variable HeavenFastCase, HeavenFastCalls -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
