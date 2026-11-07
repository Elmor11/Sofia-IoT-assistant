# Sofia IoT Assistant

Asistente de voz con ojos animados para un **ESP32 estándar** (sin PSRAM), basado en el firmware
abierto [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32) (licencia MIT). Usa el servicio de
voz oficial de [xiaozhi.me](https://xiaozhi.me): la placa captura el audio, el servidor hace el
reconocimiento de voz, el modelo de IA y la voz, y la placa reproduce la respuesta.

## Cambios realizados

- **Ojos reactivos** en la pantalla OLED (`CONFIG_OLED_REACTIVE_EYES`):
  - Ocupan la pantalla completa, centrados y grandes, y cambian con el estado de ánimo que envía
    el servidor (feliz, triste, sorprendido, pensando, enfadado, dormido, etc.).
  - Parpadean, miran a los lados y se mueven solos cuando no pasa nada.
  - No se muestra texto de "escuchando" ni de "respondiendo", ni la conversación.
  - Cuando hace falta texto (alertas, avisos, configuración Wi‑Fi, código), los ojos se encogen
    con una animación hacia la izquierda y aparece el texto. Al terminar, vuelven a crecer.
- **Configuración Wi‑Fi en dos fases**:
  1. *Paso 1/2*: te pide conectarte a la red Wi‑Fi (hotspot) de la placa.
  2. *Paso 2/2*: cuando un dispositivo se une a esa red, muestra la URL de la página de
     configuración.
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

Con Docker, desde la carpeta del proyecto:

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

La primera compilación descarga dependencias y tarda bastante (unos 40 minutos en pruebas con
Docker en Windows). El resultado queda en `build/`; el archivo que se graba entero es
`build/merged-binary.bin`.

## Grabar la placa

1. Conecta la placa por USB. Debe aparecer como un puerto COM (con un CH340 aparece como
   "USB-SERIAL CH340"). Si no aparece, prueba otro cable (algunos solo cargan) o instala el
   driver CH340.
2. Con *Flash Download Tools*: carga `merged-binary.bin` en la dirección `0x0`, elige el puerto,
   velocidad 460800 y pulsa **START**. No marques ERASE: borrar la flash elimina el Wi‑Fi guardado
   y la activación.
3. Con `esptool`:

   ```sh
   python -m esptool --chip esp32 -b 460800 write-flash 0x0 build/merged-binary.bin
   ```

4. Reinicia la placa.

## Primer uso

1. **Wi‑Fi.** Si la placa no tiene una red guardada, entra en modo de configuración:
   *Paso 1/2* te indica la red Wi‑Fi de la placa a la que conectarte desde el móvil o el PC;
   al conectarte pasa a *Paso 2/2* con la URL de configuración (normalmente `http://192.168.4.1`).
   Elige tu red y su contraseña. Si la página dice "Failed to fetch" justo al conectar, es normal:
   la red de configuración se apaga cuando la placa pasa a conectarse a tu Wi‑Fi.
2. **Activación.** En el primer arranque con red, la placa se registra con xiaozhi.me. Si el
   servidor pide un código de verificación, aparece en la pantalla y el log; introdúcelo en la
   consola de xiaozhi.me para vincular el dispositivo a tu agente.
3. **Hablar.** Pulsa BOOT y habla. Los ojos reaccionan al estado de ánimo de la respuesta.

## Personalizar el agente (en xiaozhi.me, sin tocar el firmware)

- **Modelo de IA (LLM), rol y personalidad, voz (TTS):** Agents → tu agente → Configure.
- **Servicios oficiales** (música, clima, base de conocimiento): pestaña Extensions.
- **Herramientas propias:** Custom Services → MCP Endpoint.

La voz y el modelo los decide el servidor. El firmware solo reproduce el audio recibido.

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
- Sin PSRAM: no hay palabra de activación ni AEC, y la memoria libre es limitada.

## Ideas futuras

- Más comportamientos de los ojos en reposo (bostezo, sueño, sobresalto).
- Reproductor de música en pantalla (título, barra de progreso y controles), con una app
  Android complementaria conectada por Bluetooth de bajo consumo.
- Herramientas del dispositivo (MCP local) para controlar brillo, volumen o hardware extra.

## Créditos y licencia

Basado en [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32), © 2025 Shenzhen Xinzhi Future
Technology Co., Ltd. Distribuido bajo la licencia MIT; consulta [LICENSE](LICENSE).
