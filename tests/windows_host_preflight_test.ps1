# Execute the canonical opt-in native gate with commands mocked; no MSVC/GPU/downloads.
param([string]$BuildScript = '', [string]$Workflow = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $BuildScript) { $BuildScript = Join-Path $repoRoot 'scripts/build_windows.ps1' }
if (-not $Workflow) { $Workflow = Join-Path $repoRoot '.github/workflows/frame-generation.yml' }

function Assert-Preflight {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# Both the conditional fast path and unconditional preparation must opt in;
# otherwise a manual fsr3-only dispatch would miss the early native check.
$text = Get-Content -Raw -LiteralPath $Workflow
foreach ($name in @('Fast native DLSS SDK compile and host contracts', 'Prepare native baseline and verified SDK inputs')) {
    $step = [regex]::Match($text, '(?ms)^      - name: ' + [regex]::Escape($name) + '\r?\n(?<head>.*?)^        run: \|\r?\n(?<body>.*?)(?=^      - )')
    Assert-Preflight $step.Success "Missing optional workflow stage: $name"
    if ($name -like 'Prepare*') {
        Assert-Preflight ($step.Groups['head'].Value -notmatch '(?m)^        if:') 'FSR-only dispatch must receive the unconditional native preflight.'
    }
    $body = ($step.Groups['body'].Value -split '\r?\n' | ForEach-Object { $_ -replace '^          ', '' }) -join "`n"
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($body, [ref]$tokens, [ref]$errors)
    Assert-Preflight ($errors.Count -eq 0) "Optional workflow stage has PowerShell syntax errors: $name"
    $calls = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -eq './scripts/build_windows.ps1'
    }, $true))
    Assert-Preflight ($calls.Count -eq 1) "Optional workflow bypassed one canonical build call: $name"
    $parameters = @($calls[0].CommandElements | Where-Object { $_ -is [Management.Automation.Language.CommandParameterAst] } | ForEach-Object { $_.ParameterName })
    Assert-Preflight ($parameters -contains 'HostPreflight') "Optional workflow bypassed the early native preflight: $name"
    Assert-Preflight (($parameters -contains 'ConfigureOnly') -eq ($name -like 'Fast*')) "Optional workflow changed configure/full-build sequencing: $name"
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven host preflight ' + [Guid]::NewGuid().ToString('N'))
$saved = @{}
foreach ($name in @('VCPKG_DISABLE_METRICS', 'VCPKG_ROOT')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    New-Item -ItemType Directory "$temporary/scripts" | Out-Null
    Copy-Item -LiteralPath $BuildScript -Destination "$temporary/scripts/build_windows.ps1"
    Copy-Item -LiteralPath "$repoRoot/scripts/windows_common.ps1" -Destination "$temporary/scripts"
    Add-Content -LiteralPath "$temporary/scripts/windows_common.ps1" -Value @'
function Import-HeavenMsvcEnvironment {}
function Get-HeavenGitBash { return 'fixture-pins.exe' }
function Initialize-HeavenSdlSource { param([string]$BuildDir) return (Join-Path $BuildDir 'fixture SDL') }
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments = @(), [int]$ExpectedExitCode = 0)
    $name = [IO.Path]::GetFileName($Command)
    $global:HeavenPreflightCalls.Add(@{ name = $name; arguments = $Arguments })
    if ($name -eq 'cmake.exe' -and $Arguments -contains '-S' -and $global:HeavenPreflightCase -eq 'configure-failure') {
        throw 'fixture canonical configure failure'
    }
    if ($name -ne 'ctest.exe') { return }
    $build = $Arguments[[Array]::IndexOf($Arguments, '--test-dir') + 1]
    if ($Arguments -contains '--show-only=json-v1') {
        if ($global:HeavenPreflightCase -eq 'discovery-exit') { throw 'fixture discovery command failure' }
        if ($global:HeavenPreflightCase -eq 'malformed') { return 'not JSON' }
        $compiler = if ($global:HeavenPreflightCase -eq 'wrong-compiler') { '/fixture/clang++.exe' } else { '/fixture MSVC/cl.exe' }
        $source = "$global:HeavenPreflightRoot/benchmark/src/rhi/frame_generation/filament_depth_program_lifetime_test.py"
        if ($global:HeavenPreflightCase -eq 'wrong-source') { $source = '/other/filament_depth_program_lifetime_test.py' }
        $test = @{ name = 'heaven_filament_depth_program_lifetime'; command = @('python.exe', $source, '--cxx', $compiler, '--work-dir', "$build/depth-program-lifetime-test") }
        $tests = @($test)
        if ($global:HeavenPreflightCase -eq 'missing') { $tests = @() }
        if ($global:HeavenPreflightCase -eq 'wrong-name') { $test.name = 'heaven_wrong_test' }
        if ($global:HeavenPreflightCase -eq 'duplicate') { $tests += $test }
        return @{ tests = $tests } | ConvertTo-Json -Depth 5
    }
    New-Item -ItemType Directory -Force "$build/Testing/Temporary", "$build/depth-program-lifetime-test" | Out-Null
    Set-Content "$build/Testing/Temporary/LastTest.log" 'fixture last native test'
    Set-Content "$build/depth-program-lifetime-test/production-compile.log" 'fixture native compiler log'
    Set-Content "$build/depth-program-lifetime-test/receipt.json" '{}'
    Set-Content $Arguments[[Array]::IndexOf($Arguments, '--output-junit') + 1] '<testsuites/>'
    Write-Output 'fixture material-lifetime CTest output'
    if ($global:HeavenPreflightCase -eq 'tests') { throw 'fixture native material-lifetime failure' }
}
'@
    function global:git.exe { $global:LASTEXITCODE = 0; return 'fixture-revision' }
    function global:python.exe {}
    foreach ($case in @('success', 'configure-only', 'disabled-full', 'disabled-configure-only',
                        'missing', 'wrong-name', 'duplicate', 'wrong-compiler', 'wrong-source',
                        'malformed', 'discovery-exit', 'tests', 'configure-failure')) {
        $working = Join-Path $temporary $case
        New-Item -ItemType Directory "$working/scripts", "$working/packaging/windows", "$working/vcpkg/.git", "$working/benchmark/assets" | Out-Null
        Copy-Item "$temporary/scripts/build_windows.ps1", "$temporary/scripts/windows_common.ps1" "$working/scripts"
        Set-Content "$working/packaging/windows/vcpkg.json" '{"builtin-baseline":"fixture-revision"}'
        Set-Content "$working/benchmark/assets/fixture.txt" 'fixture asset'
        $global:HeavenPreflightCase = $case
        $global:HeavenPreflightRoot = $working
        $global:HeavenPreflightCalls = [Collections.Generic.List[object]]::new()
        $configureOnly = $case -in @('configure-only', 'disabled-configure-only')
        $enabled = $case -notlike 'disabled-*'
        $build = Join-Path $working 'build with spaces'
        $caught = $false
        try {
            & "$working/scripts/build_windows.ps1" -BuildDir $build -VcpkgRoot "$working/vcpkg" -Jobs 2 -ConfigureOnly:$configureOnly -HostPreflight:$enabled
        } catch { $caught = $true }
        $expectedFailure = $case -notin @('success', 'configure-only', 'disabled-full', 'disabled-configure-only')
        Assert-Preflight ($caught -eq $expectedFailure) "Wrong native preflight outcome: $case"
        $calls = @($global:HeavenPreflightCalls.ToArray())
        $configures = @($calls | Where-Object { $_.name -eq 'cmake.exe' -and $_.arguments -contains '-S' })
        $builds = @($calls | Where-Object { $_.name -eq 'cmake.exe' -and $_.arguments -contains '--build' })
        $discoveries = @($calls | Where-Object { $_.name -eq 'ctest.exe' -and $_.arguments -contains '--show-only=json-v1' })
        $executions = @($calls | Where-Object { $_.name -eq 'ctest.exe' -and $_.arguments -contains '--output-on-failure' })
        Assert-Preflight ($calls[0].name -eq 'fixture-pins.exe') "Preflight bypassed canonical source verification: $case"
        if (-not $enabled) {
            Assert-Preflight ($discoveries.Count -eq 0 -and $executions.Count -eq 0 -and
                $builds.Count -eq $(if ($configureOnly) { 0 } else { 2 })) "Default local/configure-only behavior changed: $case"
            continue
        }
        Assert-Preflight ($builds.Count -eq $(if ($case -eq 'success') { 2 } else { 0 })) "Failed or configure-only preflight compiled the renderer: $case"
        if ($case -eq 'configure-failure') {
            Assert-Preflight ($discoveries.Count -eq 0) 'Preflight ran before failed canonical configuration.'
            continue
        }
        Assert-Preflight ($discoveries.Count -eq 1 -and
            [Array]::IndexOf($calls, $configures[0]) -lt [Array]::IndexOf($calls, $discoveries[0])) "Preflight did not reuse completed canonical configuration: $case"
        foreach ($event in @($discoveries) + @($executions)) {
            $arguments = $event.arguments
            Assert-Preflight ($arguments[[Array]::IndexOf($arguments, '--test-dir') + 1] -eq $build -and
                $arguments -contains '--no-tests=error') "Preflight used another build or allowed missing tests: $case"
            Assert-Preflight ($arguments[[Array]::IndexOf($arguments, '-R') + 1] -ceq '^heaven_filament_depth_program_lifetime$') "Preflight selected unrelated tests: $case"
        }
        $executed = $case -in @('success', 'configure-only', 'tests')
        Assert-Preflight ($executions.Count -eq $(if ($executed) { 1 } else { 0 })) "Invalid discovery reached native test execution: $case"
        $phase = if ($configureOnly) { 'configure-only' } else { 'build' }
        $evidence = Join-Path $build "host-preflight/$phase"
        if ($expectedFailure) { Assert-Preflight (Test-Path "$evidence/failure.log") "Preflight failure evidence missing: $case" }
        if ($executed) {
            foreach ($name in @('host-tests.xml', 'ctest.log', 'LastTest.log', 'fixture/production-compile.log', 'fixture/receipt.json')) {
                Assert-Preflight (Test-Path (Join-Path $evidence $name)) "Preflight native/JUnit evidence missing: $case $name"
            }
            if ($case -eq 'success') {
                Assert-Preflight ([Array]::IndexOf($calls, $executions[0]) -lt [Array]::IndexOf($calls, $builds[0])) 'Preflight ran after renderer compilation.'
            }
        }
    }
    Write-Host 'Windows host preflight fixtures passed: canonical source/configuration, exact MSVC discovery, early failure/JUnit preservation, fsr3-only workflow and unchanged default/configure-only behavior (13 cases).'
} finally {
    foreach ($entry in $saved.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
    Remove-Item Function:git.exe, Function:python.exe -ErrorAction SilentlyContinue
    Remove-Variable HeavenPreflightCase, HeavenPreflightRoot, HeavenPreflightCalls -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
