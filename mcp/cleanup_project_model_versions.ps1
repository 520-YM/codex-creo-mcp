param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectDirectory,

    [Parameter(Mandatory = $true)]
    [string]$KeepFilesBase64
)

$ErrorActionPreference = 'Stop'

function Normalize-FullPath {
    param([string]$Value)
    return [System.IO.Path]::GetFullPath($Value).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

$root = Normalize-FullPath $ProjectRoot
$project = Normalize-FullPath $ProjectDirectory
$rootPrefix = $root + [System.IO.Path]::DirectorySeparatorChar

if (-not $project.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'ProjectDirectory is outside ProjectRoot.'
}

$relativeProject = $project.Substring($rootPrefix.Length)
if ([string]::IsNullOrWhiteSpace($relativeProject) -or
    $relativeProject -eq '.' -or
    $relativeProject.StartsWith('..') -or
    $relativeProject.Contains([System.IO.Path]::DirectorySeparatorChar) -or
    $relativeProject.Contains([System.IO.Path]::AltDirectorySeparatorChar)) {
    throw 'ProjectDirectory must be one direct child of ProjectRoot.'
}

if (-not (Test-Path -LiteralPath $project -PathType Container)) {
    throw 'ProjectDirectory does not exist.'
}

$keepFilesJson = [System.Text.Encoding]::UTF8.GetString(
    [System.Convert]::FromBase64String($KeepFilesBase64))
$keepFiles = [string[]]($keepFilesJson | ConvertFrom-Json)
if ($keepFiles.Count -lt 1 -or $keepFiles.Count -gt 8) {
    throw 'KeepFilesBase64 must decode to between 1 and 8 file names.'
}

$families = @{}
foreach ($keepFile in $keepFiles) {
    if ($keepFile -isnot [string] -or
        [string]::IsNullOrWhiteSpace($keepFile) -or
        [System.IO.Path]::GetFileName($keepFile) -ne $keepFile) {
        throw 'Each keep file must be one exact file name.'
    }

    $match = [regex]::Match(
        $keepFile,
        '^(?<stem>[A-Za-z0-9_-]{1,80})\.(?<ext>prt|asm)(?:\.(?<version>\d+))?$',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        throw "Unsupported Creo keep file: $keepFile"
    }

    $keepPath = Normalize-FullPath (Join-Path $project $keepFile)
    if ([System.IO.Path]::GetDirectoryName($keepPath) -ne $project -or
        -not (Test-Path -LiteralPath $keepPath -PathType Leaf)) {
        throw "Keep file does not exist in the project directory: $keepFile"
    }

    $family = ('{0}.{1}' -f $match.Groups['stem'].Value, $match.Groups['ext'].Value).ToLowerInvariant()
    if ($families.ContainsKey($family) -and
        $families[$family] -ne $keepFile) {
        throw "More than one keep file was supplied for family $family."
    }
    $families[$family] = $keepFile
}

Add-Type -AssemblyName Microsoft.VisualBasic
$moved = New-Object System.Collections.Generic.List[string]

foreach ($family in $families.Keys) {
    $keepFile = $families[$family]
    $familyPattern = '^' + [regex]::Escape($family) + '(?:\.\d+)?$'
    $candidates = Get-ChildItem -LiteralPath $project -File | Where-Object {
        $_.Name.ToLowerInvariant() -match $familyPattern
    }
    foreach ($candidate in $candidates) {
        if ($candidate.Name -eq $keepFile) {
            continue
        }
        $candidatePath = Normalize-FullPath $candidate.FullName
        if ([System.IO.Path]::GetDirectoryName($candidatePath) -ne $project) {
            throw "Candidate escaped the project directory: $candidatePath"
        }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
            $candidatePath,
            [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
            [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
        $moved.Add($candidate.Name)
    }
}

[ordered]@{
    ok = $true
    destructive = $true
    recovery = 'Windows Recycle Bin'
    project_directory = $project
    kept_files = @($keepFiles)
    moved_to_recycle_bin = @($moved)
    moved_count = $moved.Count
} | ConvertTo-Json -Compress
