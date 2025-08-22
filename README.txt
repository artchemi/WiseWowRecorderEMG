Remove-Item -Recurse -Force build
Remove-Item -Recurse -Force CMakeCache.txt
Remove-Item -Recurse -Force CMakeFiles

mkdir build
cd build

Индексы и значения в пакете


cmake .. -G "MinGW Makefiles"

mingw32-make

v1 - отрисовка фильтрованого графика
