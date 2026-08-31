param(
    [Parameter(Mandatory = $true)][int]$NewCount,
    [Parameter(Mandatory = $true)][double]$NewSpacing
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
    if ($NewCount -lt 2 -or $NewCount -gt 1000) {
        throw 'NewCount must be between 2 and 1000.'
    }
    if ($NewSpacing -le 0 -or $NewSpacing -gt 1000000) {
        throw 'NewSpacing must be greater than zero and at most 1000000.'
    }
    $connectionWatch = [Diagnostics.Stopwatch]::StartNew()
    $pong = Send-CreoPipe 'PING' 500
    $connectionWatch.Stop()

    $guardWatch = [Diagnostics.Stopwatch]::StartNew()
    $before = Read-Basic
    $workingDirectory = [string]$before.working_directory
    $currentName = [string]$before.current_model.name
    $currentType = [string]$before.current_model.model_type
    if ($currentType -ieq 'part' -and $currentName -match '(?i)^(.*00000.*)_SKEL$') {
        $topAssembly = $Matches[1]
        $skeletonName = $currentName
    }
    elseif ($currentType -ieq 'assembly' -and $currentName -match '(?i)00000(?:-|$)') {
        $topAssembly = $currentName
        $skeletonName = $topAssembly + '_SKEL'
    }
    else {
        throw 'The active model is neither the five-zero top assembly nor its skeleton.'
    }
    if (-not (Find-LatestCreoFile $workingDirectory $skeletonName 'prt')) {
        throw "Skeleton $skeletonName was not found."
    }
    $topFile = Find-LatestCreoFile $workingDirectory $topAssembly 'asm'
    if (-not $topFile) {
        throw "Top assembly $topAssembly was not found."
    }
    $guardWatch.Stop()

    $loadTopWatch = [Diagnostics.Stopwatch]::StartNew()
    if ($currentType -ieq 'part') {
        $displayResultPath = Join-Path ([IO.Path]::GetTempPath()) (
            'creo_display_top_' + [guid]::NewGuid().ToString('N') + '.json')
        $tempPaths.Add($displayResultPath)
        $displayReply = Send-CreoPipe (
            'DISPLAY|' + $displayResultPath + '|' + $topFile.File.FullName + '|' + $topAssembly)
        $displayResult = Get-Content -LiteralPath $displayResultPath -Raw -Encoding UTF8 |
            ConvertFrom-Json
        if (-not $displayResult.ok) {
            throw "Top assembly load failed at stage $($displayResult.stage)."
        }
    }
    $loadTopWatch.Stop()

    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'creo_patternset_' + [guid]::NewGuid().ToString('N') + '.json')
    $tempPaths.Add($resultPath)
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    $command = @(
        'PATTERNSET', $resultPath, $skeletonName, $topAssembly,
        '8242', '铰链阵列', '8241', 'LOCAL_GROUP', 'd229',
        [string]$NewCount, $NewSpacing.ToString('R', $invariant)
    ) -join '|'

    $modifyWatch = [Diagnostics.Stopwatch]::StartNew()
    $reply = Send-CreoPipe $command
    $modifyWatch.Stop()
    $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $result.ok) {
        throw "Creo hinge pattern update failed at stage $($result.stage), error $($result.error_code)."
    }

    $verifyWatch = [Diagnostics.Stopwatch]::StartNew()
    $after = Read-Basic
    if ([string]$after.current_model.name -ine $topAssembly -or
        [string]$after.current_model.model_type -ine 'assembly') {
        throw 'Top assembly was not restored after regeneration.'
    }
    if ([int]$result.new_count -ne $NewCount -or
        [math]::Abs([double]$result.verified_spacing - $NewSpacing) -gt 0.000001) {
        throw 'Pattern readback does not match the requested count and spacing.'
    }
    $verifyWatch.Stop()

    $cleanupWatch = [Diagnostics.Stopwatch]::StartNew()
    $latestSkeleton = Find-LatestCreoFile $workingDirectory $skeletonName 'prt'
    $latestTop = Find-LatestCreoFile $workingDirectory $topAssembly 'asm'
    $keepJson = @($latestSkeleton.File.Name, $latestTop.File.Name) | ConvertTo-Json -Compress
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
        Skeleton = $skeletonName
        TopAssembly = $topAssembly
        PatternFeatureId = [int]$result.pattern_feature_id
        LeaderFeatureId = [int]$result.leader_feature_id
        OldCount = [int]$result.old_count
        NewCount = [int]$result.new_count
        OldSpacing = [double]$result.old_spacing
        NewSpacing = [double]$result.verified_spacing
        ModelRegenerateStatus = $result.model_regenerate_status
        TopRegenerateStatus = $result.top_regenerate_status
        FinalCurrentModel = [string]$after.current_model.name
        Saved = [bool]$result.saved
        KeptFiles = $cleanup.kept_files
        RecycledOldVersions = $cleanup.moved_to_recycle_bin
        ConnectionMs = $connectionWatch.ElapsedMilliseconds
        GuardMs = $guardWatch.ElapsedMilliseconds
        LoadTopMs = $loadTopWatch.ElapsedMilliseconds
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
