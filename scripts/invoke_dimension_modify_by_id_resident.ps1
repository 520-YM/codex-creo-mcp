param(
    [Parameter(Mandatory = $true)][string]$ExpectedModel,
    [Parameter(Mandatory = $true)][int]$FeatureId,
    [Parameter(Mandatory = $true)][string]$DimensionSymbol,
    [Parameter(Mandatory = $true)][double]$ExpectedValue,
    [Parameter(Mandatory = $true)][double]$NewValue,
    [string]$AssemblyModel = ''
)

$ErrorActionPreference = 'Stop'
$watch = [Diagnostics.Stopwatch]::StartNew()
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'creo_dim_' + [guid]::NewGuid().ToString('N'))
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

try {
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    $resultPath = Join-Path $tempRoot 'result.json'
    $commandPath = Join-Path $tempRoot 'command.txt'
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($value in @(
        'creo_dimension_modify_bridge.exe', $resultPath, $ExpectedModel,
        ('#' + $FeatureId), '1', $DimensionSymbol,
        $ExpectedValue.ToString([Globalization.CultureInfo]::InvariantCulture),
        $NewValue.ToString([Globalization.CultureInfo]::InvariantCulture))) {
        $lines.Add([string]$value)
    }
    if ($AssemblyModel) { $lines.Add($AssemblyModel) }
    [IO.File]::WriteAllText(
        $commandPath,
        [string]::Join([Environment]::NewLine, $lines),
        [Text.UTF8Encoding]::new($false))
    $reply = Send-CreoPipe ('EXECFILE|' + $commandPath)
    $watch.Stop()
    $result = if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else { $null }
    [pscustomobject]@{
        Ok = [bool]($result -and $result.ok)
        Result = $result
        Reply = ($reply | ConvertFrom-Json)
        ElapsedMs = $watch.ElapsedMilliseconds
    } | ConvertTo-Json -Depth 10 -Compress
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
