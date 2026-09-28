param(
    [Parameter(Mandatory = $true)][string]$ModelFile
)

$ErrorActionPreference = 'Stop'
$watch = [Diagnostics.Stopwatch]::StartNew()
$pipeName = $null
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'creo_mass_' + [guid]::NewGuid().ToString('N'))

if ($env:CREO_INTERNAL_PIPE_NAME) {
    $pipeName = $env:CREO_INTERNAL_PIPE_NAME -replace '^\\\\\.\\pipe\\', ''
} else {
    $creoProcesses = @(Get-Process -Name xtop -ErrorAction SilentlyContinue)
    if ($creoProcesses.Count -ne 1) {
        throw 'Exactly one Creo process is required when no internal pipe is supplied.'
    }
    $pipeName = 'codex_creo_internal_v11_' + $creoProcesses[0].Id
}

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

try {
    if (-not (Test-Path -LiteralPath $ModelFile -PathType Leaf)) {
        throw "Model file not found: $ModelFile"
    }
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    $resultPath = Join-Path $tempRoot 'result.json'
    $commandPath = Join-Path $tempRoot 'command.txt'
    $commandLines = @(
        'creo_mass_properties_bridge.exe'
        $resultPath
        [IO.Path]::GetFullPath($ModelFile)
    )
    [IO.File]::WriteAllText(
        $commandPath,
        [string]::Join([Environment]::NewLine, $commandLines),
        [Text.UTF8Encoding]::new($false))

    $reply = Send-CreoPipe ('EXECFILE|' + $commandPath)
    $watch.Stop()
    $result = if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else { $null }
    [pscustomobject]@{
        Ok = [bool]($result -and $result.ok)
        ModelFile = [IO.Path]::GetFileName($ModelFile)
        Result = $result
        Reply = ($reply | ConvertFrom-Json)
        ElapsedMs = $watch.ElapsedMilliseconds
    } | ConvertTo-Json -Depth 8 -Compress
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
