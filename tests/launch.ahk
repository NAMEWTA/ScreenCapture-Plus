#NoEnv
SetWorkingDir %A_ScriptDir%\..

^!h::Run, artifacts\bin\x64\Release\huhucapture.exe --auto-quit=true
