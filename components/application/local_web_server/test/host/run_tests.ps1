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
    ($webPage -notmatch 'activeTab !== ''lights''')) {
    throw 'Lights UI is missing bounded polling, debounced writes, or stale-response protection'
}

Write-Output 'local Web path policy: PASS'
