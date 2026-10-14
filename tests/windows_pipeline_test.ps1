# Exercise orchestration and failure preservation without MSVC, downloads or a GPU.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. "$repoRoot/scripts/windows_common.ps1"

function Assert-That {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-HostCTestGate {
    param([string[]]$Arguments)
    foreach ($option in @('-R', '--tests-regex')) {
        Assert-That ($Arguments -notcontains $option) 'Host test gate has a restrictive include regex.'
    }
    Assert-That (@($Arguments | Where-Object { $_ -eq '-E' }).Count -eq 1) 'Host test gate must contain one exclusion.'
    Assert-That ($Arguments -notcontains '--exclude-regex') 'Host test gate contains a second exclusion spelling.'
    $pattern = $Arguments[[Array]::IndexOf($Arguments, '-E') + 1]
    Assert-That ($pattern -ceq '^heaven_benchmark_smoke$') 'Host test gate must exclude only graphical smoke.'
    foreach ($hostTest in @('heaven_benchmark_tests', 'heaven_profiler_trace_tests',
                           'heaven_microprofile_analysis', 'heaven_runtime_paths_cli',
                           'heaven_asset_packer_cli', 'heaven_gtao_bent_normal_contract',
                           'heaven_specular_occluded_moment_contract', 'heaven_surface_color_contract',
                           'heaven_capture_camera_motion_tests', 'heaven_frame_generation_history_tests',
                           'heaven_filament_ssr_variant_tests', 'heaven_vulkan_pipeline_lifetime_tests',
                           'heaven_dlss_contract_tests', 'heaven_dlss_runtime_bridge_tests',
                           'heaven_fsr31_source_contract', 'heaven_temporal_input_contract',
                           'heaven_filament_depth_program_lifetime', 'heaven_filament_froxel_stream_lifetime',
                           'heaven_future_cpu_contract', 'heaven_benchmark_smoke_cpu_contract')) {
        Assert-That ($hostTest -notmatch $pattern) "Host test gate omits $hostTest."
    }
    Assert-That ('heaven_benchmark_smoke' -match $pattern) 'Host test gate absorbs the separately controlled graphical smoke gate.'
}

$nativeShell = Join-Path $PSHOME $(if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' })
Invoke-Checked $nativeShell @('-NoLogo', '-NoProfile', '-Command', 'exit 7') -ExpectedExitCode 7
$failed = $false
try { Invoke-Checked $nativeShell @('-NoLogo', '-NoProfile', '-Command', 'exit 7') }
catch { $failed = $true }
Assert-That $failed 'Invoke-Checked accepted an unexpected native exit status.'
Invoke-Checked $nativeShell @('-NoLogo', '-NoProfile', '-File', "$PSScriptRoot/windows_build_flags_test.ps1")

$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven windows pipeline ' + [Guid]::NewGuid().ToString('N'))
$environmentNames = @('PATH', 'SystemRoot', 'HEAVEN_USER_DIR', 'HEAVEN_DATA', 'HEAVEN_DEFAULT_DATA',
                      'HEAVEN_RESOURCE_DIR', 'HEAVEN_SMOKE')
$originalEnvironment = @{}
foreach ($name in $environmentNames) {
    $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    New-Item -ItemType Directory "$temporary/scripts", "$temporary/benchmark" | Out-Null
    Copy-Item -LiteralPath "$repoRoot/scripts/ci_windows.ps1" -Destination "$temporary/scripts"
    Copy-Item -LiteralPath "$repoRoot/scripts/windows_common.ps1" -Destination "$temporary/scripts"
    Set-Content -LiteralPath "$temporary/benchmark/CMakeLists.txt" -Value 'project(HeavenBench VERSION 1.2.3 LANGUAGES CXX)'
    # Override only native execution/platform discovery in this private fixture.
    # The actual release script, archive extraction, path selection and assertions run unchanged.
    Add-Content -LiteralPath "$temporary/scripts/windows_common.ps1" -Value @'
function Import-HeavenMsvcEnvironment { $env:PATH = 'fixture-build-tool-path' }
function Get-HeavenWindowsBuildDirectory { param($RepoRoot) return (Join-Path $RepoRoot 'default build') }
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments = @(), [int]$ExpectedExitCode = 0)
    $name = [IO.Path]::GetFileName($Command)
    $global:HeavenPipelineEvents.Add(@{ name = $name; arguments = $Arguments;
        expected = $ExpectedExitCode; cwd = (Get-Location).Path; path = $env:PATH;
        smoke = $env:HEAVEN_SMOKE; resource = $env:HEAVEN_RESOURCE_DIR })
    if ($name -eq 'python.exe' -and $global:HeavenPipelineFailure -eq 'preflight' -and
        [IO.Path]::GetFileName($Arguments[0]) -eq 'dependency_bootstrap_test.py') {
        throw 'fixture native script preflight failure'
    }
    if ($name -eq 'ctest.exe' -and
        $global:HeavenPipelineFailure -in @('tests', 'future-tests', 'smoke-tests') -and
        [IO.Path]::GetFileName($Arguments[[Array]::IndexOf($Arguments, '--test-dir') + 1]) -ne 'tools') {
        $failingTest = switch ($global:HeavenPipelineFailure) {
            'tests' { 'heaven_profiler_trace_tests' }
            'future-tests' { 'heaven_future_cpu_contract' }
            'smoke-tests' { 'heaven_benchmark_smoke' }
        }
        $selected = $true
        foreach ($option in @('-R', '--tests-regex')) {
            if ($Arguments -contains $option -and
                $failingTest -notmatch $Arguments[[Array]::IndexOf($Arguments, $option) + 1]) {
                $selected = $false
            }
        }
        foreach ($option in @('-E', '--exclude-regex')) {
            if ($Arguments -contains $option -and
                $failingTest -match $Arguments[[Array]::IndexOf($Arguments, $option) + 1]) {
                $selected = $false
            }
        }
        if ($selected) { throw "fixture selected test failure: $failingTest" }
    }
    if ($name -eq 'heaven_asset_packer.exe' -and $Arguments -contains '--pack-assets') {
        if ($ExpectedExitCode -ne 2) { throw 'Missing-asset failure must explicitly require exit 2.' }
        if ($global:HeavenPipelineFailure -eq 'packer-output') {
            Set-Content -LiteralPath $Arguments[-1] -Value 'unwanted partial archive'
        }
    }
    if ($name -eq 'Launch-HeavenBench.cmd') {
        New-Item -ItemType Directory -Force $env:HEAVEN_USER_DIR | Out-Null
        $index = [Array]::IndexOf($Arguments, '--write-config')
        if ($index -ge 0) {
            $data = Join-Path (Split-Path $Command -Parent) 'assets'
            if ($env:HEAVEN_DATA) { $data = $env:HEAVEN_DATA }
            $dataIndex = [Array]::IndexOf($Arguments, '--data')
            if ($dataIndex -ge 0) { $data = $Arguments[$dataIndex + 1] }
            if ($global:HeavenPipelineFailure -eq 'launcher-data') { $data = 'wrong assets' }
            @{ data_dir = $data } | ConvertTo-Json | Set-Content -LiteralPath $Arguments[$index + 1]
        }
    }
}
'@
    Set-Content -LiteralPath "$temporary/scripts/build_windows.ps1" -Value @'
param([string]$BuildDir, [string]$Configuration, [int]$Jobs, [switch]$HostPreflight)
$global:HeavenPipelineEvents.Add(@{ name = 'build'; directory = $BuildDir; configuration = $Configuration; jobs = $Jobs; host_preflight = [bool]$HostPreflight })
if ($global:HeavenPipelineFailure -eq 'build') { throw 'fixture build failure' }
'@
    Set-Content -LiteralPath "$temporary/scripts/package_windows.ps1" -Value @'
param([string]$BuildDir, [string]$OutputDir, [string]$Version)
$global:HeavenPipelineEvents.Add(@{ name = 'package'; directory = $BuildDir; output = $OutputDir; version = $Version })
$stage = Join-Path $OutputDir 'fixture staging'
$package = Join-Path $stage "HeavenBench-$Version-windows-x64"
New-Item -ItemType Directory -Force "$package/assets" | Out-Null
foreach ($name in @('heaven_benchmark.exe', 'heaven_asset_packer.exe', 'Launch-HeavenBench.cmd', 'assets/README.txt')) {
    Set-Content -LiteralPath (Join-Path $package $name) -Value 'fixture'
}
# An unrelated old release must not prevent verification of this exact output.
Set-Content -LiteralPath (Join-Path $OutputDir 'HeavenBench-0.0.1-windows-x64.zip') -Value 'old release'
$archive = Join-Path $OutputDir "HeavenBench-$Version-windows-x64.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive)
Remove-Item -LiteralPath $stage -Recurse -Force
'@
    $env:SystemRoot = Join-Path $temporary 'Windows'
    $env:PATH = 'caller PATH'
    foreach ($name in $environmentNames | Where-Object { $_ -notin @('PATH', 'SystemRoot') }) {
        [Environment]::SetEnvironmentVariable($name, "caller $name", 'Process')
    }
    $callerEnvironment = @{}
    foreach ($name in $environmentNames) {
        $callerEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $initialLocation = (Get-Location).Path
    foreach ($case in @('success', 'defaults', 'preflight', 'build', 'tests', 'future-tests',
                        'smoke-tests', 'packer-output', 'launcher-data')) {
        $failure = if ($case -in @('success', 'defaults')) { '' } else { $case }
        $global:HeavenPipelineFailure = $failure
        $global:HeavenPipelineEvents = [Collections.Generic.List[object]]::new()
        $build = if ($case -eq 'defaults') { Join-Path $temporary 'default build' }
                 else { Join-Path $temporary "$case/build with spaces" }
        $output = if ($case -eq 'defaults') { Join-Path $temporary 'build/packages/windows' }
                  else { Join-Path $temporary "$case/packages with spaces" }
        $jobs = if ($case -eq 'defaults') { 2 } else { 3 }
        $caught = $false
        try {
            if ($case -eq 'defaults') { & "$temporary/scripts/ci_windows.ps1" }
            else { & "$temporary/scripts/ci_windows.ps1" -Jobs $jobs -BuildDir $build -OutputDir $output }
        }
        catch { $caught = $true }
        Assert-That ($caught -eq [bool]$failure) "Wrong failure outcome for '$case'."
        Assert-That ((Get-Location).Path -eq $initialLocation) "Working directory leaked after '$case'."
        foreach ($name in $environmentNames) {
            Assert-That ([Environment]::GetEnvironmentVariable($name, 'Process') -ceq $callerEnvironment[$name]) "Environment $name leaked after '$case'."
        }
        $events = @($global:HeavenPipelineEvents.ToArray())
        $buildEvent = @($events | Where-Object { $_.name -eq 'build' })
        $pythonEvents = @($events | Where-Object { $_.name -eq 'python.exe' })
        $bootstrap = @($pythonEvents | Where-Object {
            [IO.Path]::GetFileName($_.arguments[0]) -eq 'dependency_bootstrap_test.py'
        })
        Assert-That ($bootstrap.Count -eq 1) 'Bootstrap portability preflight did not run exactly once.'
        if ($failure -eq 'preflight') {
            Assert-That ($buildEvent.Count -eq 0) 'Failed script preflight reached the expensive native build.'
            Assert-That (@($events | Where-Object { $_.name -eq 'package' }).Count -eq 0) 'Failed script preflight reached packaging.'
            continue
        }
        Assert-That ($pythonEvents.Count -eq 8) 'License and seven build-independent fixture gates must all run.'
        Assert-That ($buildEvent.Count -eq 1) 'Build did not run exactly once.'
        Assert-That ([Array]::IndexOf($events, $pythonEvents[-1]) -lt [Array]::IndexOf($events, $buildEvent[0])) 'Script preflights ran after native compilation.'
        Assert-That ($buildEvent[0].directory -eq $build -and $buildEvent[0].jobs -eq $jobs -and $buildEvent[0].configuration -eq 'Release') 'Build arguments were lost.'
        Assert-That $buildEvent[0].host_preflight 'Standard Windows release bypassed the early native host preflight.'
        $packages = @($events | Where-Object { $_.name -eq 'package' })
        if ($failure -eq 'build') {
            Assert-That ($packages.Count -eq 0) 'A failed build/test reached packaging.'
            continue
        }
        $tests = @($events | Where-Object { $_.name -eq 'ctest.exe' })
        Assert-That ($tests[0].arguments[1] -eq $build) 'Host tests used another build directory.'
        Assert-HostCTestGate $tests[0].arguments
        if ($failure -in @('tests', 'future-tests')) {
            Assert-That ($packages.Count -eq 0) 'A failed host test reached packaging.'
            Assert-That ($tests.Count -eq 1) 'Host failure did not stop before extraction and graphical smoke gates.'
            continue
        }
        Assert-That ($tests.Count -eq 3) 'Host, extraction and smoke gate tests must all run.'
        Assert-That ($tests[1].arguments[1] -eq (Join-Path $build 'tools') -and
                     $tests[2].arguments[1] -eq $build) 'Extraction or graphical smoke tests used another build directory.'
        Assert-That (@($tests[2].arguments | Where-Object { $_ -eq '-R' }).Count -eq 1) 'Graphical smoke needs one dedicated include gate.'
        $smokePattern = $tests[2].arguments[[Array]::IndexOf($tests[2].arguments, '-R') + 1]
        Assert-That ($smokePattern -ceq '^heaven_benchmark_smoke$') 'Graphical smoke gate includes other test suites.'
        foreach ($option in @('--tests-regex', '-E', '--exclude-regex')) {
            Assert-That ($tests[2].arguments -notcontains $option) 'Graphical smoke contains an additional filter.'
        }
        Assert-That ($tests[2].smoke -eq '0') 'Smoke gate inherited GPU opt-in.'
        foreach ($event in $tests) {
            Assert-That ($event.arguments -contains '--no-tests=error') 'Missing test discovery would silently pass.'
            Assert-That (-not $event.resource) 'Native tests retained a source-resource override.'
        }
        if ($failure -eq 'smoke-tests') {
            Assert-That ($packages.Count -eq 0) 'A failed graphical smoke gate reached packaging.'
            continue
        }
        Assert-That ($packages.Count -eq 1 -and $packages[0].output -eq $output -and $packages[0].directory -eq $build) 'Package paths were lost.'
        $relocated = @($events | Where-Object { $_.name -eq 'heaven_benchmark.exe' })[0]
        Assert-That ($relocated.path -eq "$env:SystemRoot/System32;$env:SystemRoot") 'Relocation check retained toolchain DLL search paths.'
        Assert-That (-not $relocated.resource) 'Relocation check retained source-resource override.'
        if ($failure) {
            Assert-That (Test-Path -LiteralPath $relocated.cwd) 'Failure evidence was removed.'
            Remove-Item -LiteralPath $relocated.cwd -Recurse -Force
        } else {
            Assert-That (-not (Test-Path -LiteralPath $relocated.cwd)) 'Successful verification leaked temporary files.'
            Assert-That (@($events | Where-Object { $_.name -eq 'Launch-HeavenBench.cmd' }).Count -eq 4) 'Launcher help and all data-precedence checks did not run.'
        }
    }
    $legacyRejected = $false
    try { Assert-HostCTestGate @('--test-dir', 'fixture', '-R', '^heaven_(benchmark_tests|profiler_trace_tests)$') }
    catch {
        Assert-That ($_.Exception.Message -like '*restrictive include regex*') 'Legacy gate failed for an unrelated reason.'
        $legacyRejected = $true
    }
    Assert-That $legacyRejected 'Legacy restrictive host whitelist passed the field oracle.'
    Write-Host 'Windows pipeline fixtures passed: native exit handling, nine orchestration cases and restrictive-whitelist negative control.'
} finally {
    foreach ($entry in $originalEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
    Remove-Variable HeavenPipelineEvents, HeavenPipelineFailure -Scope Global -ErrorAction SilentlyContinue
}
