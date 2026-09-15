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
if ($downlinkSource -notmatch 's_response_tainted_epoch') {
    throw 'TTS interruption lacks response-epoch tainting'
}
if (($downlinkSource -notmatch 'response_epoch') -or
    ($downlinkSource -notmatch 'downlink_capture_callback_epoch')) {
    throw 'Downlink callbacks are not tagged with a private response epoch'
}
if (($downlinkSource -notmatch 's_interrupt_requested_epoch') -or
    ($downlinkSource -notmatch 'downlink_process_pending_interrupt')) {
    throw 'PTT interruption lacks a queue-independent terminal intent'
}
if ($downlinkSource -notmatch 'audio_stream_begin\(\);[\s\S]{0,800}downlink_response_is_tainted') {
    throw 'Downlink does not recheck a callback taint after PCM stream admission'
}
if ($downlinkSource -notmatch 's_response_tainted_epoch,[\s\S]{0,500}s_callback_response_epoch = response_epoch') {
    throw 'Downlink enables callback admission before clearing the response taint'
}
if ($downlinkSource -notmatch 'voice_assistant_audio_stream_release\(\);[\s\S]{0,1200}voice_assistant_playback_finish_turn\(ptt_generation\);[\s\S]{0,1200}downlink_finish_turn_state\(generation, response_epoch\)') {
    throw 'Downlink publishes non-busy before playback-turn finalization'
}
if ($downlinkSource -match 'xQueueReset\(s_queue\)') {
    throw 'Downlink abort still relies on a queue reset instead of epoch gating'
}
$downlinkHeader = Get-Content `
    (Join-Path $componentRoot 'include\voice_assistant_downlink.h') `
    -Raw
if (($downlinkSource -notmatch 's_transport_fence_generation') -or
    ($downlinkSource -notmatch 'xiaozhi_foundation_audio_abort_response') -or
    ($downlinkHeader -notmatch 'voice_assistant_downlink_transport_fence_required')) {
    throw 'Downlink local abort does not require a fresh response transport'
}
$voiceSource = Get-Content (Join-Path $componentRoot 'voice_assistant.c') -Raw
$pttSource = Get-Content `
    (Join-Path $componentRoot 'modules\ptt\src\voice_assistant_ptt.c') `
    -Raw
$uplinkSource = Get-Content `
    (Join-Path $componentRoot 'modules\uplink\src\voice_assistant_uplink.c') `
    -Raw
if (($voiceSource -notmatch 'VOICE_ASSISTANT_COMMAND_ROTATE_SESSION') -or
    ($voiceSource -notmatch 'xiaozhi_foundation_session_rotate_transport') -or
    ($pttSource -notmatch 'ptt_rotate_response_transport') -or
    ($pttSource -notmatch 'VOICE_ASSISTANT_PTT_ARMING_SESSION')) {
    throw 'PTT does not hold capture until the post-abort transport fence is READY'
}
if (($uplinkSource -notmatch 'voice_assistant_downlink_begin_response_wait') -or
    ($uplinkSource -notmatch 'xiaozhi_foundation_audio_uplink_stop_for_response') -or
    ($uplinkSource -notmatch 'response_wait_started\s*\?\s*xiaozhi_foundation_audio_uplink_stop_for_response')) {
    throw 'Reserved current response is not admitted before the stop-listening send path'
}
Write-Output 'voice downlink TTS interruption boundary: PASS'
Write-Output 'voice downlink response-epoch boundary: PASS'
Write-Output 'voice post-abort transport fence boundary: PASS'
