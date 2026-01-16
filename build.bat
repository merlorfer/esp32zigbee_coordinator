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
echo Cleaning build directory...
rmdir /s /q build 2>nul
echo Building project...
idf.py build
if errorlevel 1 echo Build failed with error %errorlevel%
echo Build completed
