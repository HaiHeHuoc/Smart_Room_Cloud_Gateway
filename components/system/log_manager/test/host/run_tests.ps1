$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_log_manager_tests'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$exe = Join-Path $outputRoot 'log_manager_tests.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pthread -I "$testRoot/include" -I "$componentRoot/include" -I "$componentRoot/../app_log/include" -I $componentRoot "$testRoot/platform.c" "$componentRoot/log_buffer.c" "$componentRoot/../app_log/app_log.c" "$testRoot/test_log_manager.c" -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Host logger test compilation failed' }
# Isolate test-generated files per run; never delete or access real SD data.
$runRoot = Join-Path $outputRoot ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path (Join-Path $runRoot 'sd') -Force | Out-Null
Push-Location $runRoot
try {
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Host logger tests failed: $LASTEXITCODE" }
} finally { Pop-Location }
$disabledExe = Join-Path $outputRoot 'log_manager_disabled_tests.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pthread -DCONFIG_LOG_MANAGER_ENABLE=0 -I "$testRoot/include" -I "$componentRoot/include" -I "$componentRoot/../app_log/include" -I $componentRoot "$testRoot/platform.c" "$componentRoot/log_buffer.c" "$componentRoot/../app_log/app_log.c" "$testRoot/test_disabled.c" -o $disabledExe
if ($LASTEXITCODE -ne 0) { throw 'Disabled logger test compilation failed' }
& $disabledExe
if ($LASTEXITCODE -ne 0) { throw 'Disabled logger test failed' }
$durabilityExe = Join-Path $outputRoot 'log_manager_durability_tests.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pthread -I "$testRoot/include" -I "$componentRoot/include" -I "$componentRoot/../app_log/include" -I $componentRoot "$testRoot/platform.c" "$componentRoot/log_buffer.c" "$componentRoot/../app_log/app_log.c" "$testRoot/test_durability.c" -o $durabilityExe
if ($LASTEXITCODE -ne 0) { throw 'Durability regression compilation failed' }
foreach ($case in @('flush', 'stop', 'flush_close', 'stop_close', 'background', 'background_close', 'start_fail', 'partial_write')) {
    $caseRoot = Join-Path $outputRoot ([guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Path (Join-Path $caseRoot 'sd') -Force | Out-Null
    Push-Location $caseRoot
    try {
        & $durabilityExe $case
        if ($LASTEXITCODE -ne 0) { throw "Durability regression failed: $case ($LASTEXITCODE)" }
    } finally { Pop-Location }
}
