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
    (Join-Path $componentRoot 'src\local_web_path_policy.c') `
    (Join-Path $testRoot 'test_local_web_path_policy.c') `
    -o (Join-Path $outputRoot 'local_web_path_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Local Web path-policy test build failed' }
& (Join-Path $outputRoot 'local_web_path_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Local Web path-policy tests failed' }

Write-Output 'local Web path policy: PASS'
