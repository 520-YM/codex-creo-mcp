@echo off
setlocal EnableExtensions
set "ROOT=%~dp0.."
set "SRC=%ROOT%\native"
set "BUILD=%ROOT%\build\external_v3"
set "OUT=%BUILD%\creo_sheetmetal_flat_wall_persistent_bridge_v3.exe"
if not defined VSDEVCMD (
  echo ERROR: Set VSDEVCMD to Visual Studio VsDevCmd.bat.
  exit /b 2
)
if not defined CREO_COMMON_FILES (
  echo ERROR: Set CREO_COMMON_FILES to Creo Common Files.
  exit /b 2
)
set "PTC_ROOT=%CREO_COMMON_FILES%\protoolkit"
set "PTC_OBJ=%PTC_ROOT%\x86e_win64\obj"
set "PTC_INC=%PTC_ROOT%\includes"

if not exist "%BUILD%" mkdir "%BUILD%"
call "%VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%

cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" /I"%SRC%" "%SRC%\creo_sheetmetal_flat_wall_persistent_bridge.c" /Fo"%BUILD%\persistent.obj"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" /I"%SRC%" "%SRC%\codex_external_command_registry.c" /Fo"%BUILD%\registry.obj"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" /I"%SRC%" "%SRC%\codex_external_special_registry.c" /Fo"%BUILD%\special.obj"
if errorlevel 1 exit /b %errorlevel%

call :compile creo_active_sketch_axis_symmetry_bridge
call :compile creo_active_sketch_four_inset_dimensions_bridge
call :compile creo_assembly_add_fixed_bridge
call :compile creo_assembly_components_bridge
call :compile creo_assembly_create_bridge
call :compile creo_assembly_mate_align_bridge
call :compile creo_assembly_repeat_insert_pair_bridge
call :compile creo_assembly_skeleton_bridge
call :compile creo_bridge
call :compile creo_datum_plane_bridge
call :compile creo_datum_point_bridge
call :compile creo_dimension_modify_bridge
call :compile creo_dimension_write_bridge
call :compile creo_dimensions_bridge
call :compile creo_display_model_bridge
call :compile creo_empty_assembly_create_bridge
call :compile creo_export_bridge
call :compile creo_extrude_bridge
call :compile creo_feature_resume_bridge
call :compile creo_feature_suppress_bridge
call :compile creo_features_bridge
call :compile creo_general_sketch_bridge
call :compile creo_geometry_inspect_bridge
call :compile creo_hole_bridge
call :compile creo_mass_properties_bridge
call :compile creo_multi_write_bridge
call :compile creo_project_workdir_bridge
call :compile creo_sheetmetal_plate_bridge
call :compile creo_sheetmetal_reverse_wall_direction_bridge
call :compile creo_sheetmetal_skeleton_link_bridge
call :compile creo_sheetmetal_three_circle_cut_bridge
call :compile creo_skeleton_box_bridge
call :compile creo_skeleton_resize_box_bridge
call :compile creo_skeleton_reverse_direction_bridge
call :compile creo_template_copy_bridge
call :compile creo_verify_copy
call :compile creo_write_bridge

set "OBJECTS=%BUILD%\persistent.obj %BUILD%\registry.obj %BUILD%\special.obj"
for %%F in (%BUILD%\cmd_*.obj) do call set "OBJECTS=%%OBJECTS%% %%F"
link /out:"%OUT%" /subsystem:console /debug:none /machine:amd64 %OBJECTS% "%PTC_OBJ%\protoolkit_NU.lib" "%PTC_OBJ%\pt_asynchronous.lib" "%PTC_OBJ%\ucore.lib" "%PTC_OBJ%\udata.lib" libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib ws2_32.lib winmm.lib version.lib
exit /b %errorlevel%

:compile
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /DProEngineerConnect=codex_host_connect /DProEngineerDisconnect=codex_host_disconnect /Dwmain=codex_cmd_%1 /FI"%SRC%\codex_external_host.h" /I"%PTC_INC%" /I"%SRC%" "%SRC%\%1.c" /Fo"%BUILD%\cmd_%1.obj"
if errorlevel 1 exit /b %errorlevel%
exit /b 0
