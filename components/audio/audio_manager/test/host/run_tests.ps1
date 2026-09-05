$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_audio_wav_tests'
$parserExecutable = Join-Path $outputRoot 'audio_wav_parser_tests.exe'
$streamExecutable = Join-Path $outputRoot 'audio_wav_stream_tests.exe'
$pcmStreamExecutable = Join-Path $outputRoot 'audio_pcm_stream_core_tests.exe'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc `
    -std=c11 `
    -Wall `
    -Wextra `
    -Werror `
    -I (Join-Path $testRoot 'include') `
    -I $componentRoot `
    (Join-Path $componentRoot 'audio_wav.c') `
    (Join-Path $testRoot 'test_audio_wav_parser.c') `
    -o $parserExecutable

if ($LASTEXITCODE -ne 0) {
    throw "Host WAV parser test build failed with exit code $LASTEXITCODE"
}

& $parserExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host WAV parser tests failed with exit code $LASTEXITCODE"
}

& $gcc `
    -std=c11 `
    -Wall `
    -Wextra `
    -Werror `
    -I (Join-Path $testRoot 'include') `
    -I $componentRoot `
    (Join-Path $componentRoot 'audio_wav.c') `
    (Join-Path $testRoot 'test_audio_wav_stream.c') `
    -o $streamExecutable

if ($LASTEXITCODE -ne 0) {
    throw "Host WAV stream-contract test build failed with exit code $LASTEXITCODE"
}

& $streamExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host WAV stream-contract tests failed with exit code $LASTEXITCODE"
}

& $gcc `
    -std=c11 `
    -Wall `
    -Wextra `
    -Werror `
    -I (Join-Path $testRoot 'include') `
    -I $componentRoot `
    (Join-Path $componentRoot 'audio_manager_pcm_stream_core.c') `
    (Join-Path $testRoot 'test_audio_pcm_stream_core.c') `
    -o $pcmStreamExecutable

if ($LASTEXITCODE -ne 0) {
    throw "Host PCM stream-core test build failed with exit code $LASTEXITCODE"
}

& $pcmStreamExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host PCM stream-core tests failed with exit code $LASTEXITCODE"
}
