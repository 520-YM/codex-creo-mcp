param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path $RepositoryRoot 'build\internal_v11'
$outputRoot = Join-Path $RepositoryRoot 'build\internal_v14_hot'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$dllSource = Join-Path $sourceRoot 'creo_safe_resident_internal_v11.dll'
$loaderSource = Join-Path $sourceRoot 'creo_internal_resident_loader_v11.exe'
$dllOutput = Join-Path $outputRoot 'creo_safe_resident_internal_v14.dll'
$loaderOutput = Join-Path $outputRoot 'creo_internal_resident_loader_v14.exe'
Copy-Item -LiteralPath $dllSource -Destination $dllOutput -Force
Copy-Item -LiteralPath $loaderSource -Destination $loaderOutput -Force

function Replace-Bytes {
    param([string]$Path, [byte[]]$Old, [byte[]]$New)
    if ($Old.Length -ne $New.Length) { throw 'Replacement byte lengths differ.' }
    $data = [IO.File]::ReadAllBytes($Path); $count = 0
    for ($i = 0; $i -le $data.Length - $Old.Length; $i++) {
        $matched = $true
        for ($j = 0; $j -lt $Old.Length; $j++) {
            if ($data[$i + $j] -ne $Old[$j]) { $matched = $false; break }
        }
        if ($matched) { [Array]::Copy($New, 0, $data, $i, $New.Length); $count++; $i += $Old.Length - 1 }
    }
    if ($count -lt 1) { throw "Pattern not found in $Path" }
    [IO.File]::WriteAllBytes($Path, $data); return $count
}

$unicode = [Text.Encoding]::Unicode; $ascii = [Text.Encoding]::ASCII
$changes = [ordered]@{}
$changes.DllDispatch = Replace-Bytes $dllOutput ($unicode.GetBytes('CodexCreoInternalV11Dispatch')) ($unicode.GetBytes('CodexCreoInternalV14Dispatch'))
$changes.DllPipe = Replace-Bytes $dllOutput ($unicode.GetBytes('codex_creo_internal_v11_')) ($unicode.GetBytes('codex_creo_internal_v14_'))
$changes.DllProtocol = Replace-Bytes $dllOutput ($ascii.GetBytes('creo-safe-internal-v11')) ($ascii.GetBytes('creo-safe-internal-v14'))
$changes.LoaderAppName = Replace-Bytes $loaderOutput ($unicode.GetBytes('CodexCreoInternalV11')) ($unicode.GetBytes('CodexCreoInternalV14'))
[pscustomobject]@{Ok=$true;Dll=$dllOutput;Loader=$loaderOutput;Changes=$changes} | ConvertTo-Json -Depth 5 -Compress
