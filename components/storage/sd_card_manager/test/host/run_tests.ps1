$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_sd_card_manager_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I $componentRoot `
    (Join-Path $componentRoot 'sd_card_manager_usage.c') `
    (Join-Path $testRoot 'test_sd_card_manager_usage.c') `
    -o (Join-Path $outputRoot 'sd_card_manager_usage_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'SD capacity test build failed' }
& (Join-Path $outputRoot 'sd_card_manager_usage_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'SD capacity tests failed' }

Write-Output 'sd_card_manager capacity invariants: PASS'
