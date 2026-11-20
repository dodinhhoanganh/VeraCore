param(
    [string]$Root = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'
$rootPath = [IO.Path]::GetFullPath($Root)
$header = Get-Content -Raw -LiteralPath (
    Join-Path $rootPath 'VeraCore.QT\StemSeparationService.h')
$source = Get-Content -Raw -LiteralPath (
    Join-Path $rootPath 'VeraCore.QT\StemSeparationService.cpp')

$checks = [ordered]@{
    CompletionPassesValidatedPaths = $source.Contains(
        'tr("Validated %1 stem files.").arg(paths.size()), paths)')
    ManifestStoresOutputArray = $source.Contains(
        'manifest.insert(QStringLiteral("outputs"), outputs)')
    IdentityStoresRelativePath = $source.Contains(
        'identity.insert(QStringLiteral("relativePath"), relative)')
    IdentityStoresFileSize = $source.Contains(
        'identity.insert(QStringLiteral("sizeBytes"), QString::number(file.size()))')
    IdentityStoresAudioShape = $source.Contains(
        'identity.insert(QStringLiteral("sampleRate"), probe.sampleRate)') -and
        $source.Contains(
        'identity.insert(QStringLiteral("channelCount"), probe.channelCount)') -and
        $source.Contains(
        'identity.insert(QStringLiteral("frameCount"), QString::number(probe.frameCount))')
    RecoveryChecksRecordedIdentity = $source.Contains(
        'recordedStemOutputMatches(recordedOutputs.value(stem.toLower())')
    LegacyManifestFallback = $source.Contains(
        'const bool hasRecordedOutputs = object.contains(QStringLiteral("outputs"))') -and
        $source.Contains('(hasRecordedOutputs &&')
    SignatureCarriesValidatedPaths = $header.Contains(
        'const QStringList& validatedOutputPaths = {}) const;')
}

$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value })
if ($failed.Count -ne 0) {
    Write-Output ('STEM_OUTPUT_IDENTITY_TEST=FAIL CHECKS=' +
        (($failed | ForEach-Object Key) -join ','))
    exit 1
}

Write-Output (('STEM_OUTPUT_IDENTITY_TEST=PASS VECTORS={0} ' +
    'FIELDS=PATH,SIZE,MTIME,SAMPLE_RATE,CHANNELS,FRAMES LEGACY_V1=SUPPORTED') -f
    $checks.Count)
exit 0
