@echo off
set IDF_PATH=D:\Programing\esp-idf\v5.5.1\esp-idf
set PATH=%PATH%;D:\Users\meren\.espressif\tools\cmake\3.30.2\bin;D:\Users\meren\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;D:\Users\meren\.espressif\tools\ninja\1.12.1;D:\Users\meren\.espressif\tools\ccache\4.11.2\ccache-4.11.2-windows-x86_64
cd /d D:\Programing\esp-idf\projects\AiAgent\CLCode01\build
cmake --build . > D:\Programing\esp-idf\projects\AiAgent\CLCode01\build_log.txt 2>&1
echo EXIT_CODE=%ERRORLEVEL% >> D:\Programing\esp-idf\projects\AiAgent\CLCode01\build_log.txt
