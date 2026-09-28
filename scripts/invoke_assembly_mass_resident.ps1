param(
    [Parameter(Mandatory = $true)][string]$AssemblyFile,
    [Parameter(Mandatory = $true)][string]$ExpectedAssembly,
    [Parameter(Mandatory = $true)][string]$ReturnFile,
    [Parameter(Mandatory = $true)][string]$ReturnModel
)

$ErrorActionPreference = 'Stop'
$watch = [Diagnostics.Stopwatch]::StartNew()
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'creo_asm_mass_' + [guid]::NewGuid().ToString('N'))
$creoProcesses = @(Get-Process -Name xtop -ErrorAction SilentlyContinue)
if ($creoProcesses.Count -ne 1) { throw 'Exactly one Creo process is required.' }
$pipeName = 'codex_creo_internal_v11_' + $creoProcesses[0].Id

function Send-CreoPipe {
    param([string]$Message, [int]$ConnectTimeout = 1000)
    $pipe = [IO.Pipes.NamedPipeClientStream]::new(
        '.', $pipeName, [IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect($ConnectTimeout)
        $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
        $bytes = [Text.Encoding]::UTF8.GetBytes($Message + "`n")
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $buffer = New-Object byte[] 4096
        $response = [IO.MemoryStream]::new()
        do {
            $count = $pipe.Read($buffer, 0, $buffer.Length)
            if ($count -gt 0) { $response.Write($buffer, 0, $count) }
        } while (-not $pipe.IsMessageComplete)
        return [Text.Encoding]::UTF8.GetString($response.ToArray()).Trim()
    }
    finally { $pipe.Dispose() }
}

function Write-Utf8NoBom {
    param([string]$Path, [string[]]$Lines)
    [IO.File]::WriteAllText(
        $Path,
        [string]::Join([Environment]::NewLine, $Lines),
        [Text.UTF8Encoding]::new($false))
}

try {
    foreach ($path in @($AssemblyFile, $ReturnFile)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Model file not found: $path"
        }
    }
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    $openPath = Join-Path $tempRoot 'open.json'
    $massPath = Join-Path $tempRoot 'mass.json'
    $commandPath = Join-Path $tempRoot 'command.txt'
    $returnPath = Join-Path $tempRoot 'return.json'

    $openReply = Send-CreoPipe (
        'DISPLAY|' + $openPath + '|' + [IO.Path]::GetFullPath($AssemblyFile) +
        '|' + $ExpectedAssembly)
    Write-Utf8NoBom $commandPath @('creo_mass_properties_bridge.exe', $massPath)
    $massReply = Send-CreoPipe ('EXECFILE|' + $commandPath)
    $returnReply = Send-CreoPipe (
        'DISPLAY|' + $returnPath + '|' + [IO.Path]::GetFullPath($ReturnFile) +
        '|' + $ReturnModel)

    $watch.Stop()
    $mass = if (Test-Path -LiteralPath $massPath -PathType Leaf) {
        Get-Content -LiteralPath $massPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else { $null }
    [pscustomobject]@{
        Ok = [bool]($mass -and $mass.ok)
        Assembly = $ExpectedAssembly
        Mass = $mass
        OpenReply = ($openReply | ConvertFrom-Json)
        MassReply = ($massReply | ConvertFrom-Json)
        ReturnReply = ($returnReply | ConvertFrom-Json)
        ElapsedMs = $watch.ElapsedMilliseconds
    } | ConvertTo-Json -Depth 8 -Compress
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
