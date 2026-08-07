[CmdletBinding()]
param(
    [string]$EnvFile = (Join-Path $PSScriptRoot '..\..\..\.env')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-DotEnvValues {
    param([Parameter(Mandatory)][string]$Path)
    $result = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$') { continue }
        $name = $Matches[1]
        $value = $Matches[2].Trim()
        if ($value.Length -ge 2) {
            $quoted = ($value.StartsWith('"') -and $value.EndsWith('"')) -or
                ($value.StartsWith("'") -and $value.EndsWith("'"))
            if ($quoted) { $value = $value.Substring(1, $value.Length - 2) }
        }
        $result[$name] = $value
    }
    return $result
}

function Read-ErrorResponse {
    param([Parameter(Mandatory)]$Exception)
    $response = $Exception.Response
    if (-not $response) { return [PSCustomObject]@{ Status = 0; Body = $null } }
    $stream = $response.GetResponseStream()
    $reader = [IO.StreamReader]::new($stream)
    try { $text = $reader.ReadToEnd() }
    finally { $reader.Dispose(); $stream.Dispose() }
    $body = $null
    try { $body = $text | ConvertFrom-Json } catch { }
    return [PSCustomObject]@{ Status = [int]$response.StatusCode; Body = $body }
}

$values = Read-DotEnvValues -Path (Resolve-Path -LiteralPath $EnvFile).Path
foreach ($name in @('CALLS_APP_ID', 'CALLS_APP_SECRET')) {
    if (-not $values.ContainsKey($name) -or [string]::IsNullOrWhiteSpace([string]$values[$name])) {
        throw "Missing or empty $name"
    }
}

$headers = @{ Authorization = 'Bearer ' + [string]$values['CALLS_APP_SECRET']; Accept = 'application/json' }
$baseUrl = 'https://rtc.live.cloudflare.com/v1/apps/' + [uri]::EscapeDataString([string]$values['CALLS_APP_ID'])
$variants = [ordered]@{
    camel_wrapper = @{ dataChannel = @{ location = 'remote'; dataChannelName = 'server-events' } }
    lower_wrapper = @{ datachannel = @{ location = 'remote'; dataChannelName = 'server-events' } }
    flat_object = @{ location = 'remote'; dataChannelName = 'server-events' }
}

try {
    foreach ($variant in $variants.GetEnumerator()) {
        $sessionResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/sessions/new" -Headers $headers
        $session = $sessionResponse.Content | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace([string]$session.sessionId)) { throw 'Session creation returned no ID.' }
        $uri = "$baseUrl/sessions/$($session.sessionId)/datachannels/establish"
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Method Post -Uri $uri -Headers $headers `
                -ContentType 'application/json' -Body ($variant.Value | ConvertTo-Json -Depth 5 -Compress)
            $body = $response.Content | ConvertFrom-Json
            Write-Output ("ESTABLISH_VARIANT={0} STATUS={1} OFFER_PRESENT={2}" -f
                $variant.Key, [int]$response.StatusCode,
                (-not [string]::IsNullOrWhiteSpace([string]$body.sessionDescription.sdp)))
            break
        }
        catch {
            $errorResponse = Read-ErrorResponse -Exception $_.Exception
            $errorCode = if ($errorResponse.Body) { [string]$errorResponse.Body.errorCode } else { '' }
            $errorDescription = if ($errorResponse.Body) { [string]$errorResponse.Body.errorDescription } else { '' }
            Write-Output ("ESTABLISH_VARIANT={0} STATUS={1} ERROR_CODE={2} ERROR_DESCRIPTION={3}" -f
                $variant.Key, $errorResponse.Status, $errorCode, $errorDescription)
        }
    }
}
finally {
    $headers = $null
    $values.Clear()
}
