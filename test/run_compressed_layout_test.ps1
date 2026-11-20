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
$exe = Join-Path $artifact 'compressed_layout_test.exe'
$objDir = Join-Path $artifact 'codec-obj'
$vectors = Join-Path $artifact 'vectors'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null
New-Item -ItemType Directory -Path $vectors -Force | Out-Null
$qt = 'C:\Qt\6.3.1\msvc2019_64'
$vcpkg = 'C:\Users\ngoan\AppData\Local\VeraCore\vcpkg_installed\x64-windows'
if (-not (Test-Path -LiteralPath (Join-Path $vcpkg 'lib\avformat.lib'))) {
    throw "Embedded FFmpeg libraries are missing: $vcpkg"
}
$compile = @"
call "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /MD /DNDEBUG /DQT_CORE_LIB /DQT_NO_DEBUG /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /DWIN64 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVC_AUDIO_WITH_FFMPEG=1 /DVC_AUDIO_FFMPEG_READY=1 /permissive- /Zc:__cplusplus /utf-8 /I"$qt\include" /I"$qt\include\QtCore" /I"$qt\mkspecs\win32-msvc" /I"$vcpkg\include" /Fo"$objDir\\" /Fe"$exe" "$root\test\compressed_layout_test.cpp" "$root\VeraCore.QT\FfmpegAudioCodecBackend.cpp" /link /LIBPATH:"$qt\lib" /LIBPATH:"$vcpkg\lib" Qt6Core.lib avformat.lib avcodec.lib avutil.lib swresample.lib bcrypt.lib ws2_32.lib secur32.lib
"@
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$run = "set `"PATH=$qt\bin;$vcpkg\bin;%PATH%`" && `"$exe`" `"$vectors`""
& cmd.exe /d /s /c $run
exit $LASTEXITCODE
