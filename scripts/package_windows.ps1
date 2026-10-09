<#
.SYNOPSIS
Stage a portable Windows x64 ZIP, including runtime DLLs, resources and licenses.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$OutputDir = '',
    [string]$Version = ''
)
. "$PSScriptRoot/windows_common.ps1"
$repoRoot = Split-Path $PSScriptRoot -Parent
Import-HeavenMsvcEnvironment
if (-not $BuildDir) { $BuildDir = Get-HeavenWindowsBuildDirectory $repoRoot }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'build/packages/windows' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (-not $Version) {
    $Version = Get-HeavenPackageVersion $repoRoot
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([.-][A-Za-z0-9.-]+)?$') { throw 'Invalid package version.' }
$cache = Get-Content -Raw "$BuildDir/CMakeCache.txt"
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'Only Release builds can be packaged.' }
if ($cache -notmatch '(?m)^HEAVEN_SDL_SOURCE_DIR:PATH=(.+)\r?$') { throw 'Missing configured SDL source directory.' }
$sdlSource = $Matches[1].Trim()
$triplet = Join-Path $BuildDir 'vcpkg_installed/x64-windows-static'
$runtimeDir = Join-Path $triplet 'bin'
$packageName = "HeavenBench-$Version-windows-x64"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$stagingParent = Join-Path $OutputDir ('.stage-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $stagingParent $packageName
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($relative in @('heaven_benchmark.exe', 'heaven_asset_packer.exe', 'tools/uniginex.exe', 'tools/mesh2obj.exe')) {
    $source = Join-Path $BuildDir $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing executable: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}
# Filament can load Vulkan dynamically, so include it even when it is absent
# from the EXE's import table. GPU drivers themselves remain system-installed.
Copy-Item -LiteralPath (Join-Path $runtimeDir 'vulkan-1.dll') -Destination $stage
$searchDirectories = @($stage, $BuildDir, (Join-Path $BuildDir 'tools'), $runtimeDir)
$pending = [Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $stage -File | ForEach-Object { $pending.Enqueue($_.FullName) }
$inspected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if (-not $inspected.Add($binary)) { continue }
    $imports = & dumpbin.exe /NOLOGO /DEPENDENTS $binary
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect DLL dependencies: $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { continue }
        $name = $Matches[1]
        if ($name -match '^(api-ms-|ext-ms-)') { continue }
        $destination = Join-Path $stage $name
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            $pending.Enqueue($destination)
            continue
        }
        $source = $null
        foreach ($directory in $searchDirectories) {
            $candidate = Join-Path $directory $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $source = $candidate; break }
        }
        if ($source) {
            Copy-Item -LiteralPath $source -Destination $destination
            $pending.Enqueue($destination)
        } elseif ($name -match '^(vcruntime|msvcp|msvcr|concrt|vcomp).*\.dll$') {
            throw "Unbundled Visual C++ runtime $name imported by $binary; the package requires static CRT builds."
        } elseif (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$name") -PathType Leaf)) {
            throw "Unresolved runtime DLL $name imported by $binary"
        }
    }
}
Copy-Item -LiteralPath "$repoRoot/benchmark/assets" -Destination "$stage/resources" -Recurse
Copy-Item -LiteralPath "$repoRoot/packaging/windows/Launch-HeavenBench.cmd" -Destination $stage
Copy-Item -LiteralPath "$repoRoot/packaging/windows/README.txt" -Destination $stage
Copy-Item -LiteralPath "$repoRoot/tools/extraction/assemble_scene.py" -Destination $stage
New-Item -ItemType Directory "$stage/docs" | Out-Null
Copy-Item -LiteralPath "$repoRoot/docs/ZIP_ASSET_STORAGE.md" -Destination "$stage/docs"
New-Item -ItemType Directory "$stage/assets" | Out-Null
Copy-Item -LiteralPath "$repoRoot/packaging/windows/ASSETS.txt" -Destination "$stage/assets/README.txt"
Invoke-Checked python.exe @("$PSScriptRoot/collect_licenses.py", '--output', "$stage/licenses",
    '--dependency-dir', "vcpkg=$triplet/share", '--dependency-dir', "SDL2=$sdlSource")
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    @{ path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/');
       sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
@{ version = $Version; platform = 'windows-x64'; cpu = 'portable'; files = $files } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$stage/package-manifest.json" -Encoding utf8
$archive = Join-Path $OutputDir "$packageName.zip"
$temporaryArchive = Join-Path $OutputDir ('.archive-' + [Guid]::NewGuid().ToString('N') + '.zip')
[IO.Compression.ZipFile]::CreateFromDirectory($stagingParent, $temporaryArchive,
    [IO.Compression.CompressionLevel]::Optimal, $false)
Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $packageName.zip" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
# Remove only the unique staging tree created by this invocation after success.
Remove-Item -LiteralPath $stagingParent -Recurse -Force
Write-Host "Packaged $archive"
