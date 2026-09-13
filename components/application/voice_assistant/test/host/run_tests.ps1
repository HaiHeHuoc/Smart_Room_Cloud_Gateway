$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_voice_playback_turn_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $componentRoot 'modules\playback\include') `
    (Join-Path $componentRoot 'modules\playback\src\voice_assistant_playback_turn_policy.c') `
    (Join-Path $testRoot 'test_voice_playback_turn_policy.c') `
    -o (Join-Path $outputRoot 'voice_playback_turn_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Voice playback turn policy test build failed' }
& (Join-Path $outputRoot 'voice_playback_turn_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Voice playback turn policy tests failed' }

$downlinkSource = Get-Content `
    (Join-Path $componentRoot 'modules\downlink\src\voice_assistant_downlink.c') `
    -Raw
if ($downlinkSource -notmatch 'item\.local_cancel') {
    throw 'Downlink-owned local cancellation command is missing'
}
if ($downlinkSource -notmatch 'voice_assistant_audio_stream_cancel\(\)') {
    throw 'Intentional TTS interruption is not routed through cooperative cancel'
}
if ($downlinkSource -notmatch 's_response_tainted_generation') {
    throw 'TTS interruption lacks stale-PCM generation tainting'
}
Write-Output 'voice downlink TTS interruption boundary: PASS'
