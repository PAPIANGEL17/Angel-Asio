# AMR Angel ASIO

Driver ASIO propio para Windows (no es un VST): aparece en los ajustes de audio del DAW como
**"AMR Angel ASIO"**, igual que ASIO4ALL. Dentro lleva un "chip DSP" virtual para monitorear la voz
con efectos con la menor latencia posible, mientras el DAW graba.

## Cómo funciona

```
Micrófono -> WASAPI (exclusivo, periodo mínimo) -> hilo de audio (MMCSS Pro Audio)
                                                     |-> cadena DSP (HPF, EQ, compresor, reverb) -> audífonos  (monitoreo directo)
                                                     '-> bloques ASIO (32..2048 frames) -> DAW -> salida del DAW -> audífonos
```

- Un solo hilo de tiempo real, sin locks ni reservas de memoria en el camino de audio.
- Entrada y salida estéreo (2 in / 2 out), `ASIOSTInt32LSB`, 44.1 a 192 kHz.
- El monitoreo con efectos se mezcla en la salida **antes** de pasar por el DAW, así el cantante no
  escucha el retraso del buffer del DAW. Por defecto el DAW graba la señal limpia; con la opción
  "Grabar con efectos" graba EQ + compresor (la reverb nunca se graba).
- Si el modo exclusivo no está disponible, cae a WASAPI compartido de baja latencia (IAudioClient3).

> Límite físico: siempre hay un conversor analógico-digital (el de tu tarjeta o micrófono USB).
> El software no puede eliminarlo; esto reduce todo lo demás.

## Compilar en GitHub

1. Sube esta carpeta a un repositorio nuevo.
2. Pestaña **Actions** -> el workflow *Build AMR Angel ASIO* corre solo (o "Run workflow").
3. Descarga los artefactos `AMR-Angel-ASIO-x64` (DAWs de 64 bits, lo normal) y `AMR-Angel-ASIO-x86` (DAWs de 32 bits).

El workflow compila con MSVC (runtime estático, no necesita redistribuibles), ejecuta una prueba de humo
que carga el driver como un DAW, y también prueba el registro COM.

## Instalar

1. Descomprime el artefacto, clic derecho en `install.bat` -> **Ejecutar como administrador**.
2. En tu DAW: Ajustes de audio -> driver ASIO -> **AMR Angel ASIO**.
3. Botón "ASIO Control Panel" del DAW (o `panel.bat`) para elegir micrófono, salida, bloque y efectos.
4. Desinstalar: `uninstall.bat` como administrador.

Config y log en `%APPDATA%\AngelASIO\` (`config.ini`, `angelasio.log`).

## Afinar la latencia

- Bloque ASIO 64 o 32 frames y periodo WASAPI "Mínimo" con **exclusivo** activado.
- En exclusivo, el dispositivo queda bloqueado para otras apps mientras el DAW use el driver.
- Entrada y salida en dispositivos distintos tienen relojes distintos: puede haber un clic ocasional
  por deriva (se cuenta en "xruns de entrada" del panel). Lo ideal es usar entrada y salida del mismo dispositivo.
- Los plugins con lookahead o fase lineal en el DAW añaden su propia latencia; el DSP interno no añade ninguna.

## Probar sin audio (CI)

`ANGELASIO_VIRTUAL=1` hace que el driver use un reloj virtual en vez de WASAPI. Solo sirve para pruebas.

## Licencia y marcas

ASIO es marca registrada de Steinberg Media Technologies GmbH. Este proyecto declara por sí mismo la
interfaz COM mínima (`src/asio_defs.h`) y no incluye el SDK de Steinberg.
