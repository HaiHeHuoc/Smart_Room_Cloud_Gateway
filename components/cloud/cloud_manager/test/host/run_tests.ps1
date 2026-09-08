$ErrorActionPreference = 'Stop'

$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$componentRoot = (Resolve-Path (Join-Path $testRoot '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..')).Path
$outputRoot = Join-Path $repoRoot 'build\host_cloud_telemetry_json_tests'
$testExecutable = Join-Path $outputRoot 'cloud_telemetry_json_tests.exe'
$gcc = (Get-Command gcc -ErrorAction Stop).Source
$telemetryModule = Join-Path $componentRoot 'modules\telemetry_json'

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $gcc `
    -std=c11 `
    -Wall `
    -Wextra `
    -Werror `
    -I (Join-Path $testRoot 'include') `
    -I (Join-Path $telemetryModule 'include') `
    -I (Join-Path $componentRoot 'include') `
    (Join-Path $telemetryModule 'src\cloud_telemetry_json.c') `
    (Join-Path $testRoot 'test_cloud_telemetry_json.c') `
    -o $testExecutable

if ($LASTEXITCODE -ne 0) {
    throw "Host cloud telemetry JSON test build failed with exit code $LASTEXITCODE"
}

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host cloud telemetry JSON tests failed with exit code $LASTEXITCODE"
}
