@echo on
set MSYSTEM=
set MSYS=
set CYGWIN=
set TERM=
set SHELL=
cd /d "D:\Programing\esp-idf\v5.5.1\esp-idf"
call "D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat"
if errorlevel 1 echo Export failed with error %errorlevel%
cd /d "D:\Programing\esp-idf\projects\AiAgent\CLCode01"
echo Flashing to COM9...
idf.py -p COM9 flash
if errorlevel 1 echo Flash failed with error %errorlevel%
echo Flash completed
