$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_xiaozhi_mcp_audio_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'include') `
    -I (Join-Path $componentRoot 'modules\mcp_audio_playback\include') `
    (Join-Path $componentRoot 'modules\mcp_audio_playback\src\xiaozhi_mcp_audio_playback_policy.c') `
    (Join-Path $testRoot 'test_mcp_audio_playback_policy.c') `
    -o (Join-Path $outputRoot 'mcp_audio_playback_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'MCP audio playback policy test build failed' }
& (Join-Path $outputRoot 'mcp_audio_playback_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'MCP audio playback policy tests failed' }

$toolSource = Get-Content `
    (Join-Path $componentRoot 'modules\mcp_audio_playback\src\xiaozhi_mcp_audio_playback.c') `
    -Raw
if ($toolSource -notmatch '"audio\.get_playback_state"') {
    throw 'Read-only playback-state MCP tool name is missing'
}
if (($toolSource -notmatch '"audio\.list_tracks"') -or
    ($toolSource -notmatch '"audio\.play_track"') -or
    ($toolSource -notmatch '"audio\.play_recorded"')) {
    throw 'Phase 18.2.2 audio MCP tool registration is incomplete'
}
if ($toolSource -notmatch 'first_track_is_first_in_lexical_order') {
    throw 'Catalog text result does not identify deterministic first-track ordering'
}
if ($toolSource -notmatch 'size_bytes') {
    throw 'Catalog result does not expose bounded file-size metadata'
}
if ($toolSource -notmatch '\\"readOnlyHint\\":true') {
    throw 'Playback-state MCP readOnlyHint is not true'
}
if ($toolSource -match 'audio_manager_|i2s_|FILE\s*\*') {
    throw 'Xiaozhi MCP module bypasses the project-owned provider boundary'
}
Write-Output 'MCP read-only state boundary: PASS'
Write-Output 'MCP bounded track-tool boundary: PASS'
