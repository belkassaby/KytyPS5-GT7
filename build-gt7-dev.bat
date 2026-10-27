@echo off
rem Like build-gt7.bat, into _Build\windows-noqt-dev: builds while a game started from
rem _Build\windows-noqt is still running.
call "%~dp0build-gt7.bat" windows-noqt-dev 16
