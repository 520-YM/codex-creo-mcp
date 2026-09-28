@echo off
setlocal EnableExtensions
set "ROOT=%~dp0.."
set "SRC=%ROOT%\native"
set "BUILD=%ROOT%\build\internal_v11"
set "DLL_OUT=%BUILD%\creo_safe_resident_internal_v11.dll"
set "LOADER_OUT=%BUILD%\creo_internal_resident_loader_v11.exe"
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

cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" /I"%SRC%" "%SRC%\creo_safe_resident_dll.c" /Fo"%BUILD%\resident.obj"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" /I"%SRC%" "%SRC%\codex_external_command_registry.c" /Fo"%BUILD%\registry.obj"
if errorlevel 1 exit /b %errorlevel%

call :compile_wide creo_active_sketch_axis_symmetry_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_active_sketch_four_inset_dimensions_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_add_fixed_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_components_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_create_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_mate_align_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_repeat_insert_pair_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_assembly_skeleton_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_ansi creo_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_datum_plane_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_datum_point_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_dimension_modify_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_dimension_write_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_dimensions_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_display_model_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_empty_assembly_create_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_ansi creo_export_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_extrude_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_feature_resume_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_feature_suppress_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_features_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_general_sketch_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_geometry_inspect_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_hole_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_mass_properties_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_multi_write_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_project_workdir_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_sheetmetal_plate_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_sheetmetal_reverse_wall_direction_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_sheetmetal_skeleton_link_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_sheetmetal_three_circle_cut_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_skeleton_box_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_skeleton_resize_box_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_skeleton_reverse_direction_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_template_copy_bridge
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_verify_copy
if errorlevel 1 exit /b %errorlevel%
call :compile_wide creo_write_bridge
if errorlevel 1 exit /b %errorlevel%

set "OBJECTS=%BUILD%\resident.obj %BUILD%\registry.obj"
for %%F in (%BUILD%\cmd_*.obj) do call set "OBJECTS=%%OBJECTS%% %%F"
link /nologo /out:"%DLL_OUT%" /dll /subsystem:console /debug:none /machine:amd64 %OBJECTS% "%PTC_OBJ%\protk_dll_NU.lib" "%PTC_OBJ%\ucore.lib" "%PTC_OBJ%\udata.lib" libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib ws2_32.lib winmm.lib version.lib
if errorlevel 1 exit /b %errorlevel%

cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" "%SRC%\creo_internal_resident_loader.c" /Fo"%BUILD%\loader.obj"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:"%LOADER_OUT%" /subsystem:console /debug:none /machine:amd64 "%BUILD%\loader.obj" "%PTC_OBJ%\protoolkit_NU.lib" "%PTC_OBJ%\pt_asynchronous.lib" "%PTC_OBJ%\ucore.lib" "%PTC_OBJ%\udata.lib" libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib ws2_32.lib winmm.lib version.lib
exit /b %errorlevel%

:compile_wide
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /DProEngineerConnect=codex_internal_host_connect /DProEngineerDisconnect=codex_internal_host_disconnect /Dwmain=codex_cmd_%1 /FI"%SRC%\codex_internal_host.h" /I"%PTC_INC%" /I"%SRC%" "%SRC%\%1.c" /Fo"%BUILD%\cmd_%1.obj"
exit /b %errorlevel%

:compile_ansi
cl /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /DProEngineerConnect=codex_internal_host_connect /DProEngineerDisconnect=codex_internal_host_disconnect /Dmain=codex_cmd_%1_ansi /FI"%SRC%\codex_internal_host.h" /I"%PTC_INC%" /I"%SRC%" "%SRC%\%1.c" /Fo"%BUILD%\cmd_%1.obj"
exit /b %errorlevel%
