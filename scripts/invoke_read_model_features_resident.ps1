param(
    [Parameter(Mandatory = $true)][string]$ExpectedModel,
    [ValidateSet('prt', 'asm')][string]$Extension = 'asm',
    [string]$NamePattern = ''
)

$ErrorActionPreference = 'Stop'
$totalWatch = [Diagnostics.Stopwatch]::StartNew()
$pipeName = 'creo_safe_flat_wall_v1'
$tempPaths = [Collections.Generic.List[string]]::new()

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

function Read-Basic {
    $path = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_basic_' + [guid]::NewGuid().ToString('N') + '.json')
    $tempPaths.Add($path)
    $null = Send-CreoPipe ('BASIC|' + $path)
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Find-LatestCreoFile {
    param([string]$Directory, [string]$Stem, [string]$FileExtension)
    $pattern = '^' + [regex]::Escape($Stem) + '\.' +
        [regex]::Escape($FileExtension) + '(?:\.(\d+))?$'
    return Get-ChildItem -LiteralPath $Directory -File | ForEach-Object {
        if ($_.Name -match $pattern) {
            [pscustomobject]@{
                File = $_
                Version = if ($Matches[1]) { [int]$Matches[1] } else { 0 }
            }
        }
    } | Sort-Object Version -Descending | Select-Object -First 1
}

try {
    $connectionWatch = [Diagnostics.Stopwatch]::StartNew()
    $pong = Send-CreoPipe 'PING' 500
    $connectionWatch.Stop()

    $basicWatch = [Diagnostics.Stopwatch]::StartNew()
    $before = Read-Basic
    $workingDirectory = [string]$before.working_directory
    $topName = [string]$before.current_model.name
    $topType = [string]$before.current_model.model_type
    $basicWatch.Stop()
    $latest = Find-LatestCreoFile $workingDirectory $ExpectedModel $Extension
    if (-not $latest) { throw "Model $ExpectedModel.$Extension was not found." }

    $displayResultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_display_' + [guid]::NewGuid().ToString('N') + '.json')
    $featurePath = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_features_' + [guid]::NewGuid().ToString('N') + '.json')
    $returnResultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_return_' + [guid]::NewGuid().ToString('N') + '.json')
    $tempPaths.Add($displayResultPath)
    $tempPaths.Add($featurePath)
    $tempPaths.Add($returnResultPath)

    $openWatch = [Diagnostics.Stopwatch]::StartNew()
    $openReply = Send-CreoPipe (
        'DISPLAY|' + $displayResultPath + '|' + $latest.File.FullName + '|' + $ExpectedModel)
    $openWatch.Stop()

    $featureWatch = [Diagnostics.Stopwatch]::StartNew()
    $featureReply = Send-CreoPipe ('FEATURES|' + $featurePath)
    $features = Get-Content -LiteralPath $featurePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $featureWatch.Stop()

    $returnReply = $null
    $returnWatch = [Diagnostics.Stopwatch]::StartNew()
    if ($topName -and $topName -ine $ExpectedModel) {
        $topExtension = if ($topType -ieq 'assembly') { 'asm' } else { 'prt' }
        $topFile = Find-LatestCreoFile $workingDirectory $topName $topExtension
        if ($topFile) {
            $returnReply = Send-CreoPipe (
                'DISPLAY|' + $returnResultPath + '|' + $topFile.File.FullName + '|' + $topName)
        }
    }
    $returnWatch.Stop()
    $after = Read-Basic

    $matches = if ($NamePattern) {
        @($features.features | Where-Object { $_.name -match $NamePattern })
    } else { @($features.features) }
    $totalWatch.Stop()
    [pscustomobject]@{
        Ok = $true
        WorkingDirectory = $workingDirectory
        Model = $ExpectedModel
        ModelFile = $latest.File.Name
        FeatureCount = $features.feature_count
        Matches = $matches
        FinalCurrentModel = [string]$after.current_model.name
        ConnectionMs = $connectionWatch.ElapsedMilliseconds
        BasicReadMs = $basicWatch.ElapsedMilliseconds
        OpenModelMs = $openWatch.ElapsedMilliseconds
        FeatureReadMs = $featureWatch.ElapsedMilliseconds
        ReturnModelMs = $returnWatch.ElapsedMilliseconds
        TotalToolMs = $totalWatch.ElapsedMilliseconds
        HealthReply = $pong
        OpenReply = $openReply
        FeatureReply = $featureReply
        ReturnReply = $returnReply
    } | ConvertTo-Json -Depth 8 -Compress
}
finally {
    foreach ($path in $tempPaths) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}
