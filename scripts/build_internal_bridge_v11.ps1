param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CreoCommonFiles = $env:CREO_COMMON_FILES,
    [string]$VsDevCmd = $env:VSDEVCMD
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not $CreoCommonFiles) {
    $CreoCommonFiles = 'E:\Program Files\PTC\Creo 10.0.0.0\Common Files'
}
if (-not $VsDevCmd) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installation = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($installation) {
            $VsDevCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
        }
    }
}
if (-not $VsDevCmd) {
    $VsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat'
}
$vsDevCmd = $VsDevCmd
$sourceRoot = Join-Path $repositoryRoot 'native'
$buildRoot = Join-Path $repositoryRoot 'build\internal_v11'
$ptcRoot = Join-Path $CreoCommonFiles 'protoolkit'
$ptcObjectRoot = Join-Path $ptcRoot 'x86e_win64\obj'
$ptcIncludeRoot = Join-Path $ptcRoot 'includes'
$dllOutput = Join-Path $buildRoot 'creo_safe_resident_internal_v11.dll'
$loaderOutput = Join-Path $buildRoot 'creo_internal_resident_loader_v11.exe'

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$environmentCommand = "call `"$vsDevCmd`" -arch=amd64 -host_arch=amd64 >nul && set"
$environmentLines = & cmd.exe /d /s /c $environmentCommand
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio build environment failed.' }
$developerPath = $environmentLines | Where-Object {
    $_.StartsWith('PATH=', [StringComparison]::Ordinal)
} | Select-Object -First 1
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $environmentName = $line.Substring(0, $separator)
        $environmentValue = $line.Substring($separator + 1)
        if ($environmentName -ieq 'PATH') { continue }
        else {
            Set-Item -Path "Env:$environmentName" -Value $environmentValue
        }
    }
}
if (-not $developerPath) { throw 'Visual Studio developer PATH was not returned.' }
$env:Path = $developerPath.Substring(5)

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
        [string]$AnsiEntry,
        [switch]$InternalCommand
    )
    $arguments = @($commonCompile)
    if ($InternalCommand) {
        $arguments += @(
            '/DProEngineerConnect=codex_internal_host_connect',
            '/DProEngineerDisconnect=codex_internal_host_disconnect',
            '/DProEngineerConnectionStart=codex_internal_host_connection_start',
            "/FI$sourceRoot\codex_internal_host.h"
        )
    }
    if ($WideEntry) { $arguments += "/Dwmain=$WideEntry" }
    if ($AnsiEntry) { $arguments += "/Dmain=$AnsiEntry" }
    $arguments += @((Join-Path $sourceRoot $Source), "/Fo$Object")
    & cl.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $Source" }
}

$objects = [Collections.Generic.List[string]]::new()
foreach ($item in @(
    @{ Source='creo_safe_resident_dll.c'; Object='resident.obj' },
    @{ Source='codex_external_command_registry.c'; Object='registry.obj' }
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
    'creo_gas_spring_variant_bridge',
    'creo_geometry_inspect_bridge',
    'creo_hole_bridge',
    'creo_mass_properties_bridge',
    'creo_multi_write_bridge',
    'creo_model_rename_bridge',
    'creo_project_workdir_bridge',
    'creo_sheetmetal_plate_bridge',
    'creo_sheetmetal_reverse_wall_direction_bridge',
    'creo_sheetmetal_skeleton_link_bridge',
    'creo_sheetmetal_three_circle_cut_bridge',
    'creo_skeleton_box_bridge',
    'creo_skeleton_resize_box_bridge',
    'creo_skeleton_reverse_direction_bridge',
    'creo_template_copy_bridge',
    'creo_top_assembly_finalize_bridge',
    'creo_verify_copy',
    'creo_write_bridge'
)
foreach ($name in $wideCommands) {
    $objectPath = Join-Path $buildRoot ("cmd_$name.obj")
    Compile-Source -Source "$name.c" -Object $objectPath `
        -WideEntry "codex_cmd_$name" -InternalCommand
    $objects.Add($objectPath)
}
foreach ($name in @('creo_bridge', 'creo_export_bridge')) {
    $objectPath = Join-Path $buildRoot ("cmd_$name.obj")
    Compile-Source -Source "$name.c" -Object $objectPath `
        -AnsiEntry "codex_cmd_${name}_ansi" -InternalCommand
    $objects.Add($objectPath)
}

$commonLibraries = @(
    (Join-Path $ptcObjectRoot 'ucore.lib'),
    (Join-Path $ptcObjectRoot 'udata.lib'),
    'libcmt.lib', 'kernel32.lib', 'user32.lib', 'wsock32.lib',
    'advapi32.lib', 'mpr.lib', 'winspool.lib', 'netapi32.lib',
    'psapi.lib', 'gdi32.lib', 'shell32.lib', 'comdlg32.lib',
    'ole32.lib', 'ws2_32.lib', 'winmm.lib', 'version.lib'
)
$dllLinkArguments = @(
    "/out:$dllOutput", "/map:$buildRoot\creo_safe_resident_internal_v11.map",
    '/dll', '/subsystem:console', '/debug:none', '/machine:amd64'
) + @($objects) + @((Join-Path $ptcObjectRoot 'protk_dll_NU.lib')) + $commonLibraries
& link.exe @dllLinkArguments
if ($LASTEXITCODE -ne 0) { throw 'Internal DLL link failed.' }

$loaderObject = Join-Path $buildRoot 'loader.obj'
Compile-Source -Source 'creo_internal_resident_loader.c' -Object $loaderObject
$loaderLinkArguments = @(
    "/out:$loaderOutput", '/subsystem:console', '/debug:none', '/machine:amd64',
    $loaderObject,
    (Join-Path $ptcObjectRoot 'protoolkit_NU.lib'),
    (Join-Path $ptcObjectRoot 'pt_asynchronous.lib')
) + $commonLibraries
& link.exe @loaderLinkArguments
if ($LASTEXITCODE -ne 0) { throw 'Internal loader link failed.' }

Get-Item -LiteralPath $dllOutput, $loaderOutput |
    Select-Object FullName, Length, LastWriteTime
