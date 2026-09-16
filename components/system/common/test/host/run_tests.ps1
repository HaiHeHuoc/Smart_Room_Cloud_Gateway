$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = Resolve-Path (Join-Path $testRoot '..\..')
$repoRoot = Resolve-Path (Join-Path $componentRoot '..\..\..')
$outputRoot = Join-Path $testRoot 'out'

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$compiler = Get-Command gcc -ErrorAction Stop
& $compiler.Source -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'include') `
    (Join-Path $componentRoot 'voice_recording_critical.c') `
    (Join-Path $testRoot 'test_voice_recording_critical.c') `
    -o (Join-Path $outputRoot 'voice_recording_critical_tests.exe')

& (Join-Path $outputRoot 'voice_recording_critical_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Voice recording critical runtime tests failed' }

# These source-boundary checks cover the production ownership sequence that a
# host test cannot execute without ESP-IDF audio, I2S and Xiaozhi dependencies.
$uplinkSource = Get-Content `
    (Join-Path $repoRoot 'components\application\voice_assistant\modules\uplink\src\voice_assistant_uplink.c') `
    -Raw
if ($uplinkSource -notmatch 'voice_assistant_audio_capture_start\(\);\s*if \(ret != ESP_OK\)[\s\S]{0,1200}return ret;[\s\S]{0,400}voice_recording_critical_enter') {
    throw 'Critical window can enter before capture-start failure cleanup'
}
if ($uplinkSource -notmatch 'voice_assistant_audio_capture_stop\(\);[\s\S]{0,800}voice_recording_critical_exit') {
    throw 'Critical window does not exit after the bounded capture-stop path'
}
if (($uplinkSource -notmatch 'capture_lost') -or
    ($uplinkSource -notmatch 'transport_lost') -or
    ($uplinkSource -notmatch 'voice_assistant_ptt_cancel\(\)') -or
    ($uplinkSource -notmatch 's_turn_terminal_cancel_pending')) {
    throw 'Terminal capture or transport loss lacks one bounded cleanup path'
}

$logSource = Get-Content `
    (Join-Path $repoRoot 'components\system\log_manager\log_manager.c') `
    -Raw
if ($logSource -notmatch 'voice_recording_critical_is_active\(\)[\s\S]{0,1000}park_file\(\)[\s\S]{0,1200}xTaskNotifyWait') {
    throw 'Log writer does not release file state before critical-window wait'
}

$cloudSource = Get-Content `
    (Join-Path $repoRoot 'components\cloud\cloud_manager\cloud_manager.c') `
    -Raw
if ($cloudSource -notmatch '!forced_push_pending[\s\S]{0,300}voice_recording_critical_is_active\(\)[\s\S]{0,500}cloud_manager_wait_for_notification\(portMAX_DELAY\)') {
    throw 'Cloud periodic defer can affect an accepted forced push'
}

$sensorSource = Get-Content `
    (Join-Path $repoRoot 'components\sensing\sensor_manager\sensor_manager.c') `
    -Raw
if ($sensorSource -notmatch 'voice_recording_critical_is_active\(\)[\s\S]{0,800}vTaskDelayUntil') {
    throw 'Sensor timing transaction does not defer at its cadence boundary'
}

$performanceSource = Get-Content `
    (Join-Path $repoRoot 'components\system\performance_monitor\performance_monitor.c') `
    -Raw
if (($performanceSource -notmatch 'voice_recording_critical_is_active\(\)') -or
    ($performanceSource -notmatch 'ulTaskNotifyTake\(pdTRUE, portMAX_DELAY\)')) {
    throw 'Performance monitor does not cooperatively defer its report cycle'
}

foreach ($source in @($uplinkSource, $logSource, $cloudSource, $sensorSource, $performanceSource)) {
    if ($source -match 'vTaskSuspend\(') {
        throw 'Critical-window implementation must not suspend a worker task'
    }
}

Write-Output 'voice recording critical production-boundary audit: PASS'
