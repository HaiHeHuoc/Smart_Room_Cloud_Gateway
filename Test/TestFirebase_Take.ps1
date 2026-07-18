$ErrorActionPreference = "Stop"

$databaseUrl = "https://esp32-smart-room-gateway-default-rtdb.asia-southeast1.firebasedatabase.app"
$uri = "$databaseUrl/devices/esp32s3-001/latest.json"

Write-Host "Request URL:"
Write-Host $uri

try
{
    $response = Invoke-RestMethod `
        -Method Get `
        -Uri $uri

    Write-Host "`nFirebase GET successful:"
    $response | Format-List
}
catch
{
    Write-Error "Firebase GET failed: $($_.Exception.Message)"
    exit 1
}
