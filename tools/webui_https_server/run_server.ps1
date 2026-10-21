# Serves the webUI (web/ folder) over HTTPS from this PC for phone access.
# See README.md for the one-time phone-side setup and what to do after
# updating the web/ files.
param(
    [int]$Port = 8443
)

Set-Location $PSScriptRoot
python serve.py $Port
