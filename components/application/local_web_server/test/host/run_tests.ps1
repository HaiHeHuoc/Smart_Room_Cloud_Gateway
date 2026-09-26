$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_local_web_server_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'src') `
    (Join-Path $componentRoot 'src\local_web_audio_policy.c') `
    (Join-Path $componentRoot 'src\local_web_dashboard_policy.c') `
    (Join-Path $componentRoot 'src\local_web_download.c') `
    (Join-Path $componentRoot 'src\local_web_icon_policy.c') `
    (Join-Path $componentRoot 'src\local_web_light_policy.c') `
    (Join-Path $componentRoot 'src\local_web_path_policy.c') `
    (Join-Path $testRoot 'test_local_web_path_policy.c') `
    -o (Join-Path $outputRoot 'local_web_path_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Local Web path-policy test build failed' }
& (Join-Path $outputRoot 'local_web_path_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Local Web path-policy tests failed' }

$serverSource = Get-Content (Join-Path $componentRoot 'src\local_web_server.c') -Raw
if (($serverSource -notmatch 'LOCAL_WEB_HTTP_ROUTE_COUNT 29U') -or
    ($serverSource -notmatch 'config\.max_uri_handlers = LOCAL_WEB_HTTP_ROUTE_COUNT') -or
    ($serverSource -notmatch '"/api/assets/icon"') -or
    ($serverSource -notmatch 'local_web_icon_logical_path') -or
    ($serverSource -notmatch '"/api/light/status"') -or
    ($serverSource -notmatch '"/api/light/state"') -or
    ($serverSource -notmatch 'light_manager_set_state\(&requested\)') -or
    ($serverSource -notmatch 'app_gui_post_web_light_status')) {
    throw 'HTTPD URI-handler capacity does not cover the registered route set'
}
if (([regex]::Matches($serverSource, '\.uri = ')).Count -ne 27) {
    throw 'HTTPD URI-handler route inventory changed without a capacity review'
}

if (($serverSource -notmatch '"/api/logs/status"') -or
    ($serverSource -notmatch '"/api/logs/files"') -or
    ($serverSource -notmatch '"/api/logs/read"') -or
    ($serverSource -match '"/api/logs/(delete|download)') -or
    ($serverSource -notmatch 'log_manager_read_archive\(id, offset, &page\)') -or
    ($serverSource -match 'fopen\(|fread\(|opendir\(|readdir\(|/sdcard/logs') -or
    ($serverSource -notmatch 'details_omitted')) {
    throw 'Logs REST is not bounded, sanitized, or owner-routed'
}

if (($serverSource -notmatch 'local_web_diagnostics_status_get') -or
    ($serverSource -notmatch 'local_web_diagnostics_export_get') -or
    ($serverSource -notmatch 'local_web_send_responsef\(request,') -or
    ($serverSource -notmatch 'httpd_resp_send\(request, s_response_chunk, written\)')) {
    throw 'Diagnostics REST can leave a one-shot response in chunked mode'
}
$diagnosticsStart = $serverSource.LastIndexOf(
    'static esp_err_t local_web_diagnostics_status_get')
$diagnosticsEnd = $serverSource.LastIndexOf(
    'static esp_err_t local_web_audio_play_post')
$diagnosticsSource = $serverSource.Substring(
    $diagnosticsStart, $diagnosticsEnd - $diagnosticsStart)
if ($diagnosticsSource -match 'return local_web_send_chunkf\(request,') {
    throw 'Diagnostics REST response is not terminated'
}

if (($serverSource -notmatch '"/api/scenes"') -or
    ($serverSource -notmatch '"/api/scenes/status"') -or
    ($serverSource -notmatch '"/api/scenes/apply"') -or
    ($serverSource -notmatch 'scene_manager_apply\(scene_id, &status\)') -or
    ($serverSource -notmatch 'local_web_scene_parse_apply_query') -or
    ($serverSource -match 'scene_manager.*(neopixel|gpio|i2s|lv_)')) {
    throw 'Scenes REST contract or owner boundary is incomplete'
}

$sceneSource = Get-Content (Join-Path $repoRoot 'components\application\scene_manager\scene_manager.c') -Raw
if (($sceneSource -notmatch 'callers need a structured partial outcome') -or
    ($sceneSource -notmatch 'next.outcome == SCENE_MANAGER_OUTCOME_PARTIAL') -or
    ($sceneSource -match '\(status\.light_outcome == SCENE_MANAGER_OUTCOME_UNAVAILABLE\)') -or
    ($serverSource -match '\(status\.audio_outcome == SCENE_MANAGER_OUTCOME_UNAVAILABLE\)')) {
    throw 'Mixed scene owner results can be hidden instead of returned as partial'
}

$logSource = Get-Content (Join-Path $repoRoot 'components\system\log_manager\log_manager.c') -Raw
if (($logSource -notmatch 'ARCHIVE_DIRECTORY_SCAN_MAX') -or
    ($logSource -notmatch 'scan_limit_reached') -or
    ($logSource -notmatch 'directories_scanned') -or
    ($logSource -notmatch 'next_offset_valid')) {
    throw 'Archive scans lack a bounded directory/file traversal fence'
}

if (($serverSource -notmatch '"/api/dashboard/status"') -or
    ($serverSource -notmatch 'local_web_dashboard_status_get') -or
    ($serverSource -match 'dashboard/status".*, .method = HTTP_POST') -or
    ($serverSource -match 'wifi_manager_connect\(') -or
    ($serverSource -match 'wifi_manager_disconnect\(') -or
    ($serverSource -match 'neopixel_') -or
    ($serverSource -match 'lv_')) {
    throw 'Dashboard endpoint is not read-only or bypasses an owner boundary'
}

$webPage = Get-Content (Join-Path $componentRoot 'web\index.html') -Raw
if (($webPage -notmatch 'STORAGE_POLL_INTERVAL_MS = 2000') -or
    ($webPage -notmatch 'async function tickStorage') -or
    ($webPage -notmatch "Storage unavailable\. Reinsert the SD card") -or
    ($webPage -notmatch 'storageAvailable === true && !busy') -or
    ($webPage -notmatch 'storageAvailable !== true')) {
    throw 'Storage UI does not refresh SD availability without a page reload'
}

if (($webPage -notmatch 'id="tab-dashboard"') -or
    ($webPage -notmatch 'id="panel-dashboard"') -or
    ($webPage -notmatch 'data-tab="dashboard"') -or
    ($webPage -notmatch 'id="tab-storage"') -or
    ($webPage -notmatch 'id="tab-playback"') -or
    ($webPage -notmatch 'id="tab-lights"') -or
    ($webPage -notmatch 'activeTab = ''dashboard''') -or
    ($webPage -notmatch 'DASHBOARD_POLL_INTERVAL_MS = 2000') -or
    ($webPage -notmatch 'async function tickDashboard') -or
    ($webPage -notmatch 'dashboardPolling') -or
    ($webPage -notmatch 'dashboardGeneration') -or
    ($webPage -notmatch 'dashboardRefreshQueued') -or
    ($webPage -notmatch 'dashboardActive') -or
    ($webPage -notmatch "fetch\('/api/dashboard/status'") -or
    ($webPage -notmatch 'data\.ok !== true') -or
    ($webPage -notmatch 'dashboard-sensor-primary') -or
    ($webPage -notmatch 'dashboard-storage-primary') -or
    ($webPage -notmatch 'dashboard-network-primary') -or
    ($webPage -notmatch 'dashboard-audio-primary') -or
    ($webPage -notmatch 'dashboard-light-swatch') -or
    ($webPage -notmatch 'dashboard-cloud-primary') -or
    ($webPage -notmatch 'dashboard-time-primary') -or
    ($webPage -notmatch 'dashboard-storage-progress') -or
    ($webPage -notmatch 'role="tablist"') -or
    ($webPage -notmatch 'ArrowLeft') -or
    ($webPage -notmatch 'document\.hidden') -or
    ($webPage -notmatch 'finiteNumber') -or
    ($webPage -match '/api/(wifi|provision|dashboard/[^s])') -or
    ($webPage -match 'innerHTML')) {
    throw 'Dashboard UI is missing safe rendering, tab accessibility, or bounded active polling'
}

if (($webPage -notmatch 'id="tab-lights"') -or
    ($webPage -notmatch 'LIGHT_WRITE_DEBOUNCE_MS = 250') -or
    ($webPage -notmatch 'async function tickLights') -or
    ($webPage -notmatch 'lightGeneration') -or
    ($webPage -notmatch 'lightCommitActive') -or
    ($webPage -notmatch 'lightErrorIsBusy') -or
    ($webPage -notmatch 'Light manager is busy; state will refresh') -or
    ($webPage -notmatch 'requestUpdate\.brightness = requestUpdate\.brightness_percent') -or
    ($webPage -notmatch 'delete requestUpdate\.brightness_percent') -or
    ($webPage -notmatch 'Changes saved and synchronized') -or
    ($webPage -notmatch 'effect === ''rainbow''') -or
    ($webPage -notmatch 'option value="strobe"') -or
    ($webPage -notmatch 'option value="heartbeat"') -or
    ($webPage -notmatch 'option value="candle"') -or
    ($webPage -notmatch 'option value="sos"') -or
    ($webPage -notmatch 'option value="lightning"') -or
    ($webPage -notmatch 'option value="wake_up"') -or
    ($webPage -notmatch 'option value="sleep_fade"') -or
    ($webPage -notmatch 'option value="notification"') -or
    ($webPage -notmatch 'activeTab !== ''lights''')) {
    throw 'Lights UI is missing bounded polling, debounced writes, or stale-response protection'
}

if (($webPage -notmatch 'id="tab-scenes"') -or
    ($webPage -notmatch 'id="tab-logs"') -or
    ($webPage -notmatch 'id="tab-diagnostics"') -or
    ([regex]::Matches($webPage, '<button class="tab"').Count -ne 7) -or
    ($webPage -notmatch 'SCENE_POLL_INTERVAL_MS = 5000') -or
    ($webPage -notmatch "fetch\('/api/scenes'") -or
    ($webPage -notmatch '/api/scenes/apply') -or
    ($webPage -notmatch 'sceneApplyActive') -or
    ($webPage -notmatch 'Last applied:') -or
    ($webPage -notmatch 'Last requested:') -or
    ($webPage -match 'Current Scene:|Active scene:')) {
    throw 'Scenes UI, result semantics, or seven-tab navigation is incomplete'
}

if (($webPage -notmatch "fetch\('/api/logs/status'") -or
    ($webPage -notmatch "fetch\('/api/logs/files'") -or
    ($webPage -notmatch '/api/logs/read\?id=') -or
    ($webPage -notmatch 'details_omitted') -or
    ($webPage -notmatch 'next_offset_valid') -or
    ($webPage -notmatch 'textContent = safeText\(record && record.event\)') -or
    ($webPage -match '/api/logs/(delete|download)') -or
    ($webPage -match 'raw log|filesystem path')) {
    throw 'Logs UI does not preserve the bounded sanitized contract'
}

if (($webPage -notmatch "fetch\('/api/diagnostics/status'") -or
    ($webPage -notmatch '/api/diagnostics/export') -or
    ($webPage -notmatch 'DIAGNOSTICS_POLL_INTERVAL_MS = 5000') -or
    ($webPage -notmatch 'diagnosticsPolling') -or
    ($webPage -notmatch 'diagnosticsActive') -or
    ($webPage -notmatch 'Peak 500 ms') -or
    ($webPage -notmatch 'Performance sample age') -or
    ($webPage -match '/api/diagnostics/(start|measure|control)')) {
    throw 'Diagnostics UI is not read-only, bounded, or freshness-aware'
}

if ((-not $webPage.Contains("details.join(' | ')")) -or
    (-not $webPage.Contains('${bytes(file.size_bytes)} | ${file.time_named === true ? ''dated'' : ''unsynced''}')) -or
    (-not $webPage.Contains('generation === sceneGeneration ? SCENE_POLL_INTERVAL_MS : 0')) -or
    (-not $webPage.Contains('generation === logsGeneration ? LOGS_POLL_INTERVAL_MS : 0')) -or
    (-not $webPage.Contains('generation === diagnosticsGeneration ? DIAGNOSTICS_POLL_INTERVAL_MS : 0')) -or
    (-not $webPage.Contains("setLogsFeedback('Loading logger status...')")) -or
    (-not $webPage.Contains("setDiagnosticsFeedback('Loading the latest copied diagnostic snapshot...')")) -or
    (-not $webPage.Contains('if (logFilesModel) renderLogFiles(logFilesModel);'))) {
    throw 'New-tab load lifecycle can leave stale initial content or disabled archive actions'
}

$effectNames = @(
    'solid', 'blink', 'breath', 'pulse', 'rainbow', 'strobe', 'heartbeat',
    'candle', 'sos', 'lightning', 'wake_up', 'sleep_fade', 'notification'
)
$lightPolicy = Get-Content (Join-Path $componentRoot 'src\local_web_light_policy.c') -Raw
$mcpSetState = Get-Content (Join-Path $repoRoot 'components\application\xiaozhi_foundation\modules\mcp_light_set_state\src\xiaozhi_mcp_light_set_state.c') -Raw
$mcpStateQuery = Get-Content (Join-Path $repoRoot 'components\application\xiaozhi_foundation\modules\mcp_light_state_query\src\xiaozhi_mcp_light_state_query.c') -Raw
$mcpCapabilities = Get-Content (Join-Path $repoRoot 'components\application\xiaozhi_foundation\modules\mcp_light_capabilities\src\xiaozhi_mcp_light_capabilities.c') -Raw
$managerSource = Get-Content (Join-Path $repoRoot 'components\output\light_manager\light_manager.c') -Raw
$managerHeader = Get-Content (Join-Path $repoRoot 'components\output\light_manager\include\light_manager.h') -Raw
$foundationHeader = Get-Content (Join-Path $repoRoot 'components\application\xiaozhi_foundation\include\xiaozhi_foundation.h') -Raw
$providerSet = Get-Content (Join-Path $repoRoot 'components\application\smart_room_mcp_adapter\modules\provider\src\smart_room_mcp_light_set_state.c') -Raw
$providerState = Get-Content (Join-Path $repoRoot 'components\application\smart_room_mcp_adapter\modules\provider\src\smart_room_mcp_light_state.c') -Raw
$guiSource = Get-Content (Join-Path $repoRoot 'components\ui\app_gui\app_gui.c') -Raw
$guiHeader = Get-Content (Join-Path $repoRoot 'components\ui\app_gui\include\app_gui.h') -Raw
if ($managerSource -notmatch 'return effect <= LIGHT_MANAGER_EFFECT_NOTIFICATION') {
    throw 'Light manager does not validate the complete public effect range'
}
foreach ($effectName in $effectNames) {
    $literal = '"' + [regex]::Escape($effectName) + '"'
    $enumSuffix = $effectName.ToUpperInvariant()
    if (($lightPolicy -notmatch $literal) -or
        ($webPage -notmatch ('option value="' + [regex]::Escape($effectName) + '"')) -or
        ($mcpSetState -notmatch $literal) -or
        ($mcpStateQuery -notmatch $literal) -or
        ($mcpCapabilities -notmatch [regex]::Escape($effectName)) -or
        ($managerHeader -notmatch "LIGHT_MANAGER_EFFECT_$enumSuffix") -or
        ($managerSource -notmatch "LIGHT_MANAGER_EFFECT_$enumSuffix") -or
        ($foundationHeader -notmatch "XIAOZHI_FOUNDATION_LIGHT_EFFECT_$enumSuffix") -or
        ($providerSet -notmatch "LIGHT_MANAGER_EFFECT_$enumSuffix") -or
        ($providerState -notmatch "LIGHT_MANAGER_EFFECT_$enumSuffix") -or
        ($guiSource -notmatch "UI_WEB_LIGHT_EFFECT_$enumSuffix") -or
        ($guiHeader -notmatch "UI_WEB_LIGHT_EFFECT_$enumSuffix")) {
        throw "Light effect contract is inconsistent for $effectName"
    }
}

Write-Output 'local Web path policy: PASS'
