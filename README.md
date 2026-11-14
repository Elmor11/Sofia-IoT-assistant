# Sofia IoT Assistant

Asistente de voz con ojos animados para un **ESP32 estándar** (sin PSRAM), basado en el firmware
abierto [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32) (licencia MIT). Usa el servicio de
voz oficial de [xiaozhi.me](https://xiaozhi.me): la placa captura el audio, el servidor hace el
reconocimiento de voz, el modelo de IA y la voz, y la placa reproduce la respuesta.

**Versión actual: Sofia 0.4.0** (basada en XiaoZhi 2.5.3). Historial en
[CHANGELOG.md](CHANGELOG.md); cada versión tiene su etiqueta de git `sofia-vX.Y.Z`.

## Cambios realizados

- **Ojos reactivos** en la pantalla OLED (`CONFIG_OLED_REACTIVE_EYES`):
  - Ocupan la pantalla completa, centrados y grandes, y cambian con el estado de ánimo que envía
    el servidor (feliz, triste, sorprendido, pensando, enfadado, dormido, etc.).
  - Parpadean, miran a los lados y se mueven solos cuando no pasa nada.
  - No se muestra texto de "escuchando" ni de "respondiendo", ni la conversación.
  - Cuando hace falta texto (alertas, avisos, configuración Wi‑Fi, código), los ojos se encogen
    con una animación hacia la izquierda y aparece el texto. Al terminar, vuelven a crecer.
  - Se mueven con "resorte" (se pasan un poco y vuelven), se aplastan al parpadear, a veces
    parpadean dos veces o ladean la mirada, y respiran en reposo.
  - **En reposo** (40 s quieta): imagina con burbujas de pensamiento, juega con una pelota o
    tararea. A los **2 minutos** le da sueño, cabecea y se duerme respirando con "Z" flotando;
    al hablarle abre los ojos poco a poco.
  - **Escenas**: con la música sonando baila (120 BPM); durante el pomodoro lee un libro y martilla
    (estudio) o hace pesas y tararea (descanso).
- **Pomodoro de estudio** (`CONFIG_POMODORO_TIMER`): por voz ("empieza un pomodoro"), con un arco
  de progreso semicircular, melodía en cada cambio de fase e historial de 7 días.
  Ver [docs/pomodoro.md](docs/pomodoro.md).
- **Botón BOOT mantenido 5 s**: borra la red Wi‑Fi y la vinculación con el teléfono y reinicia
  mostrando un QR nuevo (conserva la activación de xiaozhi.me).
- **Red más estable**: si el servidor no responde tras un rato en reposo, reconecta y reintenta;
  si falla dos veces seguidas, se reinicia sola. En reposo el Wi‑Fi usa un ahorro moderado.
- **Configuración Wi‑Fi en dos fases**:
  1. *Paso 1/2*: te pide conectarte a la red Wi‑Fi (hotspot) de la placa.
  2. *Paso 2/2*: cuando un dispositivo se une a esa red, muestra la URL de la página de
     configuración.
- **Puente de música con el teléfono** (`CONFIG_MUSIC_BRIDGE_BLE`): las órdenes de voz (poner,
  pausar, siguiente, volumen) llegan a una app Android, y la pantalla muestra un reproductor
  (título, artista, barra de progreso) mientras suena algo. BLE solo se usa para vincular; la
  música va por la **red local** (TCP/UDP con autenticación HMAC) porque sin PSRAM no caben BLE,
  Wi‑Fi, TLS y audio a la vez. Protocolo completo en [docs/music-bridge.md](docs/music-bridge.md).
- **Configuración Wi‑Fi con código QR** (pantalla 128x64): la placa muestra un QR que la app escanea
  para enviarle la red por BLE con cifrado AES‑128‑GCM. Si la pantalla no puede mostrar el QR
  (128x32), se usa el hotspot de las dos fases.
- **Validación de longitud** de las tramas binarias v2 y v3 del protocolo WebSocket, para descartar
  paquetes mal formados.

## Hardware probado

Placa `bread-compact-esp32` (ESP32 clásico, 4 MB de flash, sin PSRAM):

| Componente | Detalle |
| --- | --- |
| Micrófono I2S | WS = GPIO25, SCK = GPIO26, DIN = GPIO32 |
| Altavoz I2S (amplificador) | DOUT = GPIO33, BCLK = GPIO14, LRCK = GPIO27 |
| Pantalla OLED SSD1306 (I2C) | SDA = GPIO4, SCL = GPIO15, 128x64 (o 128x32) |
| Botón BOOT | GPIO0 |

Los pines salen de [config.h](main/boards/bread-compact-esp32/config.h) y de la
[documentación de la placa](main/boards/bread-compact-esp32/README.md). Compruébalos contra tu
cableado antes de grabar.

> Sin PSRAM no hay palabra de activación ni cancelación de eco. El asistente funciona con
> **pulsar para hablar**: pulsa el botón BOOT (GPIO0) para empezar a hablar.

## Requisitos

- Docker (opción más sencilla en Windows), o ESP-IDF **v6.0.1 o superior** (recomendado la rama
  `release/v6.1`). ESP-IDF 5.x no está soportado.
- Una cuenta en [xiaozhi.me](https://xiaozhi.me) y un agente creado en su consola.
- Una herramienta para grabar el firmware: `esptool`, `idf.py flash` o *Flash Download Tools* de
  Espressif.

## Compilar

### Opción rápida (recomendada en Windows): `scripts/build-fast.sh`

Con Docker Desktop abierto, desde la carpeta del proyecto en Git Bash:

```sh
bash scripts/build-fast.sh                 # placa bread-compact-esp32
bash scripts/build-fast.sh <placa> <salida> # opcional: otra placa / otra carpeta de salida
```

El script copia el código a un volumen de Docker (`sofia-work`) y usa ccache (volumen
`sofia-ccache`), así se evita la lentitud del disco de Windows montado en Docker. Al terminar
imprime `BUILD_OK` y deja los binarios en `~/Downloads/xiaozhi-build-out/`:

- `merged-binary.bin`: imagen completa, se graba en `0x0`.
- `xiaozhi.bin`: solo la aplicación (para OTA).

Tiempos medidos en Windows: la primera vez unos 14 minutos (descarga la imagen
`espressif/idf:release-v6.1`, los componentes y compila ~2000 archivos); las siguientes
reutilizan el volumen y la caché y tardan pocos minutos.

### Otras opciones

Con Docker montando la carpeta directamente (más lento en Windows):

```sh
docker run --rm -v "$(pwd):/project" -w /project espressif/idf:release-v6.1 \
  bash -c "python3 scripts/build.py bread-compact-esp32 --name bread-compact-esp32"
```

Con ESP-IDF instalado:

```sh
source /ruta/a/esp-idf/export.sh
python3 scripts/build.py bread-compact-esp32 --name bread-compact-esp32
```

Para la pantalla de 128x32 usa `--name bread-compact-esp32-128x32`. Para ver todas las placas:
`python3 scripts/build.py --list-boards`.

Con estas opciones la primera compilación tarda bastante más (unos 40 minutos en pruebas con
Docker montando la carpeta en Windows). El resultado queda en `build/`; el archivo que se graba
entero es `build/merged-binary.bin`.

## Grabar la placa

1. Conecta la placa por USB. Debe aparecer como un puerto COM (con un CH340 aparece como
   "USB-SERIAL CH340"). Si no aparece, prueba otro cable (algunos solo cargan) o instala el
   driver CH340. En PowerShell puedes ver el puerto con
   `[System.IO.Ports.SerialPort]::GetPortNames()`.
2. **Recomendado: `esptool`** (se instala una vez con `python -m pip install esptool`). Cambia
   `COM3` por tu puerto:

   ```sh
   python -m esptool --chip esp32 -p COM3 -b 460800 write-flash 0x0 ~/Downloads/xiaozhi-build-out/merged-binary.bin
   ```

   Tarda unos 40 segundos. Debe terminar con `Hash of data verified.` y reinicia la placa sola.
   Si se queda en `Connecting...`, mantén pulsado BOOT hasta que empiece a escribir.
3. Alternativa, *Flash Download Tools* de Espressif: al abrirla elige ChipType **ESP32**,
   WorkMode **Develop**, LoadMode **UART**. Marca la primera fila, carga `merged-binary.bin` en la
   dirección `0x0`, deja SPI 40MHz / DIO y `DoNotChgBin` marcado, elige el puerto, velocidad
   460800 y pulsa **START**. No pulses ERASE: borra la flash (incluido el Wi‑Fi guardado y la
   activación) y, si la escritura no llega a completarse, la placa queda sin firmware.

### Comprobar que arrancó

Abre el monitor serie a 115200 baudios (`idf.py monitor`, PuTTY o el monitor de Arduino) y
reinicia la placa. Un arranque correcto muestra `SKU=bread-compact-esp32`, la inicialización de
la OLED y, sin red guardada, `State: starting -> wifi_configuring`.

Si ves en bucle `invalid header: 0xffffffff`, la flash está vacía (se borró pero no se escribió
el firmware). La placa no está dañada: vuelve a grabar `merged-binary.bin` con `esptool`.

## Primer uso

1. **Wi‑Fi.** Si la placa no tiene una red guardada, entra en modo de configuración. Con la
   pantalla 128x64 y la app complementaria, muestra un QR: escanéalo con la app y envía la red
   (ver [docs/music-bridge.md](docs/music-bridge.md)). Si no, usa el hotspot: *Paso 1/2* te indica la red Wi‑Fi de la placa a la que conectarte desde el móvil o el PC;
   al conectarte pasa a *Paso 2/2* con la URL de configuración (normalmente `http://192.168.4.1`).
   Elige tu red y su contraseña. Si la página dice "Failed to fetch" justo al conectar, es normal:
   la red de configuración se apaga cuando la placa pasa a conectarse a tu Wi‑Fi.
2. **Activación.** En el primer arranque con red, la placa se registra con xiaozhi.me. Si el
   servidor pide un código de verificación, aparece en la pantalla y el log; introdúcelo en la
   consola de xiaozhi.me para vincular el dispositivo a tu agente.
3. **Hablar.** Pulsa BOOT y habla. Los ojos reaccionan al estado de ánimo de la respuesta.

Para cambiar de red o de teléfono, **mantén BOOT pulsado 5 s**: aparece "Borrando conexion...",
la placa se reinicia y muestra un QR nuevo. Elimina antes "Sofia-XXXX" del Bluetooth del teléfono.
Para dejarla totalmente de fábrica (identificador y activación nuevos, sin historial del pomodoro)
hay que borrar la NVS por USB: `python -m esptool --chip esp32 -p COM3 erase-region 0x9000 0x4000`.

## Personalizar el agente (en xiaozhi.me, sin tocar el firmware)

- **Modelo de IA (LLM), rol y personalidad, voz (TTS):** Agents → tu agente → Configure.
- **Servicios oficiales** (música, clima, base de conocimiento): pestaña Extensions.
- **Herramientas propias:** Custom Services → MCP Endpoint.

La voz y el modelo los decide el servidor. El firmware solo reproduce el audio recibido.

## Herramientas por voz (MCP del dispositivo)

| Herramienta | Para qué |
| --- | --- |
| `self.music.play`, `pause`, `resume`, `next`, `previous`, `volume_up`, `volume_down`, `get_now_playing` | Música de Spotify en el teléfono |
| `self.pomodoro` (`action`: start, pause, resume, stop, status) | Pomodoro y horas de estudio |
| `self.lamp.*`, `self.audio_speaker.set_volume`, `self.get_device_status`, ... | Herramientas base de XiaoZhi |

Sin PSRAM la memoria es justa: mantén las descripciones cortas y agrupa acciones en una sola
herramienta. La lista se envía al servidor en páginas de `CONFIG_MCP_TOOLS_LIST_PAGE_SIZE` bytes
(2000 en esta placa).

## Cómo funcionan los ojos

El código está en [main/display/oled_eyes.cc](main/display/oled_eyes.cc) y se activa con la opción
`CONFIG_OLED_REACTIVE_EYES` (ya incluida en las dos variantes de `bread-compact-esp32`). La
integración con el layout y la lógica de cuándo mostrar texto están en
[main/display/oled_display.cc](main/display/oled_display.cc).

- Cada nombre de emoción del servidor (`happy`, `sad`, `thinking`...) se traduce a una pose de los
  ojos (tamaño, mirada, párpados, pupila, lágrimas, corazones).
- Los nombres que no son estados de ánimo (iconos de alerta) muestran el icono y el texto en lugar
  de los ojos.
- Las conversaciones solo aparecen si el contenido parece código.
- Las actividades de reposo solo ocurren con los ojos solos y en estado neutro; cualquier emoción
  del servidor las interrumpe. Las escenas (`OledEyes::SetScene`) las decide `OledDisplay` según
  el pomodoro y la música; mientras hay texto en pantalla no hay escena.
- El baile no sigue el ritmo real: la música suena en el teléfono y la placa no recibe su audio.

## Depuración

Para leer el log por el puerto serie (115200 baudios):

```sh
python -m serial.tools.miniterm COM11 115200
```

(cambia `COM11` por tu puerto). El log muestra la conexión Wi‑Fi, la activación y los cambios de
estado, y sirve para diagnosticar problemas de red o de pantalla. Para comprobar qué versión hay
grabada, busca la línea `Compile time` al arrancar.

## Estado y limitaciones

- Probado en una placa real con ESP32, micrófono I2S, amplificador I2S y OLED SSD1306 128x64. La
  variante 128x32 compila pero no está probada en hardware.
- Compilar correctamente no equivale a validar en hardware. Los cambios de audio y red necesitan
  pruebas físicas.
- Sin PSRAM: no hay palabra de activación ni AEC, y la memoria libre es limitada (unos 85-90 KB
  en reposo, mínimo ~50 KB al conectar). Revisa `free sram` y `minimal sram` en el log tras
  cada cambio; un `abort()` con `operator new` en el backtrace es falta de memoria.
- Vinculación por QR, enlace de música por red local, reconexión y reinicio automático probados
  en hardware con la app. Pendiente de probar en hardware en 0.4.0: pomodoro completo, escenas
  y baile en la pantalla, y el borrado con BOOT.
- Con algunos routers TP-Link en 2,4 GHz y 40 MHz la placa puede ser expulsada de la red
  (`reason: 3`); se reconecta sola en segundos. Un canal fijo de 20 MHz en el router lo evita.

## Ideas futuras

- Que la app envíe el BPM de la canción para que el baile siga el ritmo real.
- App Android complementaria que implemente el protocolo de [docs/music-bridge.md](docs/music-bridge.md)
  (el firmware ya está; la app aún no está en este repositorio).
- Herramientas del dispositivo (MCP local) para controlar brillo, volumen o hardware extra.

## Créditos y licencia

Basado en [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32), © 2025 Shenzhen Xinzhi Future
Technology Co., Ltd. Distribuido bajo la licencia MIT; consulta [LICENSE](LICENSE).
