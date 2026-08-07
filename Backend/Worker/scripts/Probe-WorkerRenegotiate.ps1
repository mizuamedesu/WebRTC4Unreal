[CmdletBinding()]
param(
    [string]$WorkerUrl = 'https://webrtc4unreal-realtime.fasutotesuto.workers.dev',
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
        if ($value.Length -ge 2 -and ((($value[0] -eq '"') -and ($value[$value.Length - 1] -eq '"')) -or
            (($value[0] -eq "'") -and ($value[$value.Length - 1] -eq "'")))) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $result[$name] = $value
    }
    return $result
}

function Read-ErrorResponse {
    param([Parameter(Mandatory)]$ErrorRecord)
    $response = $ErrorRecord.Exception.Response
    if (-not $response) { return [PSCustomObject]@{ Status = 0; Body = $null } }
    $text = [string]$ErrorRecord.ErrorDetails.Message
    if ([string]::IsNullOrWhiteSpace($text)) {
        $stream = $response.GetResponseStream()
        $reader = [IO.StreamReader]::new($stream)
        try { $text = $reader.ReadToEnd() }
        finally { $reader.Dispose(); $stream.Dispose() }
    }
    $body = $null
    try { $body = $text | ConvertFrom-Json } catch { }
    return [PSCustomObject]@{ Status = [int]$response.StatusCode; Body = $body }
}

$values = Read-DotEnvValues -Path (Resolve-Path -LiteralPath $EnvFile).Path
if (-not $values.ContainsKey('CLIENT_ACCESS_KEY')) { throw 'CLIENT_ACCESS_KEY is missing.' }
$baseUrl = $WorkerUrl.TrimEnd('/')
$room = $null
$participantToken = $null
try {
    $roomResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/v1/rooms" -Headers @{
        'X-P2P-Bootstrap-Key' = [string]$values['CLIENT_ACCESS_KEY']; Accept = 'application/json'
    } -ContentType 'application/json' -Body '{"room_name":"Renegotiate probe","max_participants":2}'
    $room = $roomResponse.Content | ConvertFrom-Json
    $participantToken = [string]$room.participant_token
    $participantBase = "$baseUrl/v1/rooms/$($room.room_id)/participants/$($room.participant_id)"
    $authHeaders = @{ Authorization = 'Bearer ' + $participantToken; Accept = 'application/json' }

    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$participantBase/session" -Headers $authHeaders `
        -ContentType 'application/json' -Body '{}' | Out-Null
    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$participantBase/realtime/establish" -Headers $authHeaders `
        -ContentType 'application/json' -Body '{}' | Out-Null

    try {
        Invoke-WebRequest -UseBasicParsing -Method Put -Uri "$participantBase/realtime/renegotiate" `
            -Headers $authHeaders -ContentType 'application/json' `
            -Body '{"sessionDescription":{"type":"answer","sdp":"v=0 invalid answer payload with sufficient length"}}' | Out-Null
        Write-Output 'INVALID_RENEGOTIATE_UNEXPECTEDLY_SUCCEEDED=true'
    }
    catch {
        $result = Read-ErrorResponse -ErrorRecord $_
        $hasNestedError = $null -ne $result.Body -and $null -ne $result.Body.error
        Write-Output ('INVALID_RENEGOTIATE_STATUS=' + $result.Status)
        Write-Output ('NESTED_ERROR_PRESENT=' + $hasNestedError)
        if ($hasNestedError) {
            Write-Output ('ERROR_CODE=' + [string]$result.Body.error.code)
            Write-Output ('ERROR_MESSAGE=' + [string]$result.Body.error.message)
        }
        elseif ($null -ne $result.Body) {
            Write-Output ('UPSTREAM_ERROR_CODE=' + [string]$result.Body.errorCode)
            Write-Output ('UPSTREAM_ERROR_DESCRIPTION=' + [string]$result.Body.errorDescription)
        }
    }
}
finally {
    if ($null -ne $room -and -not [string]::IsNullOrWhiteSpace($participantToken)) {
        try {
            Invoke-WebRequest -UseBasicParsing -Method Delete `
                -Uri "$baseUrl/v1/rooms/$($room.room_id)/participants/$($room.participant_id)" `
                -Headers @{ Authorization = 'Bearer ' + $participantToken } | Out-Null
        } catch { }
    }
    $participantToken = $null
    $values.Clear()
}
