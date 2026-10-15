# Execute the production package-only/relocation orchestration with fake ZIP payloads.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
function Assert-Package([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven experimental packages ' + [Guid]::NewGuid().ToString('N'))
$names = @('PATH', 'SystemRoot', 'HEAVEN_USER_DIR', 'HEAVEN_DATA', 'HEAVEN_DEFAULT_DATA',
    'HEAVEN_RESOURCE_DIR', 'HEAVEN_SMOKE', 'HEAVEN_DLSS_PLUGIN_DIR', 'HEAVEN_DLSS_PROJECT_ID')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    New-Item -ItemType Directory "$temporary/scripts" | Out-Null
    Copy-Item "$repo/scripts/ci_windows.ps1" "$temporary/scripts"
    Set-Content "$temporary/scripts/windows_common.ps1" @'
function Import-HeavenMsvcEnvironment {}
function Get-HeavenPackageVersion { param($RepoRoot) return '1.2.3' }
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments = @(), [int]$ExpectedExitCode = 0)
    $name = [IO.Path]::GetFileName($Command)
    $global:ExperimentalCalls.Add(@{ name=$name; path=$env:PATH; plugin=$env:HEAVEN_DLSS_PLUGIN_DIR; arguments=$Arguments })
    if ($name -in @('ctest.exe', 'python.exe', 'pwsh.exe')) { throw 'Package-only unexpectedly built or reran host/preflight tests.' }
    if ($name -eq 'Launch-HeavenBench.cmd') {
        New-Item -ItemType Directory -Force $env:HEAVEN_USER_DIR | Out-Null
        $index = [Array]::IndexOf($Arguments, '--write-config')
        if ($index -ge 0) {
            $data = Join-Path (Split-Path $Command -Parent) 'assets'
            if ($env:HEAVEN_DATA) { $data=$env:HEAVEN_DATA }
            $dataIndex=[Array]::IndexOf($Arguments,'--data')
            if ($dataIndex -ge 0) { $data=$Arguments[$dataIndex+1] }
            @{data_dir=$data;frame_generation='off'} | ConvertTo-Json | Set-Content $Arguments[$index+1]
        }
    }
}
'@
    Set-Content "$temporary/scripts/package_windows.ps1" @'
param([string]$BuildDir,[string]$OutputDir,[string]$Version,[string]$ExperimentalProviders,[string]$SourceManifest)
if (-not $ExperimentalProviders -or -not $SourceManifest) { throw 'Experimental package arguments lost.' }
$global:ExperimentalCalls.Add(@{name='package';build=$BuildDir;providers=$ExperimentalProviders})
if ($global:ExperimentalCase -eq 'package-failure') { throw 'fixture package failure' }
$source=Get-Content -Raw $SourceManifest | ConvertFrom-Json
$name="HeavenBench-$Version-experimental-$ExperimentalProviders-$($source.source_sha256.Substring(0,12))-windows-x64"
$parent=Join-Path $OutputDir 'stage';$stage=Join-Path $parent $name
New-Item -ItemType Directory -Force "$stage/assets" | Out-Null
foreach ($file in @('heaven_benchmark.exe','heaven_asset_packer.exe','Launch-HeavenBench.cmd','assets/README.txt')) { Set-Content (Join-Path $stage $file) 'fixture' }
$provider=if($global:ExperimentalCase -eq 'wrong-provider'){'wrong'}else{$ExperimentalProviders}
$hash=if($global:ExperimentalCase -eq 'wrong-source'){'wrong'}else{$source.source_sha256}
@{experimental=$true;providers_built=$provider;source_archive_sha256=$hash;binary_sha256=(Get-FileHash "$stage/heaven_benchmark.exe" -Algorithm SHA256).Hash.ToLowerInvariant()} | ConvertTo-Json | Set-Content "$stage/experimental-frame-generation.json"
if($global:ExperimentalCase -eq 'runtime-plugin'){Set-Content "$stage/sl.interposer.dll" 'must be rejected'}
$files=@(Get-ChildItem $stage -File -Recurse | ForEach-Object {
    @{path=[IO.Path]::GetRelativePath($stage,$_.FullName);sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
if($global:ExperimentalCase -eq 'bad-manifest'){$files[0].sha256='wrong'}
@{files=$files}|ConvertTo-Json -Depth 4|Set-Content "$stage/package-manifest.json"
[IO.Compression.ZipFile]::CreateFromDirectory($parent,(Join-Path $OutputDir "$name.zip"))
Remove-Item $parent -Recurse -Force
'@
    Set-Content "$temporary/scripts/build_windows.ps1" "throw 'Package-only reached native build.'"
    $env:SystemRoot=Join-Path $temporary 'Windows'
    $env:HEAVEN_DLSS_PLUGIN_DIR='caller plugins';$env:HEAVEN_DLSS_PROJECT_ID='caller identifier'
    $initialPath=$env:PATH
    $cases=@('fsr3','dlss','both','wrong-provider','wrong-source','runtime-plugin','bad-manifest','package-failure')
    foreach($case in $cases){
        $global:ExperimentalCase=$case;$global:ExperimentalCalls=[Collections.Generic.List[object]]::new()
        $output=Join-Path $temporary $case;New-Item -ItemType Directory $output|Out-Null
        $manifest=Join-Path $output 'source-manifest.json';@{source_sha256=('a'*64)}|ConvertTo-Json|Set-Content $manifest
        $provider=if($case -in @('fsr3','dlss','both')){$case}else{'both'}
        $caught=$false
        try{& "$temporary/scripts/ci_windows.ps1" -PackageOnly -BuildDir "$temporary/build" -OutputDir $output -ExperimentalProviders $provider -SourceManifest $manifest}catch{$caught=$true}
        Assert-Package ($caught -eq ($case -notin @('fsr3','dlss','both'))) "Unexpected experimental package outcome: $case"
        Assert-Package ($env:PATH -ceq $initialPath -and $env:HEAVEN_DLSS_PLUGIN_DIR -ceq 'caller plugins' -and $env:HEAVEN_DLSS_PROJECT_ID -ceq 'caller identifier') "Caller environment leaked: $case"
        $calls=@($global:ExperimentalCalls.ToArray())
        Assert-Package (@($calls|Where-Object name -eq 'package').Count -eq 1) 'Existing build was not packaged exactly once.'
        foreach($call in @($calls|Where-Object name -eq 'heaven_benchmark.exe')){
            Assert-Package ($call.path -ceq "$env:SystemRoot/System32;$env:SystemRoot" -and -not $call.plugin) 'Relocation retained toolchain paths or proprietary plugin input.'
        }
    }
    $workflow=Get-Content -Raw "$repo/.github/workflows/frame-generation.yml"
    Assert-Package ($workflow.IndexOf('- name: Package and relocate experimental Windows build') -gt $workflow.IndexOf('- name: Compile optional native paths and run host contracts')) 'Packaging moved before SDK host gates.'
    Assert-Package ($workflow -match '\./scripts/ci_windows\.ps1 -PackageOnly') 'Experimental workflow bypassed canonical relocation verification.'
    # Exercise the actual packager's early label guard before any binary/DLL staging.
    Copy-Item "$repo/scripts/package_windows.ps1" "$temporary/scripts/package_windows.ps1" -Force
    $guardBuild = Join-Path $temporary 'guard build'
    New-Item -ItemType Directory $guardBuild | Out-Null
    foreach ($case in @('sdk-on', 'sdk-off', 'missing-manifest')) {
        $flag = if ($case -eq 'sdk-off') { 'OFF' } else { 'ON' }
        Set-Content (Join-Path $guardBuild 'CMakeCache.txt') "CMAKE_BUILD_TYPE:STRING=Release`nHEAVEN_SDL_SOURCE_DIR:PATH=$temporary`nHEAVEN_ENABLE_FSR3:BOOL=$flag`nHEAVEN_ENABLE_DLSS:BOOL=OFF"
        $caught = ''
        try {
            if ($case -eq 'missing-manifest') {
                & "$temporary/scripts/package_windows.ps1" -BuildDir $guardBuild -OutputDir "$temporary/guard output" -ExperimentalProviders fsr3
            } else {
                & "$temporary/scripts/package_windows.ps1" -BuildDir $guardBuild -OutputDir "$temporary/guard output"
            }
        } catch { $caught = $_.Exception.Message }
        $expected = switch ($case) {
            'sdk-on' { 'require explicit experimental packaging' }
            'sdk-off' { 'Missing executable' }
            'missing-manifest' { 'requires both provider selection' }
        }
        Assert-Package ($caught.Contains($expected)) "Actual packager label guard failed for $case : $caught"
    }
    Write-Host 'PASS: eight experimental package-only/relocation cases plus three actual packager label guards; provider/source/hash/plugin failures, no rebuild, environment restored and SDK host gates precede artifacts.'
} finally {
    foreach($entry in $saved.GetEnumerator()){[Environment]::SetEnvironmentVariable($entry.Key,$entry.Value,'Process')}
    Remove-Variable ExperimentalCase,ExperimentalCalls -Scope Global -ErrorAction SilentlyContinue
    if(Test-Path $temporary){Remove-Item $temporary -Recurse -Force}
}
