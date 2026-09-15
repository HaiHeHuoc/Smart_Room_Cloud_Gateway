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

$arbiterSource = Get-Content `
    (Join-Path $componentRoot 'modules\arbitration\src\audio_manager_playback_arbiter.c') `
    -Raw
if (($arbiterSource -notmatch 'pcm_manager_cleanup_complete') -or
    ($arbiterSource -notmatch 'start_dispatching') -or
    ($arbiterSource -notmatch 'take_lock_for_dispatch_reconciliation')) {
    throw 'Arbiter lacks manager-cleanup and start-handoff ownership guards'
}
if (($arbiterSource -notmatch 'audio_manager_get_playback_status\(&playback\)') -or
    ($arbiterSource -notmatch 'retrying later can play stale speech')) {
    throw 'PCM admission does not revalidate paused WAV ownership or fail stale speech'
}
if ($arbiterSource -notmatch 'cancel_unstarted_wav_for_client') {
    throw 'PTT cannot atomically cancel an unstarted local WAV request'
}
$managerSource = Get-Content (Join-Path $componentRoot 'audio_manager.c') -Raw
if (($managerSource -notmatch 'audio_manager_consume_resume_requested') -or
    ($managerSource -notmatch 'handoff_pending')) {
    throw 'Rapid pause-resume intent is not consumed after retained playback suspension'
}
Write-Output 'audio arbiter lifecycle ownership boundary: PASS'
