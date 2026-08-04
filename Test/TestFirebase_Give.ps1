param(
    [string]$DatabaseUrl = $env:FIREBASE_DATABASE_URL,
    [string]$DeviceId = $env:FIREBASE_DEVICE_ID,
    [string]$IdToken = $env:FIREBASE_ID_TOKEN
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($DeviceId))
{
    $DeviceId = "esp32s3-001"
}

if ([string]::IsNullOrWhiteSpace($DatabaseUrl))
{
    throw "Set FIREBASE_DATABASE_URL before running this test."
}

if ([string]::IsNullOrWhiteSpace($IdToken))
{
    throw "Set a short-lived FIREBASE_ID_TOKEN before running this test."
}

$databaseBase = $DatabaseUrl.TrimEnd('/')
$escapedDeviceId = [Uri]::EscapeDataString($DeviceId)
$uri =
    "$databaseBase/devices/$escapedDeviceId/latest.json" +
    "?auth=$([Uri]::EscapeDataString($IdToken))&print=silent"

$body = @{
    temperature_c     = 29.8
    humidity_percent  = 66.3
    sensor_valid      = $true
    sensor_stale      = $false
    sensor_state      = 3
    last_error        = 0
    sample_uptime_ms  = 123456
    source            = "esp32_cloud_manager"
} | ConvertTo-Json -Compress

try
{
    Invoke-RestMethod `
        -Method Put `
        -Uri $uri `
        -ContentType "application/json" `
        -Body $body

    Write-Host "Authenticated Firebase PUT successful."
}
catch
{
    Write-Error "Firebase PUT failed: $($_.Exception.Message)"
    exit 1
}
finally
{
    $IdToken = $null
}
