# Music bridge — protocolo entre la placa y la app del teléfono

La app ejecuta en el teléfono las órdenes de música (por voz) y le cuenta a la placa qué está
sonando, para que dibuje el reproductor en la pantalla OLED.

> **Canal actual: red local (Wi‑Fi), no BLE.** En el ESP32 sin PSRAM, tener el BLE encendido junto
> con Wi‑Fi, TLS y audio deja la placa sin RAM: falla la activación y se reinicia. Por eso:
>
> - El **BLE solo se usa para configurar el Wi‑Fi con el QR** (sección "Configurar el Wi‑Fi con el
>   QR"). Cuando el Wi‑Fi conecta, la placa libera la memoria del Bluetooth y el BLE queda apagado
>   hasta el siguiente reinicio.
> - La música va por la **red local** (sección "Conexión por la red local"), con los mismos JSON
>   `Cmd` y `State` descritos abajo. Las secciones de roles, conexión y UUID BLE quedan como
>   referencia, pero en esta placa la placa ya no anuncia el servicio de música por BLE.

## Conexión por la red local

El teléfono tiene que estar en la **misma red Wi‑Fi** que la placa.

### 1. Descubrir la placa (UDP 8766)

La app envía el texto `SOFIA?` por UDP al puerto **8766**, a la dirección de broadcast de la red
(por ejemplo `192.168.0.255`) o a `255.255.255.255`. Cada placa responde al remitente:

```json
{"n":"Sofia-D3DE","p":8765}
```

- `n`: nombre de la placa. Los 4 últimos dígitos coinciden con los del QR (`SF D3DE ...`): así la
  app sabe cuál es la suya.
- `p`: puerto TCP.
- **Reintentar**: la placa ahorra energía en el Wi‑Fi y puede perderse un broadcast suelto. Enviar
  `SOFIA?` cada 500 ms durante unos 5 s (en pruebas respondió al 3.er intento). La IP de la respuesta
  se puede guardar y usar directamente la próxima vez (un envío directo a la IP responde a la
  primera).
- En Android hace falta un `WifiManager.MulticastLock` para recibir respuestas a broadcast en
  algunos teléfonos.

### 2. Conectarse y autenticarse (TCP 8765)

Mensajes JSON de una línea, terminados en `\n`, en ambos sentidos.

1. La app abre TCP a `IP:8765`. La placa saluda:

   ```json
   {"hello":"Sofia-D3DE","nonce":"9649ffb87447769b4973e7d1cc100970"}
   ```

2. La app responde en menos de **10 s** con:

   ```json
   {"auth":"<64 dígitos hex>"}
   ```

   donde el valor es `HMAC-SHA256(clave, nonce)` en hexadecimal:
   - `clave`: los **10 bytes** del secreto del QR (los 20 dígitos hex convertidos a bytes, igual que
     para el cifrado del Wi‑Fi).
   - `nonce`: el texto del campo `nonce` tal cual (32 caracteres ASCII), no sus bytes.

3. La placa contesta `{"auth":"ok"}` y la conexión queda lista. Si la clave no coincide contesta
   `{"auth":"bad"}` y cierra. Tras **8 intentos fallidos** responde `{"auth":"locked"}` a cualquier
   conexión hasta que se reinicie.

Ejemplo en Kotlin:

```kotlin
val key = SecretKeySpec(secretHex.chunked(2).map { it.toInt(16).toByte() }.toByteArray(), "HmacSHA256")
val mac = Mac.getInstance("HmacSHA256").apply { init(key) }.doFinal(nonce.toByteArray(Charsets.US_ASCII))
val auth = mac.joinToString("") { "%02x".format(it) }
```

### 3. Mensajes

- Placa → app: órdenes con el mismo formato que `Cmd` (`{"c":"play","q":"..."}`, etc.), una por
  línea.
- App → placa: estado con el mismo formato que `State` (`{"t":"...","a":"...","p":...,"d":...,"s":1}`),
  una por línea, **240 bytes como máximo** (una línea más larga cierra la conexión).

### Reglas

- **Un teléfono a la vez**: una conexión nueva reemplaza a la anterior.
- La placa activa *TCP keepalive* (detecta en ~1 min un teléfono que se fue sin cerrar). Si la
  conexión se cae, la app debe volver a conectarse (y, si la IP cambió, volver a descubrir).
- Al cerrarse la conexión, la placa oculta el reproductor.
- El canal no va cifrado (solo autenticado). Úsalo solo en una red de confianza.

### Probar sin la app

Descubrimiento desde PowerShell en un PC de la misma red:

```powershell
$u = New-Object System.Net.Sockets.UdpClient 0; $u.EnableBroadcast = $true; $u.Client.ReceiveTimeout = 600
$b = [Text.Encoding]::ASCII.GetBytes('SOFIA?')
1..10 | % { [void]$u.Send($b, $b.Length, '192.168.0.255', 8766); try { $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0); [Text.Encoding]::ASCII.GetString($u.Receive([ref]$ep)); break } catch {} }
```

El firewall de Windows puede bloquear la respuesta para `python.exe` si no tiene permiso en redes
privadas.

---

Lo que sigue describe el canal BLE original (y el QR, que sigue usándolo).

- Firmware: [main/music/music_bridge.cc](../main/music/music_bridge.cc), se activa con
  `CONFIG_MUSIC_BRIDGE_BLE=y` (junto con `CONFIG_BT_NIMBLE_ENABLED=y`; ya está en la variante
  `bread-compact-esp32`).
- Pantalla: [main/display/oled_music.cc](../main/display/oled_music.cc).

## Roles

| Dispositivo | Rol BLE |
| --- | --- |
| Placa (ESP32) | Periférico y servidor GATT. Anuncia el nombre `Sofia-Music`. |
| Teléfono (app) | Central y cliente GATT. Busca la placa, se conecta y se empareja. |

## Conexión

1. La app busca el nombre `Sofia-Music` (o el UUID del servicio, que va en la respuesta de escaneo).
2. Se conecta y **pide un MTU alto** (`requestMtu(247)` en Android). Hace falta: los mensajes que la
   placa envía por notificación no se parten, y si no caben en `MTU - 3` bytes se descartan.
3. La placa solicita seguridad al conectar. El emparejamiento es *Just Works* con *bonding*; el
   enlace queda cifrado pero no autenticado (cualquiera cerca podría emparejarse mientras la placa
   anuncia). Android lo hace solo al escribir en la característica protegida.
4. La app descubre el servicio y **activa las notificaciones** de la característica `Cmd`. La placa
   solo envía órdenes cuando el enlace está cifrado y las notificaciones están activas.
5. Al desconectarse, la placa vuelve a anunciar sola y borra el reproductor de la pantalla.

## UUID

UUID propuestos (cámbialos en el firmware y en la app a la vez si hace falta):

| Qué | UUID |
| --- | --- |
| Servicio | `6e7a0001-3b1d-4f5e-9a2c-5b8d1c0f7a10` |
| `Cmd` (placa → teléfono, *Notify*) | `6e7a0002-3b1d-4f5e-9a2c-5b8d1c0f7a10` |
| `State` (teléfono → placa, *Write* / *Write without response*, requiere cifrado) | `6e7a0003-3b1d-4f5e-9a2c-5b8d1c0f7a10` |

## Cmd: órdenes de la placa (JSON UTF‑8, una línea)

```json
{"c":"play","q":"bohemian rhapsody queen"}
{"c":"pause"}
{"c":"resume"}
{"c":"next"}
{"c":"prev"}
{"c":"vol","d":1}
{"c":"vol","d":-1}
```

- `c`: orden. `q`: texto de búsqueda para `play` (máx. 100 bytes, en el idioma que dijo el usuario).
- `k` (opcional, solo en `play`): qué busca el usuario: `track` (una canción), `artist`, `album` o
  `playlist`. Si no viene, no se sabe: la app decide (por ejemplo, canción y artista). Ejemplo:
  `{"c":"play","q":"tabata","k":"playlist"}`. Una app que no lo conozca puede ignorarlo.
- `d` en `vol`: `1` sube y `-1` baja el volumen del teléfono.
- Una orden por notificación. La app no responde por esta vía: el efecto se ve en el siguiente
  `State`.

## State: lo que suena en el teléfono (JSON UTF‑8)

```json
{"t":"Bohemian Rhapsody","a":"Queen","p":85000,"d":354000,"s":1}
```

| Campo | Significado |
| --- | --- |
| `t` | Título. Opcional. |
| `a` | Artista. Opcional. |
| `p` | Posición actual en milisegundos. |
| `d` | Duración total en milisegundos (`0` si se desconoce). |
| `s` | Estado: `1` reproduciendo, `0` en pausa, `2` detenido (oculta el reproductor). **Obligatorio.** |

Reglas para la app:

- Enviar **al cambiar de canción o de estado**, y tras un salto de posición. No hace falta enviar
  el progreso cada segundo: la placa avanza sola el contador mientras `s` es `1`. Reenviar `p` cada
  10–15 s corrige desvíos.
- Mensajes de **240 bytes como máximo**. La placa recorta título y artista a 40 caracteres y
  descarta lo que su fuente no puede dibujar (los acentos comunes se convierten a la letra base).
- Enviar `{"s":2}` cuando no suene nada.

## Herramientas de voz (MCP) que usan este canal

| Herramienta | Orden enviada |
| --- | --- |
| `self.music.play(query)` | `play` |
| `self.music.pause` / `resume` / `next` / `previous` | `pause` / `resume` / `next` / `prev` |
| `self.music.volume_up` / `volume_down` | `vol` con `d` = `1` / `-1` |
| `self.music.get_now_playing` | no envía nada; devuelve el último `State` |

Si el teléfono no está conectado, las herramientas devuelven un error y el asistente le pide al
usuario que abra la app.

## Configurar el Wi‑Fi con el QR

Si la placa no tiene una red Wi‑Fi guardada (o no consigue conectarse), en lugar del hotspot
muestra un **QR a pantalla completa** (solo en la pantalla de 128x64; con 128x32 sigue usando el
hotspot de antes). La app lo escanea, se conecta por BLE y le envía la red.

### Contenido del QR

26 caracteres, texto plano, ancho fijo:

```
SF  ABCD  0123456789ABCDEF0123
│   │     └─ secreto de 80 bits, 20 dígitos hex en mayúsculas
│   └─ últimos 4 dígitos hex del nombre BLE
└─ prefijo
```

- El nombre BLE de la placa es `Sofia-ABCD` (los mismos 4 dígitos). La app debe buscar ese nombre.
- El secreto se genera al azar la primera vez y **se guarda en la placa** (NVS, espacio
  `sofia_link`), así que el QR es el mismo en cada arranque.

### Envío de las credenciales (característica `Prov`, `...0004`)

1. La app se conecta y pide MTU alto. En este modo la placa **no pide emparejamiento**: los datos
   viajan cifrados por la propia app.
2. La app activa las notificaciones de `Prov` y escribe un único mensaje con este formato binario:

   ```
   nonce (12 bytes aleatorios) | texto cifrado | etiqueta (16 bytes)
   ```

   - Cifrado: **AES‑128‑GCM**.
   - Clave: los primeros 16 bytes de `SHA‑256("sofia-wifi-v1" + secreto)`, donde `secreto` son los
     10 bytes que salen de los 20 dígitos hex del QR.
   - AAD (datos asociados): los 10 bytes ASCII `SOFIA-WIFI`.
   - Texto sin cifrar (JSON UTF‑8): `{"s":"MiRed","p":"MiClave"}` (`s`: 1 a 32 bytes; `p`: hasta 63
     bytes, vacío o ausente para redes abiertas).
   - El mensaje completo no puede pasar de 200 bytes.
3. La placa responde por notificación en `Prov`:

   | Respuesta | Significado |
   | --- | --- |
   | `{"w":"saved"}` | Credenciales guardadas. La placa se reinicia en ~1,5 s y se conecta a esa red. |
   | `{"w":"bad_key"}` | No se pudo descifrar (secreto equivocado o mensaje alterado). |
   | `{"w":"bad_data"}` | El JSON no es válido o el SSID/clave no cumplen los límites. |
   | `{"w":"locked"}` | Demasiados intentos fallidos (8). Hay que reiniciar la placa. |

La placa **no prueba** la red antes de guardarla: tras `saved` reinicia y se conecta. Si la red o la
clave están mal, vuelve sola al QR.

### Emparejamiento para la música

Tras reiniciar y conectarse al Wi‑Fi, la app se conecta de nuevo a `Sofia-ABCD`. La primera
escritura en `State` provoca el emparejamiento (*Just Works*) y el sistema recuerda el enlace, así
que se hace una sola vez.

## Pantalla

Cuando `s` es `0` o `1`, los ojos se encogen a la izquierda y aparece el reproductor: título y
artista desplazándose, barra de progreso, `1:25 / 3:40` y los iconos de pausa y siguiente (en la
pantalla de 128x32 solo título y barra). Las alertas y avisos (por ejemplo la configuración Wi‑Fi)
tienen prioridad y ocultan el reproductor mientras estén visibles.

Mientras `s` es `1` (sonando) los ojos **bailan** a 120 BPM en su columna: rebotan en cada pulso,
se mecen cada compás y alternan el ojo grande. No es el ritmo real de la canción (la placa no
recibe el audio); si la app enviara el BPM se podría ajustar. Si hay un pomodoro en marcha, su
pantalla tiene prioridad sobre el reproductor.

## Borrar la vinculación desde la placa

Mantener el botón BOOT pulsado 5 s borra la red Wi‑Fi, el secreto del QR (`sofia_link`) y las
claves Bluetooth (`nimble_bond`), y reinicia la placa en modo de vinculación con un QR nuevo. La
app debe olvidar la placa vieja (secreto anterior) y el teléfono, quitar `Sofia-ABCD` de sus
dispositivos Bluetooth.

## Probar sin la app

Con una app BLE genérica (por ejemplo nRF Connect en Android):

1. Conéctate a `Sofia-Music` y empareja cuando lo pida.
2. Pide MTU 247 y activa las notificaciones de `Cmd`.
3. Escribe en `State` el texto `{"t":"Prueba","a":"Sofia","p":0,"d":200000,"s":1}`: el
   reproductor debe aparecer en la placa.
4. Di "pausa la música": en `Cmd` debe llegar `{"c":"pause"}`.

## Límites conocidos

- Sin PSRAM la memoria es justa con Wi‑Fi y BLE a la vez. Hay que comprobar la RAM libre en el log
  (`free sram`) tras compilar con esta función.
- Solo un teléfono conectado a la vez.
- El log de la placa muestra `MusicBridge: ...` con cada conexión, suscripción y orden enviada.
