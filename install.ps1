param(
    [string]$InstallRoot = (Join-Path $env:USERPROFILE '.codex\mcp\creo_safe'),
    [switch]$SkipBuild
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
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\cleanup_project_versions.ps1') -Destination $InstallRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'mcp\cleanup_project_model_versions.ps1') -Destination $InstallRoot -Force
Copy-Item -Path (Join-Path $distBin '*') -Destination $installBin -Force
Copy-Item -Path (Join-Path $repoRoot 'scripts\*.ps1') -Destination $installBin -Force

[pscustomobject]@{
    Ok = $true
    InstallRoot = $InstallRoot
    NextStep = 'Create creotk.dat and add the MCP block using the templates under config.'
} | ConvertTo-Json -Depth 3
