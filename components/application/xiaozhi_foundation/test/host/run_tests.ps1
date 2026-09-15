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

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'include') `
    -I (Join-Path $componentRoot 'modules\mcp_cloud_push_latest\include') `
    (Join-Path $componentRoot 'modules\mcp_cloud_push_latest\src\xiaozhi_mcp_cloud_push_latest_policy.c') `
    (Join-Path $testRoot 'test_mcp_cloud_push_latest_policy.c') `
    -o (Join-Path $outputRoot 'mcp_cloud_push_latest_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'MCP cloud push-latest policy test build failed' }
& (Join-Path $outputRoot 'mcp_cloud_push_latest_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'MCP cloud push-latest policy tests failed' }

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
if (($toolSource -notmatch 's_track_list_callback_busy') -or
    ($toolSource -notmatch 'MALLOC_CAP_SPIRAM') -or
    ($toolSource -notmatch 'EXT_RAM_BSS_ATTR static xiaozhi_foundation_audio_track_list_t') -or
    ($toolSource -notmatch 'playback_confirmed')) {
    throw 'Catalog callback lacks the bounded RAM and playback-confirmation boundary'
}
if ($toolSource -notmatch '\\"readOnlyHint\\":true') {
    throw 'Playback-state MCP readOnlyHint is not true'
}
if ($toolSource -match 'audio_manager_|i2s_|FILE\s*\*') {
    throw 'Xiaozhi MCP module bypasses the project-owned provider boundary'
}
$cloudPushToolSource = Get-Content `
    (Join-Path $componentRoot 'modules\mcp_cloud_push_latest\src\xiaozhi_mcp_cloud_push_latest.c') `
    -Raw
if (($cloudPushToolSource -notmatch '"cloud\.push_latest"') -or
    (-not $cloudPushToolSource.Contains('\"upload_complete\":false')) -or
    (-not $cloudPushToolSource.Contains('\"idempotentHint\":false'))) {
    throw 'Cloud push-latest MCP tool lacks its bounded scheduling semantics'
}
if ($cloudPushToolSource -match '#include\s+"cloud_manager\.h"|firebase_auth|esp_http_client|esp_wifi|nvs_') {
    throw 'Cloud push-latest MCP tool bypasses the project-owned provider boundary'
}
$cloudPushAdapterSource = Get-Content `
    (Join-Path $repoRoot 'components\application\smart_room_mcp_adapter\modules\provider\src\smart_room_mcp_cloud_push_latest.c') `
    -Raw
if (($cloudPushAdapterSource -notmatch 'cloud_manager_request_push_latest') -or
    ($cloudPushAdapterSource -match 'firebase_auth|esp_http_client|esp_wifi|nvs_')) {
    throw 'Cloud push-latest adapter does not remain on the public manager boundary'
}
$adapterTracksSource = Get-Content `
    (Join-Path $repoRoot 'components\application\smart_room_mcp_adapter\modules\provider\src\smart_room_mcp_audio_tracks.c') `
    -Raw
if (($adapterTracksSource -notmatch 'catalog_task') -or
    ($adapterTracksSource -notmatch 'catalog_lock\(0U\)') -or
    ($adapterTracksSource -notmatch 'scan_catalog\(&s_catalog\)')) {
    throw 'Audio catalog SD scan is not isolated in its worker/cache boundary'
}
if ($adapterTracksSource -match 'scan_catalog\(&catalog\)') {
    throw 'Audio catalog still scans SD from the synchronous MCP callback'
}
if (($adapterTracksSource -notmatch 'make_catalog_track_id_unique') -or
    ($adapterTracksSource -notmatch 'catalog_contains_track_id') -or
    ($adapterTracksSource -notmatch 'stem_length == 0U')) {
    throw 'Audio catalog does not disambiguate bounded logical track IDs'
}
$sessionSource = Get-Content `
    (Join-Path $componentRoot 'modules\session\src\xiaozhi_session.c') `
    -Raw
if (($sessionSource -notmatch 's_response_delivery_enabled') -or
    ($sessionSource -notmatch 'xiaozhi_foundation_audio_abort_response') -or
    ($sessionSource -notmatch 'esp_xiaozhi_chat_send_abort_speaking') -or
    ($sessionSource -notmatch 'xiaozhi_foundation_audio_uplink_stop_for_response') -or
    ($sessionSource -notmatch 'admit_reserved_response')) {
    throw 'Foundation lacks the closed-channel response-delivery gate or abort signal'
}
if (($sessionSource -notmatch 'XIAOZHI_SESSION_PRIVATE_FENCE_DRAIN_EVENT_ID') -or
    ($sessionSource -notmatch 'XIAOZHI_SESSION_EVENT_FENCE_DRAINED') -or
    ($sessionSource -notmatch 'esp_event_post\(') -or
    ($sessionSource -notmatch 'esp_xiaozhi_chat_stop\(chat\)') -or
    ($sessionSource -notmatch 'esp_xiaozhi_chat_start\(chat\)')) {
    throw 'Foundation lacks the controlled WebSocket transport fence'
}
if (($sessionSource -notmatch 'xiaozhi_session_set_normal_status_for_generation') -or
    ($sessionSource -notmatch 'xiaozhi_session_normal_generation_is_current') -or
    ($sessionSource -notmatch 'xiaozhi_session_publish_status_snapshot')) {
    throw 'Foundation transport fence does not publish only generation-consistent status'
}
if ($sessionSource -notmatch 's_response_delivery_enabled = valid && admit_reserved_response;[\s\S]{0,500}esp_xiaozhi_chat_send_stop_listening') {
    throw 'Reserved response delivery is not admitted before stop-listening transmit'
}
if ($sessionSource -notmatch 'esp_xiaozhi_chat_stop\(chat\)[\s\S]{0,1800}esp_event_post\([\s\S]{0,2500}esp_xiaozhi_chat_start\(chat\)') {
    throw 'Transport fence does not preserve stop, FIFO drain, then fresh-start ordering'
}
if (($sessionSource -notmatch 'xiaozhi_mcp_cloud_push_latest_attach\(s_mcp\)') -or
    ($sessionSource -notmatch 'xiaozhi_mcp_cloud_push_latest_detach\(\)')) {
    throw 'Cloud push-latest MCP lifecycle attachment is incomplete'
}
Write-Output 'MCP read-only state boundary: PASS'
Write-Output 'MCP bounded track-tool boundary: PASS'
Write-Output 'MCP cloud push-latest provider boundary: PASS'
Write-Output 'MCP asynchronous catalog-cache boundary: PASS'
Write-Output 'Xiaozhi transport packet fence boundary: PASS'
