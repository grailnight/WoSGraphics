@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /MT /EHsc /std:c++17 parser_test.cpp /Fe:parser_test.exe
if errorlevel 1 exit /b 1
parser_test.exe
