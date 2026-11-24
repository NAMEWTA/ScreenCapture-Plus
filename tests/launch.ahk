#NoEnv
SetWorkingDir %A_ScriptDir%\..

^!h::Run, artifacts\bin\x64\Release\ScreenCapture.exe --auto-quit=true
