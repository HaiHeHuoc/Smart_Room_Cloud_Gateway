$ErrorActionPreference = "Stop"

$databaseUrl = "https://esp32-smart-room-gateway-default-rtdb.asia-southeast1.firebasedatabase.app"
$uri = "$databaseUrl/devices/esp32s3-001/latest.json"

$body = @{
    temperature_c     = 29.8
    humidity_percent  = 66.3
    sensor_valid      = $true
    sensor_stale      = $false
    uptime_ms         = 123456
} | ConvertTo-Json

Write-Host "Request URL:"
Write-Host $uri

Write-Host "`nJSON body:"
Write-Host $body

try
{
    $response = Invoke-RestMethod `
        -Method Put `
        -Uri $uri `
        -ContentType "application/json" `
        -Body $body

    Write-Host "`nFirebase PUT successful."
    $response | ConvertTo-Json
}
catch
{
    Write-Error "Firebase PUT failed: $($_.Exception.Message)"
    exit 1
}