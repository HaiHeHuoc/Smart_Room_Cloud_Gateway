$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$sourceRoot = Join-Path $componentRoot 'modules\provider\src'
$outputRoot = Join-Path $repoRoot 'build\host_smart_room_mcp_adapter_tests'
$gcc = (Get-Command gcc -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc -std=c11 -Wall -Wextra -Werror `
    -I $sourceRoot `
    (Join-Path $sourceRoot 'smart_room_mcp_audio_catalog_entry.c') `
    (Join-Path $testRoot 'test_smart_room_mcp_audio_catalog_entry.c') `
    -o (Join-Path $outputRoot 'smart_room_mcp_audio_catalog_entry_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Audio catalog entry test build failed' }
& (Join-Path $outputRoot 'smart_room_mcp_audio_catalog_entry_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Audio catalog entry tests failed' }

Write-Output 'smart_room_mcp_adapter audio catalog entry invariants: PASS'
