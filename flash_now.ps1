Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:CYGWIN -ErrorAction SilentlyContinue
$env:IDF_PATH = "D:\Programing\esp-idf\v5.5.1\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "D:\Programing\esp-idf\tools\python_env\idf5.5_py3.11_env"
$env:PATH = "D:\Programing\esp-idf\tools\python_env\idf5.5_py3.11_env\Scripts;D:\Programing\esp-idf\tools\tools\idf-python\3.11.2;D:\Users\meren\.espressif\tools\cmake\3.30.2\bin;D:\Users\meren\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;D:\Users\meren\.espressif\tools\ninja\1.12.1;D:\Users\meren\.espressif\tools\ccache\4.11.2\ccache-4.11.2-windows-x86_64;" + $env:PATH

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
    0x0    "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\bootloader\bootloader.bin" `
    0x8000 "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\partition_table\partition-table.bin" `
    0x10000 "D:\Programing\esp-idf\projects\AiAgent\CLCode01\build\esp32c6_zigbee_gateway.bin"
