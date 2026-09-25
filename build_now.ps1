Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:CYGWIN -ErrorAction SilentlyContinue
Set-Location "D:\Programing\esp-idf\projects\AiAgent\CLCode01"
. "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1" | Out-Null
idf.py build 2>&1 | Tee-Object -FilePath "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build_log.txt"
