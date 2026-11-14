# Historial de versiones de Sofia

Sofia usa su propia numeración ([SemVer](https://semver.org/lang/es/)) con etiquetas de git
`sofia-vX.Y.Z`. Es independiente de `PROJECT_VER` en `CMakeLists.txt`, que sigue siendo la versión
de XiaoZhi en la que se basa (el servidor la usa para las actualizaciones OTA; no se cambia).

## [0.4.0] - 2026-11-13

Basada en XiaoZhi 2.5.3. Placa: `bread-compact-esp32` (128x64).

### Añadido
- **Pomodoro** (`CONFIG_POMODORO_TIMER`): temporizador de estudio por voz con la herramienta
  `self.pomodoro` (start, pause, resume, stop, status). Arco de progreso semicircular en la OLED
  con el tiempo transcurrido debajo, melodía corta en cada cambio de fase e historial de 7 días
  guardado en NVS (`pomodoro/history`, 30 bytes). Ver [docs/pomodoro.md](docs/pomodoro.md).
- **Escenas de los ojos**: durante el pomodoro leen un libro y martillan (estudio) o hacen pesas y
  tararean (descanso); con la música sonando bailan a 120 BPM.
- **Vida en reposo**: tras 40 s quieta imagina (burbujas de pensamiento), juega con una pelota o
  tararea; a los 2 min le da sueño, cabecea y se duerme respirando con "Z" flotando.
- **Animaciones más vivas**: movimiento con resorte, aplastar y estirar al parpadear, parpadeo
  doble, inclinación curiosa y respiración sutil.
- **Borrar la conexión con BOOT**: mantener pulsado 5 s borra la red Wi-Fi, el secreto del QR y
  las claves Bluetooth y reinicia en modo de vinculación (conserva la activación).
- **Enlace de música por la red local** (TCP 8765 / UDP 8766 con autenticación HMAC): sustituye a
  BLE para la música una vez conectada al Wi-Fi. Ver [docs/music-bridge.md](docs/music-bridge.md).
- Opción `CONFIG_MCP_TOOLS_LIST_PAGE_SIZE` para paginar la lista de herramientas MCP en placas
  sin PSRAM (esta placa usa 2000 bytes).

### Cambiado
- Wi-Fi en reposo con ahorro moderado (`MIN_MODEM`) en vez de máximo: el router cortaba la
  conexión con el servidor tras unos minutos quieta.
- Agregación de paquetes al enviar (`AMPDU_TX`) activada de nuevo; la de recibir sigue apagada.
- BLE solo se usa para la vinculación por QR y se libera al conectar al Wi-Fi.

### Corregido
- "Timeout waiting for response" tras un rato en reposo: reconecta MQTT y reintenta el saludo con
  el servidor; si falla dos veces seguidas se reinicia sola.
- El QR quedaba en pantalla tras conectar; la OLED no respondía tras un reinicio por software.
- Falta de memoria al enviar la lista de herramientas al servidor (descripciones más cortas,
  páginas más pequeñas y una sola herramienta de pomodoro).

## [0.3.0] - 2026-11-09 (`1a615b9`, "mejoras")
- Ajustes de la vinculación por QR, de la pantalla y de la documentación.

## [0.2.0] - 2026-11-06 (`97f5f89`)
- Puente de música por BLE con la app Android y configuración Wi-Fi por QR.

## [0.1.0] - 2026-11-06 (`1f925e4`)
- Ojos reactivos en la OLED, configuración Wi-Fi en dos fases y README del proyecto.
