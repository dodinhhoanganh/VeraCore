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
$exe = Join-Path $artifact 'spatial_output_plan_test.exe'
$objDir = Join-Path $artifact 'routing-obj'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null
$compile = @"
call "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /MD /DNDEBUG /permissive- /Zc:__cplusplus /utf-8 /Fo"$objDir\\" /Fe"$exe" "$root\test\spatial_output_plan_test.cpp" "$root\VeraCore.Audio\SpatialAudioTypes.cpp"
"@
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
exit $LASTEXITCODE
