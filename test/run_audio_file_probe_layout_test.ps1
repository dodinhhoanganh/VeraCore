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
$exe = Join-Path $artifact 'audio_file_probe_layout_test.exe'
$objDir = Join-Path $artifact 'probe-obj'
$vectors = Join-Path $artifact 'vectors'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null
New-Item -ItemType Directory -Path $vectors -Force | Out-Null
$compile = @"
call "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /MD /DNDEBUG /DQT_CORE_LIB /DQT_NO_DEBUG /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /DWIN64 /D_ENABLE_EXTENDED_ALIGNED_STORAGE /permissive- /Zc:__cplusplus /utf-8 /I"C:\Qt\6.3.1\msvc2019_64\include" /I"C:\Qt\6.3.1\msvc2019_64\include\QtCore" /I"C:\Qt\6.3.1\msvc2019_64\mkspecs\win32-msvc" /Fo"$objDir\\" /Fe"$exe" "$root\test\audio_file_probe_layout_test.cpp" "$root\VeraCore.QT\AudioFileDecoder.cpp" /link /LIBPATH:"C:\Qt\6.3.1\msvc2019_64\lib" Qt6Core.lib
"@
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$run = "set `"PATH=C:\Qt\6.3.1\msvc2019_64\bin;%PATH%`" && `"$exe`" `"$vectors`""
& cmd.exe /d /s /c $run
exit $LASTEXITCODE
