@echo off
REM Quick rebuild without cleaning
call D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat
cd /d D:\Programing\esp-idf\projects\AiAgent\CLCode01
idf.py build
