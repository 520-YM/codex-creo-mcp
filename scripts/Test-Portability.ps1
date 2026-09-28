param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$RequireBuildEnvironment
)

$ErrorActionPreference = 'Stop'
$checks = [ordered]@{}
$checks.Node = [bool](Get-Command node -ErrorAction SilentlyContinue)
$checks.Git = [bool](Get-Command git -ErrorAction SilentlyContinue)
$checks.Server = Test-Path -LiteralPath (Join-Path $RepositoryRoot 'mcp\server.cjs')
$checks.WorkflowRules = Test-Path -LiteralPath (Join-Path $RepositoryRoot 'standards\data\workflow_rules.json')
$checks.V14PatchScript = Test-Path -LiteralPath (Join-Path $RepositoryRoot 'scripts\make_hot_internal_v14.ps1')
$checks.ConfigTemplate = Test-Path -LiteralPath (Join-Path $RepositoryRoot 'config\codex-mcp-config.example.toml')
$checks.PrivateSourcesIgnored = (Select-String -LiteralPath (Join-Path $RepositoryRoot '.gitignore') -SimpleMatch 'standards/source-private/' -Quiet)

if ($RequireBuildEnvironment) {
    $checks.CreoCommonFiles = [bool]$env:CREO_COMMON_FILES -and (Test-Path -LiteralPath $env:CREO_COMMON_FILES)
    $checks.ProToolkitHeader = $checks.CreoCommonFiles -and (Test-Path -LiteralPath (Join-Path $env:CREO_COMMON_FILES 'protoolkit\includes\ProToolkit.h'))
    $checks.VisualStudioDevCmd = [bool]$env:VSDEVCMD -and (Test-Path -LiteralPath $env:VSDEVCMD)
}

$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object Key)
[pscustomobject]@{
    Ok = $failed.Count -eq 0
    Checks = $checks
    Failed = $failed
    Note = 'This check does not install licensed PTC software or copy private CAD/company source files.'
} | ConvertTo-Json -Depth 5
if ($failed.Count -gt 0) { exit 1 }
