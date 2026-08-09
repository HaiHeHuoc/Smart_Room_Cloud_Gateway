$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_audio_wav_tests'
$testExecutable = Join-Path $outputRoot 'audio_wav_parser_tests.exe'
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
    -o $testExecutable

if ($LASTEXITCODE -ne 0) {
    throw "Host WAV parser test build failed with exit code $LASTEXITCODE"
}

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host WAV parser tests failed with exit code $LASTEXITCODE"
}
