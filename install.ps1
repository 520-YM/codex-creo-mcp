param(
    [string]$InstallRoot = (Join-Path $env:USERPROFILE '.codex\mcp\creo_safe'),
    [switch]$SkipBuild,
    [string]$RegisterConfigProPath = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$distBin = Join-Path $repoRoot 'dist\bin'

if (-not $SkipBuild) {
    & (Join-Path $repoRoot 'build_all.cmd')
    if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
}
if (-not (Test-Path -LiteralPath $distBin -PathType Container)) {
    throw 'dist\bin does not exist. Build the native bridges first.'
}

$installBin = Join-Path $InstallRoot 'bin'
$installText = Join-Path $InstallRoot 'text'
$installOutput = Join-Path $InstallRoot 'output'
New-Item -ItemType Directory -Path $InstallRoot,$installBin,$installText,$installOutput -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\server.cjs') -Destination $InstallRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\session-binding.cjs') -Destination $InstallRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\wall-mount-orchestrator.cjs') -Destination $InstallRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\cleanup_project_versions.ps1') -Destination $InstallRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\cleanup_project_model_versions.ps1') -Destination $InstallRoot -Force
Copy-Item -Path (Join-Path $distBin '*.exe') -Destination $installBin -Force
$residentDll = Join-Path $distBin 'creo_safe_resident_internal_v14.dll'
$residentLoader = Join-Path $distBin 'creo_internal_resident_loader_v14.exe'
foreach ($required in @($residentDll, $residentLoader)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Internal Creo runtime was not built: $required"
    }
}
Copy-Item -LiteralPath $residentDll -Destination $installBin -Force
Copy-Item -LiteralPath $residentLoader -Destination $installBin -Force
Copy-Item -Path (Join-Path $repoRoot 'scripts\*.ps1') -Destination $installBin -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'standards') -Destination $InstallRoot -Recurse -Force

$residentDat = Join-Path $InstallRoot 'CreoSafeResident.dat'
$datLines = @(
    'name CreoSafeResident',
    'startup dll',
    'revision 25',
    ('exec_file ' + (Join-Path $installBin 'creo_safe_resident_internal_v14.dll')),
    ('text_dir ' + $installText),
    'delay_start false',
    'allow_stop true',
    'fail_tol true',
    'end'
)
[IO.File]::WriteAllLines($residentDat, $datLines, [Text.UTF8Encoding]::new($false))

$configChanged = $false
$configBackup = $null
if ($RegisterConfigProPath) {
    $resolvedConfig = [IO.Path]::GetFullPath($RegisterConfigProPath)
    if (-not (Test-Path -LiteralPath $resolvedConfig -PathType Leaf)) {
        throw "config.pro was not found: $resolvedConfig"
    }
    $existing = [IO.File]::ReadAllLines($resolvedConfig)
    $registration = "protkdat $residentDat"
    if (-not ($existing | Where-Object { $_.Trim() -ieq $registration })) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $configBackup = "$resolvedConfig.codex-before-creo-safe-$stamp.bak"
        Copy-Item -LiteralPath $resolvedConfig -Destination $configBackup
        [IO.File]::AppendAllText(
            $resolvedConfig,
            [Environment]::NewLine + $registration + [Environment]::NewLine,
            [Text.UTF8Encoding]::new($false))
        $configChanged = $true
    }
}

[pscustomobject]@{
    Ok = $true
    InstallRoot = $InstallRoot
    ResidentMode = 'normal_creo_startup_independent_dat'
    ResidentVersion = 'v14'
    CreoConfigChanged = $configChanged
    CreoConfigBackup = $configBackup
    WJT276Changed = $false
    ToolkitRegistryFile = $residentDat
    NextStep = if ($RegisterConfigProPath) {
        'Restart Creo normally, select a working directory, then run the read-only first-command handshake.'
    } else {
        "Review $residentDat, then explicitly register it in the Creo startup configuration before normal Creo startup."
    }
} | ConvertTo-Json -Depth 3
