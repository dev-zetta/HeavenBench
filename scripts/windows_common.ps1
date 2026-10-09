# Shared native Windows build/package helpers. Requires PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([Parameter(Mandatory)][string]$Command, [string[]]$Arguments = @(),
          [int]$ExpectedExitCode = 0)
    & $Command @Arguments
    if ($LASTEXITCODE -ne $ExpectedExitCode) {
        throw "$Command failed with exit code $LASTEXITCODE (expected $ExpectedExitCode)"
    }
}

function Get-HeavenPackageVersion {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $projectText = Get-Content -Raw (Join-Path $RepoRoot 'benchmark/CMakeLists.txt')
    if ($projectText -notmatch 'project\(HeavenBench VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Cannot read HeavenBench version.'
    }
    return $Matches[1]
}

function Get-HeavenGitBash {
    $git = (Get-Command git.exe -ErrorAction Stop).Source
    $gitRoot = Split-Path (Split-Path $git -Parent) -Parent
    $bash = Join-Path $gitRoot 'bin/bash.exe'
    if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
        throw 'Install Git for Windows with its Bash executable (WSL Bash is not used).'
    }
    return $bash
}

function Get-HeavenWindowsBuildDirectory {
    param([string]$RepoRoot, [string]$Configuration = 'Release')
    # Keep build_paths.sh as the single owner of renderer defaults.
    Push-Location $RepoRoot
    try {
        $directory = & (Get-HeavenGitBash) --noprofile --norc -c 'REPO_ROOT="$PWD"; source scripts/build_paths.sh; cygpath -w "$(heaven_renderer_build_dir windows "$1")"' -- $Configuration
        if ($LASTEXITCODE -ne 0 -or -not $directory) { throw 'Cannot resolve Windows build directory.' }
        return [IO.Path]::GetFullPath(($directory | Select-Object -Last 1))
    } finally { Pop-Location }
}

function Initialize-HeavenSdlSource {
    param([Parameter(Mandatory)][string]$BuildDir)
    # Filament's bundled SDL 2.0.8 lacks the hint APIs used by HeavenBench.
    # Keep this archive/version in sync with the Android source pin.
    $version = '2.32.10'
    $sha256 = '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'
    $toolchains = Join-Path $BuildDir 'toolchains'
    $source = Join-Path $toolchains "SDL2-$version"
    $marker = Join-Path $source '.heaven-source-sha256'
    if (Test-Path -LiteralPath $source) {
        if (-not (Test-Path -LiteralPath $marker -PathType Leaf) -or
            (Get-Content -Raw -LiteralPath $marker).Trim() -ne $sha256 -or
            -not (Test-Path -LiteralPath "$source/CMakeLists.txt" -PathType Leaf)) {
            throw "SDL source directory is incomplete or has a different pin: $source"
        }
        return $source
    }
    $downloads = Join-Path $toolchains 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $archive = Join-Path $downloads "SDL2-$version.tar.gz"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        $temporary = "$archive.$([Guid]::NewGuid().ToString('N')).partial"
        Invoke-WebRequest -Uri "https://www.libsdl.org/release/SDL2-$version.tar.gz" -OutFile $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $sha256) {
            throw "SDL download checksum mismatch: $temporary"
        }
        Move-Item -LiteralPath $temporary -Destination $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha256) {
        throw "SDL archive checksum mismatch: $archive"
    }
    $extract = Join-Path $toolchains ('.sdl-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $extract | Out-Null
    Push-Location $extract
    try { Invoke-Checked cmake.exe @('-E', 'tar', 'xzf', $archive) }
    finally { Pop-Location }
    $unpacked = Join-Path $extract "SDL2-$version"
    if (-not (Test-Path -LiteralPath "$unpacked/CMakeLists.txt" -PathType Leaf)) {
        throw "SDL archive does not contain the expected source tree: $extract"
    }
    Set-Content -LiteralPath "$unpacked/.heaven-source-sha256" -Value $sha256 -Encoding ascii
    Move-Item -LiteralPath $unpacked -Destination $source
    Remove-Item -LiteralPath $extract -Recurse -Force
    return $source
}

function Import-HeavenMsvcEnvironment {
    if (-not $IsWindows) { throw 'Native Windows builds require Windows and Visual Studio 2022 17.13 or later.' }
    if ($env:MSYSTEM) { throw 'Run this helper in native PowerShell; pinned Filament rejects MSYS2 environments.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools and Windows SDK are required.' }
    $installation = & $vswhere -latest -products '*' -version '[17.13,)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'No Visual Studio 2022 17.13+ C++ x64 installation found.' }
    Import-Module (Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        $ninjaDirectory = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
        $env:PATH = "$ninjaDirectory;$env:PATH"
    }
    # Shell-only extraction tests use Git for Windows sh; keep MSVC tools first.
    $gitRoot = Split-Path (Split-Path (Get-HeavenGitBash) -Parent) -Parent
    $env:PATH += ";$(Join-Path $gitRoot 'usr/bin')"
    foreach ($tool in @('cl.exe', 'cmake.exe', 'ninja.exe', 'python.exe', 'git.exe')) {
        Get-Command $tool -ErrorAction Stop | Out-Null
    }
}
