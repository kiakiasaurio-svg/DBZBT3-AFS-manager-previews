# Compilación (Windows)

## Requisitos previos

1. **Visual Studio** 2019 o 2022 (con "Desktop development with C++") o **MinGW** (g++).
2. **CMake** >= 3.10 (https://cmake.org/download/).
3. **Qt 5.15.x** (https://download.qt.io/) instalado con el **Maintenance Tool**, asegurándote de marcar:
   - `Qt 5.15.x` -> tu kit (ej. `MSVC 2019 64-bit` o `MinGW 64-bit`)
   - Componente **Qt Multimedia** (necesario para la función de Preview de archivos ADX)

## Pasos

Desde la raíz del repositorio:

```bat
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
```

La primera vez este comando **fallará a propósito** con el mensaje:

```
Qt path non set
Set it manually in CMakeQtPathWindowsMSVCx64.txt
```

Esto es normal: CMake habrá creado un archivo vacío `CMakeQtPathWindowsMSVCx64.txt` en la raíz del proyecto (un nivel arriba de `build/`). Ese archivo **no se versiona** (está en `.gitignore`) porque la ruta de Qt es distinta en cada equipo.

Edita ese archivo y escribe en la primera línea la ruta a tu instalación de Qt, por ejemplo:

```
C:/Qt/5.15.2/msvc2019_64
```

Vuelve a ejecutar cmake (desde `build/`):

```bat
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

Los binarios se generan en:

```
Binaries/Windows/Release/MSVC/x64/
```

## Notas

- Si usas **MinGW** en lugar de MSVC, el nombre del archivo de ruta cambia a `CMakeQtPathWindowsGNUx64.txt` (o `x86` según arquitectura), y el generador de cmake debe ser `-G "MinGW Makefiles"`.
- Para build de depuración usa `-DCMAKE_BUILD_TYPE=Debug` y `--config Debug`.
- Si CMake indica que no encuentra **Qt5Multimedia**, revisa que el componente "Qt Multimedia" esté instalado para tu kit desde el Qt Maintenance Tool, y que la ruta en `CMakeQtPath*.txt` apunte a esa instalación (carpeta que contiene `lib/cmake/Qt5`).
