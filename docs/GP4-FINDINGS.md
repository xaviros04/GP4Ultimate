# Hallazgos en el código de GP4.exe

*Notas técnicas de GP4 Ultimate (en español): qué hace GP4.exe y en qué direcciones se apoya el mod, con la instrucción que lo demuestra.*

Ejecutable analizado: `GP4.exe` sha256 `2046f390…4a926b12`, el mismo que esperan GP4 Ultimate y GP4PP. Herramientas: análisis `gp4re` de [decomp-grandprix4](https://github.com/leighrobertabbott/decomp-grandprix4) (12.838 funciones, mismos números que el repositorio) y desensamblado con capstone. Todo lo que sigue cita la instrucción que lo demuestra. Son direcciones fijas: el ejecutable no tiene relocalizaciones.

## Entrada (DirectInput)

| Hecho | Evidencia | Consecuencia para el mod |
|---|---|---|
| Solo importa `DirectInput8Create`, con `IID_IDirectInput8A` (ANSI) y versión 0x0800 | `0x505df3 push 0x5f6480` | El proxy `A` es el camino real (cubierto por las pruebas) |
| Enumera `DI8DEVCLASS_GAMECTRL` + `DIEDFL_ATTACHEDONLY` y **crea todos** los mandos | `0x505e45`, bucle `0x505f37` | Hay que ocultar los aparatos físicos (`hide_physical = 1`) |
| **Máximo 4 mandos** | callback `0x506390: cmp eax, 4` | Fusionarlos en uno evita quedarse sin plazas |
| Formato `c_dfDIJoystick` (80 bytes, 44 objetos) | `0x505fbf push 0x5f6264` | El formato que cubre la prueba de extremo a extremo |
| `SetCooperativeLevel(hwnd, EXCLUSIVE \| FOREGROUND)` (valor 5 = 0x1 \| 0x4) en cada mando | `0x505fdf push 5` | El virtual lo acepta; el volante real lo abre el mod |
| Solo **lee** los mandos nº 0 y nº 1 (`Poll` + `GetDeviceState(80)`) | `0x5064f0` (`[0xae69a4]`, `[0xae69a8]`) | Sin el mod, un tercer aparato (base + pedales + aro) no se leería |
| `DIPROP_CALIBRATIONMODE = RAW`: GP4 calibra los ejes por su cuenta | `0x5061e7 push 0xa` | Calibrar en el menú de GP4 con el mando virtual |
| **Solo lee 8 botones por mando** (`rgbButtons[0..7]`, empaquetados en un byte; "Fire1" es el botón 0) | `0x5065c2`–`0x5065dc` → `[0x7a979c]` | Hay que llevar los 8 botones útiles a los botones virtuales 0–7 con `[buttons]` explícito |
| Trato especial si el nombre contiene "Thrustmaster" | cadenas `0x66cfc8` / `0x66cfb8` | El nombre del virtual no lo activa |
| También importa la API de joystick de winmm (`joyGetPosEx`…) | `0x507990` rellena `0xb1bb80`/`0xb1bea8` al arrancar | **Código muerto**: nadie vuelve a leer esas tablas. No hace falta un proxy de winmm |

## Force feedback

| Hecho | Evidencia |
|---|---|
| Solo da FFB al mando **nº 0** (y al nº 1 si lo tiene). Exige la opción de FFB del menú activada (`[0x7a8e0c]`) | `0x505ff0`, `0x50624d` |
| Desactiva el autocentrado y consulta muelle, amortiguador y fricción, pero **solo crea una fuerza constante** | `0x506059`, `0x5068c0`, `0x506886` |
| Efecto: cartesiano, offsets de objeto, duración infinita, 1 eje por cada actuador FF (máximo 2) | `0x506750`, callback `0x5066e0` |
| Cada actualización: `magnitud = clamp(1,5 × valor, ±10000)`, relanzando el efecto (`DIEP_START`) | `0x5069d0` |
| `valor = −clamp(coche[+0x5E], ±10000)`, con el coche tomado de `[0x66d820]` | `0x51e467`, `0x51e319` |
| Fuerza a 0 si alguno de `[0x852d6a]`, `[0x88f63a]`, `[0x852d98]` no es cero | `0x51e405` |
| `+0x5E = (alineación + 2 términos de amortiguación por velocidad de giro) × FFB% (+0x173) / 100`, limitado a ±10000 | `0x41a2ec` / `0x41a463` (núcleo asm; elige entre los dos con `[0x603700]`) |
| La alineación `[0x603744]` sale de las **ruedas delanteras** (bloques por rueda en `0x6380d8`, paso `0x300`, ruedas 2 y 3): momento del neumático menos fuerza lateral × constante | `0x41a265` |
| Uno de los términos de amortiguación se pondera con la carga de los neumáticos delanteros (`+0x2F4` + `+0x2F8`) | `0x41a395` |

El mod solo usa estas direcciones si reconoce el ejecutable: comprueba en memoria el código de `0x4083a0` y `0x51e467`. Con otra versión de GP4, vuelve a buscar la tabla de coches en memoria.

**Sentido (comprobado en el juego con una Simucube 2, 2026-10-07):** en DirectInput una magnitud positiva empuja hacia −X (la dirección indica de dónde viene la fuerza). Con el volante girado a la derecha, `+0x5E` es negativo y GP4 manda una magnitud positiva: una fuerza que centra. Las primeras versiones del mod sumaban su propia amortiguación con el criterio contrario, y el volante iba solo de tope a tope en boxes.

**Frecuencia y rigidez (CSV de una carrera):** `+0x5E` cambia en ~29 % de las muestras de 10 ms en movimiento, unos 29 Hz. Por encima de ~50 m/s, la fuerza cerca del centro sube ~16 × la posición del volante normalizada: con 360° de tope a tope, llega al máximo con ~10° de giro. Un muelle tan rígido y con ese retraso hace rebotar un direct drive; una simulación del lazo pide amortiguación ~0,15-0,2 a esa velocidad.

**A 60 fps (carrera del 2026-10-08):** el estado del coche y `+0x5E` se actualizan cada 16,6 ms de media (60 Hz). La frecuencia de la simulación sigue a los fotogramas. El término de alineación `[0x603744]` explica casi toda la fuerza: correlación 0,987 con `+0x5E`.

**Modelo propio frente a GP4:** un modelo bicicleta con Pacejka simplificado, alimentado con el volante real a 500 Hz, la guiñada `+0x0C` (2^32 por vuelta) y el rumbo de la velocidad `+0x18` (65536 por vuelta), reproduce el término de alineación de GP4 con correlación 0,96 (0,98 por encima de 65 m/s). Parámetros: bloqueo 12°, pico 6°, carga aerodinámica 0,0003 y escala 1,30. Las unidades de `+0x0C` y `+0x18` quedan así confirmadas en la práctica: la deriva mediana sale 0°.

**Resultado:** la fuerza que GP4 manda al volante es `−1,5 × (+0x5E) / 10000`. Los valores por defecto del mod reproducen exactamente esa fuerza, entregada a 500 Hz y suavizada. Con `game_signal = aligning`, el mod usa solo el término de alineación y aplica su propia amortiguación.

## Datos del coche

| Hecho | Evidencia |
|---|---|
| Tabla fija de 22 coches en `0x66fef0`, paso `0x41C` | `0x409b88` |
| `[0x66d820]` apunta al coche del jugador (0 si no hay) | `0x409bec`; se intercambia con `[0x66d828]` (modo de dos jugadores alternos) en `0x40b3be` |
| `[0x66d896]` contiene el id del coche del jugador | `0x409b8d` |

## Ruedas

| Hecho | Evidencia |
|---|---|
| Bloque por rueda en `0x6380D8 + i·0x300` (0 tras. izq., 1 tras. dcha., 2 del. izq., 3 del. dcha.; izquierda/derecha por la posición de la rueda): Fy `+0xE0`, Fx `+0x1A0`, Mz `+0x274`, ángulo de deriva `+0x30`, superficie `+0x2D` (byte). **Es memoria temporal compartida**: el bucle de física la reescribe para cada uno de los 22 coches | escrituras `0x417397`, `0x4173B7`, `0x417379`, `0x416AA2`, `0x41E23B`; bucle `0x436346`–`0x4363B4` |
| Registro persistente por coche en `0x625ABC + byte[coche+0x202]·0xA8`, que `0x418942` escribe al final del paso de cada coche. Contiene, por rueda (int32 ×4): carga `+0x38`, deslizamiento lateral `+0x48`, deslizamiento longitudinal `+0x58`, **superficie `+0x68`** (0 asfalto, 1 piano, 3 hierba, 4 grava) y avería `+0x78` | `0x418901` (base), `0x418a0b`–`0x418ab4` |
| Agarre por superficie 1/1/1/0,5/0,75/0,5 y resistencia 256/256/256/2048/8192/2048 (/16384) | tablas `0x602D8C`, `0x602DBC` |
| Aceleración lateral y longitudinal en `0x623EA0`/`0x623EA8`, en ft/s²×16384 (inferido); temporal compartida | `0x41982F` |

El mod lee el registro persistente del coche del jugador para los efectos de superficie: pianos, grava, hierba, dirección más ligera fuera de pista y arrastre de las delanteras.

Para las fuerzas por rueda, el mod sustituye en memoria la llamada de `0x41874A` (`call 0x418942`, final del paso de cada coche, con el coche en `esi`; el `popal` que viene después restaura los registros). Su función llama a la original y, si `esi` es el coche del jugador (`[0x66D820]`), copia Fx, Fy y Mz de las cuatro ruedas y las aceleraciones `0x623EA0`/`0x623EA8` en una instantánea protegida (seqlock). Solo lo hace si encuentra exactamente esa llamada.

**Carrera del 2026-10-09:** en curvas a derechas desliza más la rueda 2, la exterior y cargada, así que 2 = delantera izquierda. Deslizamiento lateral de las delanteras: hasta ~10.600. Deslizamiento longitudinal no nulo el 12 % del tiempo, sobre todo frenando.

## Volante del cockpit

| Hecho | Evidencia |
|---|---|
| El ángulo del volante 3D (65536 = una vuelta) sale de `0x409a47` con el coche en `esi`: ángulo de las ruedas delanteras `+0x418` (suavizado) × 1,5 (salvo bit 0 de `+0xE5`), limitado a ±`[0x6013ac]` (= 3641, 20°, fijado en `0x411825`), × 7,0 (`[0x62ee80]`), × `[0x603468]`/16384 (= 1,125). Máximo: **157,5°** | `0x409a47`-`0x409ab0` |
| Lo usan `0x510882` (`& 0xFFFF` → `[0x8660b0]` → objeto de render `+0x4A28` → `CGP4Cockpit +0x44` → giro de `cockpit_wheel.gp4`) y `0x51b025` (→ `[0x876f1e]`, word) | `0x510887`, `0x49e023`, `0x49d335`, `0x470c46`, `0x46fdc0` |
| Entrada cruda de dirección `+0x6A` (int16): centrada, ±4064 en los extremos calibrados en el menú de GP4. Solo se escribe para mandos analógicos y en las repeticiones del coche del jugador; los coches de la IA y el teclado la dejan a 0 | `0x51df65`-`0x51df99`, `0x520773`, `0x5208bb` |
| Una entrada positiva gira las ruedas, y el volante, hacia el mismo lado | cadena `0x414fe1` → `0x4131e2` → `0x41362a` → `0x435893` → `0x4358f8` |

El mod (`[cockpit] wheel_rotation_deg`) cambia `0x409a47` en memoria por un salto a su propia función. Con entrada cruda, el ángulo es `raw/4064 × grados/2`; sin ella, aplica la fórmula original. Una prueba compara esa fórmula con el código original de `GP4.exe` en 60.000 entradas, sin ninguna diferencia (`tests/win/test_cockpit.cpp`; el código se lee del ejecutable del usuario al ejecutar la prueba y no se guarda en el repositorio).

## Resolución

| Hecho | Evidencia |
|---|---|
| El menú gráfico **no consulta al driver**: `GP2001Menu_GraphicsSwitchResolution` ofrece siete tamaños fijos (640x480, 800x600, 1024x768, 1280x1024, 1600x1200 y dos de 16 bits). Por eso no hay 16:9 | `0x500ec0`, tabla de saltos `0x501130` |
| La elección se guarda en `f1graphics.cfg` (texto `<valor>\t\t\t; <nombre>`): `[RELEASE] Game 3D Horizontal Resolution` / `Vertical Resolution`. Si el archivo existe, el arranque usa esos números directamente | lectura `0x4b2a80`, escritura `0x4b2cc0`, valores en `0xa662bc`/`0xa662c0` |
| `Initialize3DEnvironment` exige ese tamaño exacto en la lista de modos que GP4 construye con `EnumAdapterModes`, de la que solo lee los primeros 256; si no está, "NOT FOUND" y vuelve a 640x480 | `0x5053a0`, `0x504ec5`, `0x50548f`, `0x508f01` |
| La fuente del menú depende de la resolución (1600x1200 usa la grande) | `0x504c99`-`0x504ce7` |

El mod (`[display] resolution = 1920x1080`) cambia en memoria las constantes de la opción 1600x1200: menú `0x500fa5`/`0x500faa`, arranque `0x508d06`/`0x508d0b`, índice del menú `0x505cbc`/`0x505cc4` y fuente `0x504cce`/`0x504cd5`. Solo lo hace si las 8 instrucciones son las esperadas, y una prueba las compara con el `GP4.exe` real. En modo ventana ofrece además una lista de modos corta que incluye esa resolución. En pantalla completa, si esa resolución queda más allá de los 256 primeros modos del controlador (pasaba con 2560x1080 en tarjetas modernas), le da a GP4 los modos reales una vez por tamaño y formato, con la configurada la primera; si ya está entre los 256 primeros, la lista del controlador no se toca. GP4 guarda la frecuencia de cada modo como 0 (`0x504f63`), así que usa la frecuencia por defecto del controlador. `resolution = auto` toma el tamaño del escritorio.

## Vídeo

- `d3d8.dll`: solo importa `Direct3DCreate8`.
- GP4PP modifica el formato del z-buffer en `0x5050fc`, antes de `CreateDevice`, así que no interfiere con los hooks del mod.

## Red

| Hecho | Evidencia | Consecuencia |
|---|---|---|
| **DirectPlay 4** (DirectX 6, `dplayx`) vía `CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay4A)`, con proveedores TCP/IP e IPX | GUID en `0x5f6390`/`0x5f63a0`/`0x5f6370`/`0x5f6380`; funciones `0x506ad0`–`0x5074e0` | En Windows 10/11 hay que activar la característica heredada "DirectPlay". Se puede interceptar `CoCreateInstance` para sustituir el transporte |
| `wsock32`: solo `gethostname`/`gethostbyname` | importaciones | Los sockets los gestiona DirectPlay, no GP4 |
| La sesión se crea con **`dwMaxPlayers = 22`** | `0x506e9a mov [0xb1b898], 0x16` (`DPSESSIONDESC2+0x28`) | El límite de 8 no está en el protocolo: hay que buscarlo en la lógica o los menús de GP4 |
| **No hay tope de 8 humanos en la red.** La unión solo se cierra con 22 jugadores o con la carrera empezada. Las tablas por jugador son de 22: jugadores `0xb1b8d8` (22 × 0x1c), coche→jugador `0x7c2a1c`, banderas `0x7c2a48`, nombres `0x7c2b3c`, último paquete `0x83002b` | `0x541c2c cmp eax,0x16` → `0x507930` (JOINDISABLED); hueco libre de 22 en `0x541a18` | Más de 8 debería funcionar ya. Falta comprobar si algún menú lo limita |
| En carrera, **cada máquina espera el paquete de la anterior** antes de enviar el suyo, en cada paso de la simulación. El anfitrión envía su coche (684 B del registro) y un coche IA rotatorio | espera `0x445bfe`–`0x445c0b`; tiempo límite `0x445c23` (0x40 × 64 ms ≈ 4 s); envío `0x445d60`/`0x445e43` | Retraso ≈ N × ping por paso; un paquete perdido congela ~4 s. Los coches remotos se sobrescriben sin interpolar (`0x44580c`) |
| Solo usa `Send` a todos (vtable +0x68), **sin garantía** salvo que `[0x7a8e00]` esté activo | `0x51ef49`/`0x51ef50` | Pérdidas sin recuperación |
| **El cliente no puede escribir la IP del anfitrión**: el elemento de dirección IP solo se añade si `[0x7ab1a0]` está activo, y eso solo pasa en el anfitrión | `0x5071e6 cmp [0x7ab1a0], bl` / `0x5071ec je` | La búsqueda de partidas es solo por difusión en la red local: por internet, sin una VPN que pase difusiones, no aparece nada |
| Mensajes de menú: cabecera `0x80000000 \| tipo<<24 \| fotograma` y suma de control | construcción `0x5403ca`, suma `0x54161c` | Ayuda a leer `gp4ultimate.log` (líneas `net:`) |

### Carrera en red: protocolo por paso (lectura de `0x445d60` y prueba real)

- `0x445d60` se llama una vez por paso de simulación desde `0x43637f` (solo en red, `[0x7ab020] == 2`), entre la física del coche humano local y la de la IA. Los coches humanos remotos no se simulan: se sobrescriben con el registro recibido (`0x44580c`, 0x2AC bytes) en cuanto llega su paquete.
- El **director de la carrera** es la máquina con `[0x7a8df8] = 0xFF` (índice 0 del anillo calculado en `0x4456c7`), que no es necesariamente la que creó la sesión: en la prueba del 9-10-2026 lo fue el cliente.
- Paquete tipo 1 (`0x44587c`): `[orden en el anillo, 0x7c2ead, 0, 0]`, `[jugador, tipo, banderas (0x80 director, 0x40, 8, 4), 0x8914e2]`, dword de contenido (`0x62f8f4`): 0x20 eventos, 1 semilla aleatoria `0x66d8be/c2`, 2 reloj de carrera `0x66d814` (ms), 4 coche humano, 8 coche IA. El director manda `0x23|4|8` (su coche y un coche IA rotatorio, `0x445e43`); el resto `2|4`.
- Espera: el director manda y espera el paquete de todos (`0x445b9a`, máscara `0x62f8c0 == 0x62f8c8`, tiempo límite 64 × 64 ms). Cada cliente espera el del anterior en el anillo, manda el suyo y espera a todos. Luego compara relojes (`0x445d18`): si va adelantado repite la espera; si va retrasado pone `[0x62f8f9] = 0xFF` y GP4 da otro paso sin dibujar (`0x436432`).
- **Medido** (2 jugadores, 40 ms de ping): 23-24 paquetes por segundo en cada sentido, separación media 42 ms = el ping. El juego da un paso por ida y vuelta: 24 pasos/s en vez de 60, cámara lenta al 40 %. Al irse el otro jugador, 60 paquetes/s al instante.
- El mod (`[net] freerun`, `src/win/net_sync.cpp`) cambia la llamada de `0x43637f`: manda el mismo paquete que GP4, procesa lo que haya llegado con las rutinas de GP4 (`0x445a59`, `0x4459b7`) y sigue. Los clientes siguen el reloj del director con margen (`core/net_pacer.h`): esperan poco si van adelantados y usan los pasos extra de GP4 si van retrasados.

- **Medido con el modo libre** (misma pareja, 6 vueltas): 60-61 pasos/s en las dos máquinas, carrera a velocidad normal y resultados iguales. El director usa su propio límite de fotogramas. El cliente iba en ventana sin límite de fotogramas y lo limitó la sincronización: esperó unos 13 ms en cada paso y se quedó en el borde de la ventana (+30 ms). En el director, los paquetes del cliente llegaban de forma irregular (un paso ninguno, el siguiente dos; separación máxima de 33 ms en uno de cada tres segundos) y el coche remoto parpadeaba. De ahí el búfer de reproducción (`[net] playout`, `core/net_playout.h`): un paquete por jugador y paso, con uno de reserva.
- Los tiempos por vuelta que se ven durante la carrera los calcula cada PC con su reloj, sobre coches remotos que se actualizan con retraso y coches de IA que solo se corrigen uno por paso. Pueden diferir unas centésimas entre PCs. El resultado final coincidió.

`[net] log = 1` (activado por defecto) apunta en `gp4ultimate.log` las llamadas de DirectPlay de GP4: conexiones, direcciones, sesiones, jugadores y mensajes del sistema. Cada segundo añade un resumen del tráfico: paquetes y KB/s por jugador, separación media y máxima entre paquetes, y espera sin mensajes. Copia también los primeros paquetes de cada tipo.

## Herramientas de terceros

- **GPxPatch 4.52** (René Smit, 2019): arranca GP4 como depurador y lo modifica en memoria. Sus módulos `.gxm` y `GPxPDX8.dll` dibujan sobre el dispositivo D3D8 de GP4. No sustituye `dinput8.dll` ni `d3d8.dll`, así que puede convivir con el mod. `GPxCInfo` exporta datos de salpicadero (vuelta, posición, combustible, velocidad, marcha, revoluciones, diferencias) en memoria compartida `"GPxCInfo"`: útil para la pantalla del aro, no para el FFB.
- **GP4PP 2.2.2**: DLL inyectada con CSM, GP4 Memory Access o similar. Ninguna de sus direcciones de parche cae en el código de entrada, FFB o punteros que usa el mod.
