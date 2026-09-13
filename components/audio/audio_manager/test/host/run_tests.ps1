$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_audio_playback_control_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'include') `
    -I (Join-Path $componentRoot 'modules\playback\include') `
    (Join-Path $componentRoot 'modules\playback\src\audio_manager_playback_control_policy.c') `
    (Join-Path $testRoot 'test_audio_playback_control_policy.c') `
    -o (Join-Path $outputRoot 'audio_playback_control_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Playback-control policy test build failed' }
& (Join-Path $outputRoot 'audio_playback_control_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Playback-control policy tests failed' }

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'modules\wav\include') `
    (Join-Path $componentRoot 'modules\wav\src\audio_wav.c') `
    (Join-Path $testRoot 'test_audio_wav_resume_position.c') `
    -o (Join-Path $outputRoot 'audio_wav_resume_position_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'WAV resume-position test build failed' }
& (Join-Path $outputRoot 'audio_wav_resume_position_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'WAV resume-position tests failed' }
