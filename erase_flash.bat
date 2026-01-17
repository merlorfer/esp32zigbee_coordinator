@echo off
set MSYSTEM=
set MSYS=
set CYGWIN=
set TERM=
set SHELL=
cd /d "D:\Programing\esp-idf\v5.5.1\esp-idf"
call "D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat"
cd /d "D:\Programing\esp-idf\projects\AiAgent\CLCode01"
echo Erasing flash on COM9...
idf.py -p COM9 erase_flash
echo Flash erased. Now run flash.bat to reprogram.
pause
