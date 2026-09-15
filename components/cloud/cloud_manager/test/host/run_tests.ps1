$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_cloud_push_latest_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $componentRoot 'include') `
    -I (Join-Path $componentRoot 'modules\push_latest_policy\include') `
    (Join-Path $componentRoot 'modules\push_latest_policy\src\cloud_push_latest_policy.c') `
    (Join-Path $testRoot 'test_cloud_push_latest_policy.c') `
    -o (Join-Path $outputRoot 'cloud_push_latest_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Cloud push-latest policy test build failed' }
& (Join-Path $outputRoot 'cloud_push_latest_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Cloud push-latest policy tests failed' }

$managerSource = Get-Content (Join-Path $componentRoot 'cloud_manager.c') -Raw
if (($managerSource -notmatch 'cloud_manager_request_push_latest') -or
    ($managerSource -notmatch 's_forced_push_pending') -or
    ($managerSource -notmatch 'CLOUD_MANAGER_NOTIFY_FORCED_PUSH') -or
    ($managerSource -notmatch 'CLOUD_DELAY_PUBLISH_PERIOD') -or
    ($managerSource -notmatch 'CLOUD_DELAY_RETRY')) {
    throw 'Cloud manager lacks the bounded forced-push scheduling path'
}
if ($managerSource -notmatch 'cloud_manager_clear_forced_push_pending\(\);') {
    throw 'Cloud manager does not release the outstanding request after success'
}
Write-Output 'Cloud push-latest manager boundary: PASS'
