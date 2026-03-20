@echo off
cd /d "E:\--PROJECTOS--\wowbuilder006"

set /p msg=Mensagem do commit: 

git add .
git commit -m "%msg%"
git push