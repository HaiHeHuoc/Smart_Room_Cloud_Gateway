$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$audioHostInclude = Join-Path $repoRoot 'components\audio\audio_manager\test\host\include'
$outputRoot = Join-Path $repoRoot 'build\host_wake_runtime_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
& $gcc -std=c11 -Wall -Wextra -Werror `
    -I $audioHostInclude `
    -I (Join-Path $componentRoot 'include') `
    -I (Join-Path $componentRoot 'modules\runtime\include') `
    (Join-Path $componentRoot 'modules\runtime\src\wake_word_runtime_policy.c') `
    (Join-Path $testRoot 'test_wake_word_runtime_policy.c') `
    -o (Join-Path $outputRoot 'wake_word_runtime_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Wake runtime policy test build failed' }
& (Join-Path $outputRoot 'wake_word_runtime_policy_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Wake runtime policy tests failed' }
