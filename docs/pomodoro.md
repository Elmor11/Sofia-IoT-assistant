# Pomodoro de estudio

Temporizador pomodoro controlado por voz, con su propia pantalla en la OLED y un historial de las
horas de estudio de la última semana. Se activa con `CONFIG_POMODORO_TIMER` (incluida en la
variante `bread-compact-esp32`); la placa registra la herramienta en `InitializeTools()`.

Código: [main/pomodoro/](../main/pomodoro/) (lógica, herramienta MCP, historial y melodías) y
[main/display/oled_pomodoro.cc](../main/display/oled_pomodoro.cc) (arco de progreso).

## Uso por voz

Una sola herramienta MCP, `self.pomodoro`, para ahorrar memoria en la lista de herramientas:

| `action` | Qué hace |
| --- | --- |
| `start` | Empieza (o reinicia) un ciclo. Opcionales: `focus_minutes` (25), `break_minutes` (5), `long_break_minutes` (15), `rounds` (4). |
| `pause` / `resume` | Pausa o continúa la fase actual. |
| `stop` | Detiene el pomodoro y guarda el tiempo estudiado. |
| `status` | Fase, ronda, tiempo transcurrido y restante, minutos de hoy, de los últimos 7 días (`last_7_days_min`, índice 0 = hoy), de la semana y en total. |

Ejemplos: "empieza un pomodoro", "pomodoro de 50 minutos con 10 de descanso", "pausa el
pomodoro", "¿cuánto me falta?", "¿cuánto estudié esta semana?".

Texto sugerido para el rol del agente en xiaozhi.me:

> Tienes un temporizador pomodoro en la pantalla: usa self.pomodoro con action start, pause,
> resume, stop o status. Da los tiempos en horas y minutos.

## Ciclo

Estudio → descanso corto → estudio … y, tras `rounds` rondas de estudio, un descanso largo; al
terminarlo el pomodoro se detiene. En cada cambio de fase:

- suena una melodía corta (`pomodoro_break.ogg` al pasar a descanso, `pomodoro_focus.ogg` al
  volver a estudiar), salvo si Sofia está hablando;
- aparece un aviso en pantalla ("A descansar!", "A estudiar!", "Pomodoro completo!").

Las melodías son campanas sintetizadas (Ogg Opus, 16 kHz, unos 5 KB cada una) y se incrustan solo
cuando la opción está activa.

## Pantalla (128x64)

```
        ╭───────────╮
     ╭──╯  (ojos +   ╰──╮     arco: se rellena de izquierda a derecha
    ╱      escena)       ╲    con el tiempo transcurrido de la fase
   │                      │
 2/4       12:34      Foco    ronda | tiempo transcurrido | fase (Foco, Relax, Pausa)
```

Dentro del arco los ojos representan la fase: en el estudio **leen** (recorren las líneas y pasan
la página) y **martillan** un clavo; en el descanso **hacen pesas** y **tararean**. Cuando Sofia
habla, su texto ocupa la pantalla y el arco vuelve al terminar. El pomodoro tiene prioridad sobre
el reproductor de música. En pantallas de 128x32 el arco va a la derecha de los ojos.

## Historial (NVS)

Clave persistente `pomodoro` / `history`: un blob empaquetado de 30 bytes, versión 1.

| Campo | Tipo | Contenido |
| --- | --- | --- |
| `version` | `uint8` | 1 |
| `day` | `uint32` | día local (días desde 1970) del índice 0 |
| `minutes[7]` | `uint16` | minutos de estudio por día, `[0]` = `day` |
| `sessions[7]` | `uint8` | rondas de estudio completadas por día |
| `total_minutes` | `uint32` | total histórico |

- Solo cuenta el tiempo de estudio, no los descansos.
- Se guarda cada 2 minutos de estudio y al pausar, detener o terminar una fase; un corte de luz
  pierde como mucho esos 2 minutos (y los segundos sueltos por debajo de un minuto).
- La ventana de 7 días avanza con la hora del servidor (que ya incluye la zona horaria). Sin hora
  válida, el tiempo se suma al último día conocido.
- Si cambia el formato hay que subir `kHistoryVersion`; un blob con otra versión se descarta.
- `reset-board` (borrado de toda la NVS) borra el historial; el borrado con BOOT no.

## Pruebas pendientes en hardware

- Ciclo completo con duraciones cortas (`focus_minutes` 1, `break_minutes` 1, `rounds` 2).
- Que el arco y los textos de abajo no se toquen, y que el libro y el martillo se reconozcan.
- Que la melodía suene en reposo y no durante una respuesta.
- Que el historial sobreviva a un reinicio y avance de día.
