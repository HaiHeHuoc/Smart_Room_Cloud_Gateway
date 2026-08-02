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
    "?auth=$([Uri]::EscapeDataString($IdToken))"

try
{
    $response = Invoke-RestMethod `
        -Method Get `
        -Uri $uri

    Write-Host "Authenticated Firebase GET successful:"
    $response | Format-List
}
catch
{
    Write-Error "Firebase GET failed: $($_.Exception.Message)"
    exit 1
}
finally
{
    $IdToken = $null
}
