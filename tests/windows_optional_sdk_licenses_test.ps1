# Execute the production packager's notice selector without MSVC or packaging.
[CmdletBinding()]
param([switch]$NegativeSdkNoticesOmitted)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $repo 'scripts/package_windows.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Get-HeavenOptionalSdkNoticeArguments'
}, $true)
if (-not $definition) { throw 'Production optional-SDK notice selector is missing.' }
$code = $definition.Extent.Text
if ($NegativeSdkNoticesOmitted) {
    $code = $code.Replace("return @('--notice-file', `"Streamline=`$path`")", 'return @()')
}
. ([scriptblock]::Create($code))
$collector = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq 'Invoke-Checked' -and
    $node.Extent.Text.Contains('collect_licenses.py')
}, $true)
if (-not $collector -or -not $collector.Extent.Text.Contains('$optionalSdkNotices')) {
    throw 'Production package does not pass selected SDK notices to the collector.'
}
function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('heaven SDK licenses ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
try {
    $sdk = Join-Path $temporary 'SDK source with spaces'
    $headers = Join-Path $temporary 'verified headers with spaces'
    $notices = @{
        'Streamline' = Join-Path $headers 'license.txt'
    }
    foreach ($entry in $notices.GetEnumerator()) {
        New-Item -ItemType Directory -Force (Split-Path $entry.Value -Parent) | Out-Null
        [IO.File]::WriteAllText($entry.Value, "Exact $($entry.Key) framework/source notice`n")
    }
    [IO.File]::WriteAllText((Join-Path $headers 'sl.interposer.dll'), 'must never be collected')
    [IO.File]::WriteAllText((Join-Path $headers 'nvngx_dlssg.dll'), 'must never be collected')
    $sources = "HEAVEN_FSR31_SOURCE:PATH=$sdk`nHEAVEN_STREAMLINE_HEADERS:PATH=$headers`n"
    Assert-That (@(Get-HeavenOptionalSdkNoticeArguments -Cache $sources).Count -eq 0) 'Old SDK-free cache selected notices.'
    foreach ($falseValue in @('', 'OFF', '0', 'FALSE', 'NO', 'N', 'IGNORE', 'NOTFOUND', 'SDK-NOTFOUND')) {
        $cache = "HEAVEN_ENABLE_FSR3:BOOL=$falseValue`nHEAVEN_ENABLE_DLSS:BOOL=$falseValue`n$sources"
        Assert-That (@(Get-HeavenOptionalSdkNoticeArguments -Cache $cache).Count -eq 0) 'Disabled SDK changed package inputs.'
    }
    $fsrCache = "HEAVEN_ENABLE_FSR3:BOOL=ON`nHEAVEN_ENABLE_DLSS:BOOL=OFF`n$sources"
    $fsr = @(Get-HeavenOptionalSdkNoticeArguments -Cache $fsrCache)
    Assert-That ($fsr.Count -eq 0) 'FSR added duplicate notices or a new fetched-source requirement.'
    $dlssCache = "HEAVEN_ENABLE_FSR3:BOOL=OFF`nHEAVEN_ENABLE_DLSS:BOOL=TRUE`n$sources"
    $dlss = @(Get-HeavenOptionalSdkNoticeArguments -Cache $dlssCache)
    Assert-That ($dlss.Count -eq 2 -and $dlss[1] -eq "Streamline=$($notices['Streamline'])") 'DLSS package did not select exactly its framework notice.'
    $bothCache = "HEAVEN_ENABLE_FSR3:BOOL=YES`nHEAVEN_ENABLE_DLSS:BOOL=1`n$sources"
    $both = @(Get-HeavenOptionalSdkNoticeArguments -Cache $bothCache)
    Assert-That ($both.Count -eq 2) 'Combined SDK package did not select only the missing Streamline notice.'
    foreach ($entry in $notices.GetEnumerator()) {
        $bytes = [IO.File]::ReadAllBytes($entry.Value)
        Remove-Item -LiteralPath $entry.Value
        $failed = $false
        try { Get-HeavenOptionalSdkNoticeArguments -Cache $bothCache | Out-Null }
        catch { $failed = $_.Exception.Message.Contains("Missing required $($entry.Key) notice") }
        Assert-That $failed "Missing enabled $($entry.Key) notice did not fail clearly."
        [IO.File]::WriteAllBytes($entry.Value, $bytes)
    }
    $failed = $false
    try { Get-HeavenOptionalSdkNoticeArguments -Cache 'HEAVEN_ENABLE_DLSS:BOOL=ON' | Out-Null }
    catch { $failed = $_.Exception.Message.Contains('requires configured HEAVEN_STREAMLINE_HEADERS') }
    Assert-That $failed 'Enabled SDK without its configured source path was accepted.'
    # Run the actual collector with the production selector's arguments. This
    # copies notice texts only; it neither stages EXEs nor creates a package.
    $python = if ($IsWindows) { 'python.exe' } else { 'python3' }
    $baseline = Join-Path $temporary 'baseline notices'
    $fsrOutput = Join-Path $temporary 'FSR notices'
    foreach ($selection in @(@{ Output = $baseline; Arguments = @() }, @{ Output = $fsrOutput; Arguments = $fsr })) {
        & $python (Join-Path $repo 'scripts/collect_licenses.py') --output $selection.Output @($selection.Arguments)
        if ($LASTEXITCODE -ne 0) { throw 'Baseline/FSR-only collector failed.' }
    }
    Assert-That ([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $baseline 'MANIFEST.json'))) -ceq
        [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $fsrOutput 'MANIFEST.json')))) 'FSR-only changed the default collector manifest bytes.'
    $output = Join-Path $temporary 'collected notices'
    & $python (Join-Path $repo 'scripts/collect_licenses.py') --output $output @both
    if ($LASTEXITCODE -ne 0) { throw 'Actual license collector rejected selected SDK inputs.' }
    $manifest = Get-Content -Raw (Join-Path $output 'MANIFEST.json') | ConvertFrom-Json
    $sdkEntries = @($manifest.files | Where-Object { $_.path -match '^dependencies/' })
    Assert-That ($sdkEntries.Count -eq 1) 'Collector copied unselected SDK files.'
    Assert-That (@($manifest.files | Where-Object { $_.path -match '\.dll$' }).Count -eq 0) 'Collector copied a runtime DLL.'
    foreach ($entry in $notices.GetEnumerator()) {
        $relative = "dependencies/$($entry.Key)/$([IO.Path]::GetFileName($entry.Value))"
        $saved = @( $sdkEntries | Where-Object path -eq $relative )
        $expected = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-That ($saved.Count -eq 1 -and $saved[0].sha256 -eq $expected) "Collected $($entry.Key) notice hash changed."
        Assert-That ((Get-FileHash -LiteralPath (Join-Path $output $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expected) 'Staged SDK notice bytes changed.'
    }
    Write-Host 'PASS: actual Windows notice selector, SDK-off/FSR manifest identity, exact enabled Streamline notice, missing-notice failures, collector hashes and no runtime DLLs; no package/MSVC/GPU'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
