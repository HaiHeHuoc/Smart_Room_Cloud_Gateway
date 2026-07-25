$ErrorActionPreference = "Stop"

# Firebase Web API key from components/cloud/cloud_manager/README.txt.
$apiKey = "AIzaSyBXsyDzNYGd0xxRDvms8nnwtuIYwR3h8ks"
$email = "tranlonghai21@gmail.com"
$password = "Musaking888"

if ([string]::IsNullOrWhiteSpace($password))
{
    $securePassword = Read-Host `
        "Firebase password for $email" `
        -AsSecureString

    $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
        $securePassword)

    try
    {
        $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            $passwordPointer)
    }
    finally
    {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR(
            $passwordPointer)
    }
}

$loginUri =
    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=$apiKey"

$loginBody = @{
    email             = $email
    password          = $password
    returnSecureToken = $true
} | ConvertTo-Json

try
{
    $authResponse = Invoke-RestMethod `
        -Method Post `
        -Uri $loginUri `
        -ContentType "application/json" `
        -Body $loginBody

    Write-Host "Firebase login successful"
    Write-Host "UID:        $($authResponse.localId)"
    Write-Host "Expires in: $($authResponse.expiresIn) seconds"

    # Do not print these values.
    $idToken = $authResponse.idToken
    $refreshToken = $authResponse.refreshToken
}
catch
{
    Write-Error "Firebase login failed: $($_.Exception.Message)"
    exit 1
}

$databaseUrl =
    "https://esp32-smart-room-gateway-default-rtdb." +
    "asia-southeast1.firebasedatabase.app"

$databaseUri =
    "$databaseUrl/devices/esp32s3-001/latest.json" +
    "?auth=$idToken&print=silent"

$telemetryBody = @{
    temperature_c    = 30.1
    humidity_percent = 64.5
    sensor_valid     = $true
    sensor_stale     = $false
    source           = "authenticated_powershell"
} | ConvertTo-Json

try
{
    Invoke-RestMethod `
        -Method Put `
        -Uri $databaseUri `
        -ContentType "application/json" `
        -Body $telemetryBody

    Write-Host "Authenticated Firebase PUT successful"
}
catch
{
    Write-Error "Authenticated Firebase PUT failed: $($_.Exception.Message)"
    exit 1
}
