param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectDirectory,

    [Parameter(Mandatory = $true)]
    [string]$KeepFile
)

$ErrorActionPreference = 'Stop'

$rootPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
$projectPath = [System.IO.Path]::GetFullPath($ProjectDirectory).TrimEnd('\')
$keepPath = [System.IO.Path]::GetFullPath($KeepFile)

if (-not $projectPath.StartsWith(
        $rootPath + '\',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Project directory escaped the configured project root: $projectPath"
}
if (-not $keepPath.StartsWith(
        $projectPath + '\',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Keep file escaped the project directory: $keepPath"
}
if (-not (Test-Path -LiteralPath $keepPath -PathType Leaf)) {
    throw "Keep file does not exist: $keepPath"
}
if (-not [System.Text.RegularExpressions.Regex]::IsMatch(
        $keepPath,
        '\.prt(?:\.\d+)?$',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
    throw "Keep file is not a Creo part: $keepPath"
}

$targets = @(
    Get-ChildItem -LiteralPath $projectPath -File |
        Where-Object {
            $_.Name -match '\.prt(?:\.\d+)?$' -and
            -not [System.IO.Path]::GetFullPath($_.FullName).Equals(
                $keepPath,
                [System.StringComparison]::OrdinalIgnoreCase)
        }
)

foreach ($target in $targets) {
    $targetPath = [System.IO.Path]::GetFullPath($target.FullName)
    if (-not $targetPath.StartsWith(
            $projectPath + '\',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Delete target escaped the project directory: $targetPath"
    }
}

Add-Type -AssemblyName Microsoft.VisualBasic
$moved = @()
foreach ($target in $targets) {
    [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
        $target.FullName,
        [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
        [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
    $moved += $target.Name
}

[ordered]@{
    ok = $true
    project_directory = $projectPath
    kept_file = $keepPath
    moved_to_recycle_bin = $moved.Count
    moved_files = $moved
    recoverable = $true
} | ConvertTo-Json -Compress -Depth 4
