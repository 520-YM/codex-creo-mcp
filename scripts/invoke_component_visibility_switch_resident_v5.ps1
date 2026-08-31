param(
    [Parameter(Mandatory = $true)][string]$Assembly,
    [Parameter(Mandatory = $true)][string]$SuppressComponent,
    [Parameter(Mandatory = $true)][string]$ResumeComponent
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
    param([string]$Directory, [string]$Stem, [string]$Extension)
    $pattern = '^' + [regex]::Escape($Stem) + '\.' +
        [regex]::Escape($Extension) + '(?:\.(\d+))?$'
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

    $guardWatch = [Diagnostics.Stopwatch]::StartNew()
    $before = Read-Basic
    $workingDirectory = [string]$before.working_directory
    $topAssembly = [string]$before.current_model.name
    if ([string]$before.current_model.model_type -ine 'assembly' -or
        $topAssembly -notmatch '(?i)00000(?:-|$)') {
        throw 'The active model is not the five-zero top assembly.'
    }
    if (-not (Find-LatestCreoFile $workingDirectory $Assembly 'asm')) {
        throw "Assembly $Assembly was not found in the current working directory."
    }
    foreach ($component in @($SuppressComponent, $ResumeComponent)) {
        if (-not (Find-LatestCreoFile $workingDirectory $component 'prt')) {
            throw "Component $component was not found in the current working directory."
        }
    }
    $guardWatch.Stop()

    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_compswitch_' + [guid]::NewGuid().ToString('N') + '.json')
    $tempPaths.Add($resultPath)
    $command = @(
        'COMPSWITCH', $resultPath, $Assembly, $topAssembly,
        $SuppressComponent, $ResumeComponent
    ) -join '|'

    $modifyWatch = [Diagnostics.Stopwatch]::StartNew()
    $reply = Send-CreoPipe $command
    $modifyWatch.Stop()
    $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $result.ok) {
        throw "Creo component switch failed at stage $($result.stage), error $($result.error_code)."
    }

    $verifyWatch = [Diagnostics.Stopwatch]::StartNew()
    $after = Read-Basic
    if ([string]$after.current_model.name -ine $topAssembly -or
        [string]$after.current_model.model_type -ine 'assembly') {
        throw 'Top assembly was not restored after regeneration.'
    }
    if ([string]$result.suppressed_component -ine $SuppressComponent -or
        [string]$result.resumed_component -ine $ResumeComponent) {
        throw 'Component status readback does not match the request.'
    }
    $verifyWatch.Stop()

    $cleanupWatch = [Diagnostics.Stopwatch]::StartNew()
    $latestAssembly = Find-LatestCreoFile $workingDirectory $Assembly 'asm'
    $latestTop = Find-LatestCreoFile $workingDirectory $topAssembly 'asm'
    $keepJson = @($latestAssembly.File.Name, $latestTop.File.Name) | ConvertTo-Json -Compress
    $keepBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($keepJson))
    $cleanupScript = Join-Path (Split-Path -Parent $PSScriptRoot) `
        'cleanup_project_model_versions.ps1'
    $cleanupRoot = [IO.Path]::GetDirectoryName(
        [IO.Path]::GetFullPath($workingDirectory).TrimEnd('\'))
    $cleanup = & $cleanupScript -ProjectRoot $cleanupRoot `
        -ProjectDirectory $workingDirectory -KeepFilesBase64 $keepBase64 |
        ConvertFrom-Json
    $cleanupWatch.Stop()

    $totalWatch.Stop()
    [pscustomobject]@{
        Ok = $true
        WorkingDirectory = $workingDirectory
        Assembly = $Assembly
        TopAssembly = $topAssembly
        SuppressedComponent = [string]$result.suppressed_component
        SuppressedFeatureId = [int]$result.suppressed_feature_id
        ResumedComponent = [string]$result.resumed_component
        ResumedFeatureId = [int]$result.resumed_feature_id
        AssemblyRegenerateStatus = $result.assembly_regenerate_status
        TopRegenerateStatus = $result.top_regenerate_status
        FinalCurrentModel = [string]$after.current_model.name
        Saved = [bool]$result.saved
        KeptFiles = $cleanup.kept_files
        RecycledOldVersions = $cleanup.moved_to_recycle_bin
        ConnectionMs = $connectionWatch.ElapsedMilliseconds
        GuardMs = $guardWatch.ElapsedMilliseconds
        ModifyRegenerateSaveMs = $modifyWatch.ElapsedMilliseconds
        VerifyMs = $verifyWatch.ElapsedMilliseconds
        CleanupMs = $cleanupWatch.ElapsedMilliseconds
        TotalToolMs = $totalWatch.ElapsedMilliseconds
        PipeReply = $reply
        HealthReply = $pong
    } | ConvertTo-Json -Depth 6 -Compress
}
finally {
    foreach ($path in $tempPaths) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}
