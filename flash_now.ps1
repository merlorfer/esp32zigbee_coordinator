Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:CYGWIN -ErrorAction SilentlyContinue
. "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1" | Out-Null

$port = if ($args[0]) { $args[0] } else { "COM9" }

python "$env:IDF_PATH\components\esptool_py\esptool\esptool.py" `
    --chip esp32c6 `
    --port $port `
    --baud 460800 `
    --before default_reset `
    --after hard_reset `
    write_flash `
    --flash_mode dio `
    --flash_freq 80m `
    --flash_size 4MB `
    0x0       "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\bootloader\bootloader.bin" `
    0x8000    "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\partition_table\partition-table.bin" `
    0x10000   "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\esp32c6_zigbee_gateway.bin" `
    0x1F5000  "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\storage.bin"
