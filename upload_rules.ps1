param(
    [Parameter(Mandatory=$true)]
    [string]$IP,

    [Parameter(Mandatory=$true)]
    [string]$File
)

if (-not (Test-Path $File)) {
    Write-Error "File not found: $File"
    exit 1
}

$text = Get-Content $File -Raw -Encoding UTF8
$body = @{ text = $text } | ConvertTo-Json -Depth 1

try {
    $response = Invoke-RestMethod -Method POST -Uri "http://$IP/api/rules" `
        -ContentType "application/json" `
        -Body $body
    if ($response.ok) {
        Write-Host "OK - rule_count: $($response.rule_count)"
    } else {
        Write-Host "Parse error: $($response.error)"
        exit 1
    }
} catch {
    Write-Error "Request failed: $_"
    exit 1
}
