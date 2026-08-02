param(
    [string]$ApiKey = $env:FIREBASE_API_KEY,
    [string]$Email = $env:FIREBASE_DEVICE_EMAIL,
    [string]$Password = $env:FIREBASE_DEVICE_PASSWORD,
    [string]$ExpectedUid = $env:FIREBASE_DEVICE_UID,
    [string]$DatabaseUrl = $env:FIREBASE_DATABASE_URL,
    [string]$DeviceId = $env:FIREBASE_DEVICE_ID
)

$ErrorActionPreference = "Stop"

function Assert-RequiredValue
{
    param(
        [string]$Name,
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value))
    {
        throw "Missing required value: $Name"
    }
}

if ([string]::IsNullOrWhiteSpace($DeviceId))
{
    $DeviceId = "esp32s3-001"
}

Assert-RequiredValue "FIREBASE_API_KEY" $ApiKey
Assert-RequiredValue "FIREBASE_DEVICE_EMAIL" $Email
Assert-RequiredValue "FIREBASE_DATABASE_URL" $DatabaseUrl

if ([string]::IsNullOrWhiteSpace($Password))
{
    $securePassword = Read-Host `
        "Firebase password for the dedicated device account" `
        -AsSecureString

    $passwordPointer =
        [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
            $securePassword)

    try
    {
        $Password =
            [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
                $passwordPointer)
    }
    finally
    {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR(
            $passwordPointer)
    }
}

Assert-RequiredValue "Firebase device password" $Password

$loginUri =
    "https://identitytoolkit.googleapis.com/v1/" +
    "accounts:signInWithPassword?key=$ApiKey"

$loginBody = @{
    email             = $Email
    password          = $Password
    returnSecureToken = $true
} | ConvertTo-Json -Compress

$idToken = $null
$refreshToken = $null

try
{
    $authResponse = Invoke-RestMethod `
        -Method Post `
        -Uri $loginUri `
        -ContentType "application/json" `
        -Body $loginBody

    if (-not [string]::IsNullOrWhiteSpace($ExpectedUid) -and
        $authResponse.localId -ne $ExpectedUid)
    {
        throw "Firebase UID does not match FIREBASE_DEVICE_UID"
    }

    Write-Host "Firebase login successful"
    Write-Host "UID guard:  $(-not [string]::IsNullOrWhiteSpace($ExpectedUid))"
    Write-Host "Expires in: $($authResponse.expiresIn) seconds"

    # Never print these values.
    $idToken = $authResponse.idToken
    $refreshToken = $authResponse.refreshToken

    Assert-RequiredValue "Firebase ID token" $idToken

    $databaseBase = $DatabaseUrl.TrimEnd('/')
    $escapedDeviceId = [Uri]::EscapeDataString($DeviceId)
    $databaseUri =
        "$databaseBase/devices/$escapedDeviceId/latest.json" +
        "?auth=$([Uri]::EscapeDataString($idToken))&print=silent"

    $telemetryBody = @{
        temperature_c    = 30.1
        humidity_percent = 64.5
        sensor_valid     = $true
        sensor_stale     = $false
        sensor_state     = 3
        last_error       = 0
        sample_uptime_ms = 123456
        source           = "esp32_cloud_manager"
    } | ConvertTo-Json -Compress

    Invoke-RestMethod `
        -Method Put `
        -Uri $databaseUri `
        -ContentType "application/json" `
        -Body $telemetryBody

    Write-Host "Authenticated Firebase PUT successful"
}
catch
{
    Write-Error "Firebase authentication test failed: $($_.Exception.Message)"
    exit 1
}
finally
{
    $loginBody = $null
    $Password = $null
    $idToken = $null
    $refreshToken = $null
}
