param(
    [Parameter(Mandatory = $true)]
    [string]$ArtifactDir
)

$ErrorActionPreference = 'Stop'
$artifact = [IO.Path]::GetFullPath($ArtifactDir)
if (-not (Test-Path -LiteralPath $artifact -PathType Container)) {
    throw "Artifact directory does not exist: $artifact"
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$qt = 'C:\Qt\6.3.1\msvc2019_64'
$exe = Join-Path $artifact 'plugin_output_route_normalization_test.exe'
$obj = Join-Path $artifact 'route-normalization-obj'
New-Item -ItemType Directory -Path $obj -Force | Out-Null
$compile = @"
call "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /MD /DNDEBUG /DQT_NO_DEBUG /permissive- /Zc:__cplusplus /utf-8 /I"$qt\include" /I"$qt\include\QtCore" /I"$root\VeraCore.QT" /Fo"$obj\\" /Fe"$exe" "$root\test\plugin_output_route_normalization_test.cpp" "$root\VeraCore.QT\Track.cpp" "$root\VeraCore.Audio\SpatialAudioTypes.cpp" /link /LIBPATH:"$qt\lib" Qt6Core.lib
"@
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$previousPath = $env:PATH
try {
    $env:PATH = "$qt\bin;$previousPath"
    & $exe
    exit $LASTEXITCODE
} finally {
    $env:PATH = $previousPath
}
