---
name: reset-board
description: Deja la placa Sofia (bread-compact-esp32) como para un usuario nuevo borrando solo su NVS (Wi-Fi, emparejamientos BLE, secreto del QR, identificador), sin tocar el firmware, y verifica el arranque. Usar cuando el usuario pida "resetear/borrar los datos de la placa", "dejarla como nueva" o "volver a vincular".
---

# Dejar la placa lista para un usuario nuevo

Borra **solo** la partición NVS. El firmware queda grabado. No compila ni graba nada.

## Qué se borra

- Redes Wi-Fi guardadas.
- Emparejamientos Bluetooth.
- Secreto del QR (`sofia_link`): se genera uno nuevo, así que el QR cambia.
- Identificador de la placa (UUID): xiaozhi.me pedirá el código de activación otra vez.

## Pasos

1. **Liberar el puerto serie.** Si hay una captura del log en segundo plano usando COM3, detenerla
   antes (TaskStop). Con el puerto ocupado esptool falla.

2. **Confirmar el puerto** (normalmente `COM3`, "USB-SERIAL CH340"):

   ```powershell
   [System.IO.Ports.SerialPort]::GetPortNames()
   ```

3. **Confirmar la dirección de la NVS** en la tabla de particiones (en `partitions/v2/4m.csv` es
   `nvs, data, nvs, 0x9000, 0x4000`). Si la placa usa otra tabla, usar su offset y tamaño.

4. **Borrar la NVS:**

   ```powershell
   python -m esptool --chip esp32 -p COM3 -b 460800 erase-region 0x9000 0x4000
   ```

   Debe decir `Flash memory region erased successfully`. Si `esptool` no está instalado:
   `python -m pip install esptool`.

5. **Reiniciar y verificar el arranque** (pulso en RTS, leer unos 12 s):

   ```powershell
   $p = New-Object System.IO.Ports.SerialPort 'COM3',115200; $p.Open(); $p.DtrEnable=$false; $p.RtsEnable=$true; Start-Sleep -Milliseconds 150; $p.RtsEnable=$false
   $sw=[Diagnostics.Stopwatch]::StartNew(); $sb=New-Object Text.StringBuilder
   while($sw.ElapsedMilliseconds -lt 12000){ [void]$sb.Append($p.ReadExisting()); Start-Sleep -Milliseconds 100 }; $p.Close()
   $t=$sb.ToString() -replace "`e\[[0-9;]*m",""
   ($t -split "`n" | Where-Object { $_ -match 'E \(|MusicBridge|StateMachine|abort|rst:|Board:|display on|Failed' }) -join "`n"
   ```

   Un arranque correcto muestra:
   - `Board: UUID=...` (nuevo)
   - `Turning display on` (la OLED respondió)
   - `State: starting -> wifi_configuring`
   - `MusicBridge: Generated a new pairing secret`
   - `MusicBridge: Waiting for Wi-Fi credentials from the phone app (Sofia-XXXX)`

   Si aparece `Failed to initialize display` o un `abort()`, informarlo; no reintentar a ciegas.

## Qué decirle al usuario

- La placa quedó sin datos y muestra un **QR nuevo**.
- En el teléfono: **eliminar "Sofia-XXXX"** de los dispositivos Bluetooth (si no, las claves viejas
  cortan la conexión con `Disconnect reason=0x23d`).
- En la app Sofia: escanear el QR nuevo y enviar la contraseña del Wi-Fi.
- xiaozhi.me pedirá el **código de activación** otra vez (UUID nuevo).

## Límites

- No borrar toda la flash (`erase-flash`): eso elimina también el firmware.
- No compilar ni grabar firmware como parte de esta skill.
