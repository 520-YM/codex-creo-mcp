$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repositoryRoot 'native\creo_mass_properties_bridge.c'
$buildRoot = Join-Path $repositoryRoot 'build\oneoff_mass'
$object = Join-Path $buildRoot 'creo_mass_properties_bridge.obj'
$output = Join-Path $buildRoot 'creo_mass_properties_bridge.exe'
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat'
$ptcRoot = 'E:\Program Files\PTC\Creo 10.0.0.0\Common Files\protoolkit'
$ptcObjects = Join-Path $ptcRoot 'x86e_win64\obj'
$ptcIncludes = Join-Path $ptcRoot 'includes'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

$environmentLines = & cmd.exe /d /s /c (
    'call "' + $vsDevCmd + '" -arch=amd64 -host_arch=amd64 >nul && set')
$developerPath = $environmentLines | Where-Object {
    $_.StartsWith('PATH=', [StringComparison]::Ordinal)
} | Select-Object -First 1
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    if ($name -ieq 'PATH') { continue }
    else { Set-Item -Path "Env:$name" -Value $value }
}
if (-not $developerPath) { throw 'Visual Studio developer PATH was not returned.' }
$env:Path = $developerPath.Substring(5)

& cl.exe /nologo /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED `
    /DPRO_MACHINE=36 /DPRO_OS=4 "/I$ptcIncludes" $source "/Fo$object"
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }

$libraries = @(
    (Join-Path $ptcObjects 'protoolkit_NU.lib'),
    (Join-Path $ptcObjects 'pt_asynchronous.lib'),
    (Join-Path $ptcObjects 'ucore.lib'),
    (Join-Path $ptcObjects 'udata.lib'),
    'libcmt.lib', 'kernel32.lib', 'user32.lib', 'wsock32.lib',
    'advapi32.lib', 'mpr.lib', 'winspool.lib', 'netapi32.lib',
    'psapi.lib', 'gdi32.lib', 'shell32.lib', 'comdlg32.lib',
    'ole32.lib', 'ws2_32.lib', 'winmm.lib', 'version.lib'
)
& link.exe "/out:$output" /subsystem:console /debug:none /machine:amd64 `
    $object @libraries
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
Get-Item -LiteralPath $output | Select-Object FullName, Length, LastWriteTime
