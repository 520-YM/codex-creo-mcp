$ErrorActionPreference = 'Stop'

$vsDevCmd = $env:VSDEVCMD
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repositoryRoot 'native'
$buildRoot = Join-Path $repositoryRoot 'build\external_v3'
$ptcRoot = if ($env:CREO_COMMON_FILES) {
    Join-Path $env:CREO_COMMON_FILES 'protoolkit'
} else { '' }
$ptcObjectRoot = Join-Path $ptcRoot 'x86e_win64\obj'
$ptcIncludeRoot = Join-Path $ptcRoot 'includes'
$output = Join-Path $buildRoot 'creo_sheetmetal_flat_wall_persistent_bridge_v3.exe'

if (-not $vsDevCmd -or -not (Test-Path -LiteralPath $vsDevCmd)) {
    throw 'Set VSDEVCMD to the target computer Visual Studio VsDevCmd.bat.'
}
if (-not $env:CREO_COMMON_FILES -or -not (Test-Path -LiteralPath $ptcRoot)) {
    throw 'Set CREO_COMMON_FILES to the target computer Creo Common Files directory.'
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$environmentCommand = "call `"$vsDevCmd`" -arch=amd64 -host_arch=amd64 >nul && set"
$environmentLines = & cmd.exe /d /s /c $environmentCommand
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio build environment failed.' }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable(
            $line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}

$commonCompile = @(
    '/nologo', '/c', '/O2', '/GS', '/fp:precise',
    '/D_WSTDIO_DEFINED', '/DPRO_MACHINE=36', '/DPRO_OS=4',
    "/I$ptcIncludeRoot", "/I$sourceRoot"
)
function Compile-Source {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Object,
        [string]$WideEntry,
        [string]$AnsiEntry
    )
    $arguments = @($commonCompile)
    if ($WideEntry) {
        $arguments += @(
            '/DProEngineerConnect=codex_host_connect',
            '/DProEngineerDisconnect=codex_host_disconnect',
            "/Dwmain=$WideEntry", "/FI$sourceRoot\codex_external_host.h"
        )
    }
    if ($AnsiEntry) {
        $arguments += @(
            '/DProEngineerConnect=codex_host_connect',
            '/DProEngineerDisconnect=codex_host_disconnect',
            "/Dmain=$AnsiEntry", "/FI$sourceRoot\codex_external_host.h"
        )
    }
    $arguments += @((Join-Path $sourceRoot $Source), "/Fo$Object")
    & cl.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $Source" }
}

$objects = [Collections.Generic.List[string]]::new()
foreach ($item in @(
    @{ Source='creo_sheetmetal_flat_wall_persistent_bridge.c'; Object='persistent.obj' },
    @{ Source='codex_external_command_registry.c'; Object='registry.obj' },
    @{ Source='codex_external_special_registry.c'; Object='special.obj' }
)) {
    $objectPath = Join-Path $buildRoot $item.Object
    Compile-Source -Source $item.Source -Object $objectPath
    $objects.Add($objectPath)
}

$wideCommands = @(
    'creo_active_sketch_axis_symmetry_bridge',
    'creo_active_sketch_four_inset_dimensions_bridge',
    'creo_assembly_add_fixed_bridge',
    'creo_assembly_components_bridge',
    'creo_assembly_create_bridge',
    'creo_assembly_mate_align_bridge',
    'creo_assembly_repeat_insert_pair_bridge',
    'creo_assembly_skeleton_bridge',
    'creo_datum_plane_bridge',
    'creo_datum_point_bridge',
    'creo_dimension_modify_bridge',
    'creo_dimension_write_bridge',
    'creo_dimensions_bridge',
    'creo_display_model_bridge',
    'creo_empty_assembly_create_bridge',
    'creo_extrude_bridge',
    'creo_feature_resume_bridge',
    'creo_feature_suppress_bridge',
    'creo_features_bridge',
    'creo_general_sketch_bridge',
    'creo_geometry_inspect_bridge',
    'creo_hole_bridge',
    'creo_mass_properties_bridge',
    'creo_multi_write_bridge',
    'creo_project_workdir_bridge',
    'creo_sheetmetal_plate_bridge',
    'creo_sheetmetal_reverse_wall_direction_bridge',
    'creo_sheetmetal_skeleton_link_bridge',
    'creo_sheetmetal_three_circle_cut_bridge',
    'creo_skeleton_box_bridge',
    'creo_skeleton_resize_box_bridge',
    'creo_skeleton_reverse_direction_bridge',
    'creo_template_copy_bridge',
    'creo_verify_copy',
    'creo_write_bridge'
)
foreach ($name in $wideCommands) {
    $objectPath = Join-Path $buildRoot ("cmd_$name.obj")
    Compile-Source -Source "$name.c" -Object $objectPath -WideEntry "codex_cmd_$name"
    $objects.Add($objectPath)
}
foreach ($name in @('creo_bridge', 'creo_export_bridge')) {
    $objectPath = Join-Path $buildRoot ("cmd_$name.obj")
    Compile-Source -Source "$name.c" -Object $objectPath -AnsiEntry "codex_cmd_${name}_ansi"
    $objects.Add($objectPath)
}

$libraries = @(
    (Join-Path $ptcObjectRoot 'protoolkit_NU.lib'),
    (Join-Path $ptcObjectRoot 'pt_asynchronous.lib'),
    (Join-Path $ptcObjectRoot 'ucore.lib'),
    (Join-Path $ptcObjectRoot 'udata.lib'),
    'libcmt.lib', 'kernel32.lib', 'user32.lib', 'wsock32.lib',
    'advapi32.lib', 'mpr.lib', 'winspool.lib', 'netapi32.lib',
    'psapi.lib', 'gdi32.lib', 'shell32.lib', 'comdlg32.lib',
    'ole32.lib', 'ws2_32.lib', 'winmm.lib', 'version.lib'
)
$linkArguments = @(
    "/out:$output", '/subsystem:console', '/debug:none', '/machine:amd64'
) + @($objects) + $libraries
& link.exe @linkArguments
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
Get-Item -LiteralPath $output | Select-Object FullName, Length, LastWriteTime
