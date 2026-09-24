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
if (($serverSource -notmatch 'LOCAL_WEB_HTTP_ROUTE_COUNT 18U') -or
    ($serverSource -notmatch 'config\.max_uri_handlers = LOCAL_WEB_HTTP_ROUTE_COUNT') -or
    ($serverSource -notmatch '"/api/assets/icon"') -or
    ($serverSource -notmatch 'local_web_icon_logical_path') -or
    ($serverSource -notmatch '"/api/light/status"') -or
    ($serverSource -notmatch '"/api/light/state"') -or
    ($serverSource -notmatch 'light_manager_set_state\(&requested\)') -or
    ($serverSource -notmatch 'app_gui_post_web_light_status')) {
    throw 'HTTPD URI-handler capacity does not cover the registered route set'
}

$webPage = Get-Content (Join-Path $componentRoot 'web\index.html') -Raw
if (($webPage -notmatch 'STORAGE_POLL_INTERVAL_MS = 2000') -or
    ($webPage -notmatch 'async function tickStorage') -or
    ($webPage -notmatch "Storage unavailable\. Reinsert the SD card") -or
    ($webPage -notmatch 'storageAvailable === true && !busy') -or
    ($webPage -notmatch 'storageAvailable !== true')) {
    throw 'Storage UI does not refresh SD availability without a page reload'
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
