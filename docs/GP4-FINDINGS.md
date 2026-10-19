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

### Cámara y campo de visión (análisis estático, 2026-10-19)

| Hecho | Evidencia |
|---|---|
| El mundo 3D pasa por las transformaciones de Direct3D: `SetTransform` WORLD (`0x100`) por objeto, VIEW (`2`) y PROJECTION (`3`), con vértices sin transformar (FVF `0x142` XYZ+color+textura, `0x112`, `0x102`, `0x152`…) en vertex buffers. Unas 240 llamadas a `SetTransform` y 270 a `DrawPrimitive`/`DrawIndexedPrimitive` | vtable del dispositivo `+0x94`, `+0x118`, `+0x11c`, `+0x130` |
| Los vértices ya transformados (`XYZRHW`: FVF `0x144`, `0x1c4`, `0x2c4`) solo aparecen en 12 funciones: el texto (`CGP4Font::DrawText`, `0x473b20`) y otras de 2D (HUD, menús; por identificar una a una) | `0x44d88e`, `0x45bd20`, `0x48fce0`, `0x4a0f50`, `0x4a8230`, `0x4ae2c0`, `0x4b06f0`, `0x4b09f0`, `0x4beb50` |
| La proyección se construye con `_11 = 1/tan(h/2)`, `_22 = 1/tan(v/2)`, `_33 = lejos/(lejos−cerca)`: campo horizontal `[0xa4fd50]` y vertical `[0xa4fccc]` por separado, cerca `[0xa51fc0]`, lejos 9000 `[0xa4f5a4]`. Copia la matriz en el objeto del dispositivo (`+0x2098`) | `0x4939e0`, `0x493f07`, `0x4945cd` |
| **GP4 es «Vert−»**: fija el campo horizontal `h = [0xa532a0]` y deduce el vertical del aspecto de la pantalla `[0xa532d0]` (ancho/alto): `v = 2·atan(tan(h/2) / aspecto)` en `[0xa53290]`. En una pantalla más ancha se ve lo mismo a lo ancho y **menos** a lo alto: en 21:9 o en triple pantalla la imagen queda muy recortada por arriba y por abajo | `0x493e9f`-`0x493ed5` |
| El campo horizontal sale de la cámara: `h = clamp(45° / (zoom / 512), 5°, 120° o 140°)`, con el zoom en `[0xa53288]` | `0x493da9`-`0x493e63` |
| **El descarte de objetos (frustum culling) usa las matrices de GP4, no las que llegan a Direct3D**: `0x446240` saca los 6 planos de VIEW × PROJECTION de sus variables globales (`[0xa51f58]`, `[0xa50ac8]`) con D3DX (`0x55c3ee` multiplicar, `0x55e1c5` normalizar el plano) | llamadas desde `0x49a1a0`, `0x4a3300`, `0x4c6950` |

Consecuencias:

- `[display] aspect_fix` corrige la matriz al pasar por `SetTransform`, pero la matriz de GP4 no está estirada (se construye para el aspecto real), así que no detecta nada que corregir. Y si ensanchara la vista solo ahí, se verían huecos en los lados: GP4 ya habría descartado lo que cae fuera de su campo original.
- **Ultrapanorámico (Hor+)**: hay que cambiar el cálculo de GP4 en `0x493aa0` (mantener el campo vertical de 4:3 o 16:9 y ensanchar el horizontal, `h = 2·atan(tan(v/2) · aspecto)`). Así la matriz y el descarte coinciden. El límite de 120°/140° solo importa para triple pantalla.
- **Triple pantalla con monitores en ángulo**: como la cámara pasa por `SetTransform`, `d3d8.dll` puede grabar las llamadas de dibujo de la escena y repetirlas tres veces, con la vista girada y la proyección y el viewport de cada monitor (`screen_view` en `core/projection.h`). GP4 tendría que descartar con un campo que cubra las tres pantallas. Falta ver cómo se dibujan los retrovisores (render a textura) y el HUD.
**Capturas de F11 a 5760x1080 (2026-10-19):**

- **Menús**: todo con vértices sin transformar (`XYZ|DIFF|TEX1`, `XYZ|TEX1`), proyección ortográfica (`_11 = _22 = 2`, sin perspectiva) y un viewport 4:3 centrado que calcula GP4 (`x=2159 w=1440 h=1080`). Borra la pantalla entera en negro y el viewport completo en gris `0x777777`. Aun así, en triple pantalla el texto sale estrecho y deformado: GP4 calcula algo del menú (el tamaño de las letras, probablemente) con el ancho total y no con el del viewport.
- **En pista, GP4 transforma él mismo los vértices**: los 683 dibujos del fotograma son `XYZRHW|DIFF|TEX1` (ya en píxeles de pantalla), sin ningún `SetTransform`. El orden: una textura de 64x64, una de 512x512 (212 dibujos), otra de 64x64, una de 720x720 (150), otra de 720x720 (solo se borra) y por último la pantalla (318 dibujos, el mundo y el HUD mezclados, todos `XYZRHW`). Las rutas con `SetTransform` del análisis estático son las de los menús y otras pantallas.
- Consecuencias: el cambio de campo de visión de `[display] wide_fov` actúa donde debe (GP4 proyecta con esos valores). Pero en pista no se puede repetir la escena con otra cámara desde `d3d8.dll` (triple pantalla con monitores en ángulo): habría que hacer que GP4 dibuje la escena varias veces. Y para mover el HUD hay que distinguir sus dibujos de los del mundo, todos `XYZRHW`.
- **El motor de menús** guarda su propio tamaño de pantalla en `[0x9ed5b8]` (ancho) y `[0x9ed5bc]` (alto). Lo escriben `0x5b73b0` (`SetScreenSize(ancho, alto)`, stdcall), `0x5b47e5` (al iniciarse, desde `0x4ffdc9`) y `0x5ccc61`. `0x5cc820` calcula el viewport y la proyección ortográfica (`D3DXMatrixOrtho`, `0x55cd4b`): si ancho/alto < 2 usa la pantalla entera (en 16:9, el menú 4:3 estirado); si es ≥ 2 (`[0x5fa6ac]` = 2,0) lo encierra en un 4:3 centrado. Unas 40 funciones más del motor usan ese tamaño y no tienen en cuenta el recorte, de ahí el texto estrecho en 21:9 y triple pantalla. El cursor del menú se mueve por diferencias: `0x5c7d5b` devuelve el cursor de Windows a (400, 300) de la ventana.
- `[display] menu_aspect` (16:9) le da al motor de menús un ancho de 16:9 como máximo en esas tres escrituras, y `d3d8.dll` mueve sus viewports al centro del back buffer real. Mientras se ven los menús (fotogramas sin dibujos `XYZRHW` en el back buffer), el viewport de pantalla completa también se centra y los lados se borran en negro.
- `0x493aa0` también ajusta el campo de visión según el ancho de la pantalla (`[0xa69878]` es la mitad del ancho, `[0xa6987c]` la de la altura): factores fijos para 640/800/1024/1280/1600 y una fórmula para el resto. Con la cámara `[0xa6e21c] = 0` lee las teclas `+`/`-` (`VK_ADD`/`VK_OEM_PLUS`, `VK_SUBTRACT`/`VK_OEM_MINUS`) y cambia un zoom `[0xa66380]` de 0 a 100.
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
- **Prueba del 10-10-2026 (servidor de GP4 Ultimate, 2 jugadores, ~10 min)**: el GP4 del director se quedó congelado 1,5 s; el cliente (en ventana, sin límite de fotogramas) dejó de tener reloj al que seguir y, sin las esperas del ritmo, dio hasta 1.100 pasos/s (carrera 18 veces más rápida). Al volver, el director recibió 500 paquetes de golpe con tiempos muy adelantados y su GP4 se cerró. Desde entonces, con el director callado, el cliente sigue su reloj extrapolado a velocidad real: no da pasos extra, pero tampoco se adelanta. Además, los paquetes de 1.400 bytes del director iban como garantizados (más de un datagrama); ahora van en partes sin reenvío, como con DirectPlay.
- Los tiempos por vuelta que se ven durante la carrera los calcula cada PC con su reloj, sobre coches remotos que se actualizan con retraso y coches de IA que solo se corrigen uno por paso. Pueden diferir unas centésimas entre PCs. El resultado final coincidió.

### Lo que GP4 usa de DirectPlay (lectura completa de `0x506ad0`–`0x507990`)

Todo pasa por unas pocas funciones de GP4; el objeto `IDirectPlay4A` está en `[0xb1bb4c]`, el `IDirectPlayLobby3A` en `[0xb1bb48]`, el DPID propio en `[0xb1bb40]` y la descripción de sesión en `0xb1b870`. No hay más referencias a esos punteros en el ejecutable.

| Función | Llamadas | Detalle |
|---|---|---|
| `0x506ad0` (arranque) | `CoCreateInstance(DirectPlay)` temporal, `EnumConnections` (callback `0x506c40`), y por cada proveedor otro objeto + `InitializeConnection(conn, 0)` de prueba, `Release` | Tabla `0xb1b828`: 0 serie, 1 módem, 2 IPX, 3 TCP/IP. `0xb1bb74[i]` = 1 si IPX/TCP/IP funcionan |
| `0x5074e0` | Objeto temporal, `InitializeConnection`, `GetPlayerAddress(0, …)` y `EnumAddress` del lobby | Solo lista módems (`DPAID_Modem`); irrelevante para TCP/IP |
| `0x506d30` (entrar en red) | `CoCreateInstance(DirectPlay, IID_IDirectPlay4A)`, `CoCreateInstance(DirectPlayLobby, IID_IDirectPlayLobby3A)`, descripción de sesión, dirección (`0x507000`), `InitializeConnection(addr, 0)` | Sesión: `dwSize 0x50`, `dwFlags = DPSESSION_MIGRATEHOST`, nombre fijo `"GP3 2000 Network Session"` (22 jugadores, app `20e0ff41-…`) o `"GP3 2000 2 Player Session"` (2, app `20e0ff40-…`), según `[0x7ab4b8]` |
| `0x507000` (dirección) | Lobby `CreateCompoundAddress` dos veces (tamaño, luego datos) | TCP/IP: proveedor + `DPAID_INet`; la IP (`0x7aafea`) solo si `[0x7ab1a0]` (anfitrión); el cliente la manda vacía, es decir, búsqueda por difusión |
| `0x506ee0` (crear) | `Open(desc, DPOPEN_CREATE \| DPOPEN_RETURNSTATUS)` repetido mientras devuelva `DPERR_CONNECTING`; `CreateEventA`; `CreatePlayer(&id, nombre, evento, NULL, 0, 0)`; `GetCaps` | El evento y las capacidades no se leen nunca |
| `0x506f70` (unirse) | Copia el `guidInstance` elegido (`0xb1b8c8`), `Open(desc, DPOPEN_JOIN)` repetido con `DPERR_CONNECTING`, `CreatePlayer` | |
| `0x507740` (lista) | `EnumSessions(desc, 0, cb 0x5078b0, ctx, DPENUMSESSIONS_AVAILABLE \| ASYNC)` en cada refresco del menú | Callback: para en `DPESC_TIMEDOUT`; guarda **6 sesiones como mucho** (`0xae6b68`, 0x34 bytes: GUID, nombre, `dwFlags`). **El nombre se copia sin límite en un hueco de 32 bytes**: un sustituto no debe pasar de 31 caracteres |
| `0x507650` | `EnumSessions(…, AVAILABLE \| ASYNC \| RETURNSTATUS)` y elige la única sesión si solo hay una | Errores: `BUSY` 2, `NOCONNECTION` 3, `TIMEOUT` 4, `USERCANCEL` 6, el resto 5 |
| `0x507360` (recibir) | `Receive(&from, &to, DPRECEIVE_ALL, 0x7a97b0, 0x1000)` | Mensajes del sistema (`from == 0`): solo mira `DPSYS_CREATEPLAYERORGROUP` (en la sesión de 2 cierra la entrada con `SetSessionDesc`) y `DPSYS_HOST` (`0x5416c2`: pasa a ser anfitrión, `[0x7ab1a0] = 0xFF`, y quita al anterior, `[0xae6b64]`). **Ignora `DPSYS_DESTROYPLAYERORGROUP`**: las salidas las detecta su propio protocolo. Un mensaje de más de 4 KB bloquearía la recepción (no se descarta) |
| `0x507410` (enviar) | `Send(yo, destino, garantizado ? DPSEND_GUARANTEED : 0, 0x7a97b0, tamaño)` | Destino: todos (`0`) o un DPID que GP4 apuntó de un `from` recibido (`0x5403ca`). Garantizados: menús (`0x5335e5`, y `0x5403ca` cuando quien lo llama lo pide) y los paquetes de carrera con `[0x7a8e00]` |
| `0x5074a0` | `GetMessageCount(yo, &n)` | Antes de recibir, en `0x54b8d1` |
| `0x507930` / `0x507960` | `SetSessionDesc` con `DPSESSION_JOINDISABLED` puesto / quitado | `0x541c5a` / `0x541c43` |
| `0x5072f0` (salir) | `Close`, libera la dirección, `Release` del lobby y del objeto | |

No usa `DestroyPlayer`, `EnumPlayers`, grupos, `SendEx`, ni `QueryInterface`. Los DPID solo tienen que ser únicos y los mismos para todos.

### Quién dirige la carrera y si puede hacerlo un PC sin coche

- **Elección del director**: al arrancar, GP4 cronometra un bucle (`0x54c746` → `0x54c815`, 400.000 vueltas; resultado en `[0x833c98]`, se escribe en su registro como "Speed (c.f. ???)"). Cada PC manda ese tiempo al crear la partida (`0x53f80f`) o al unirse (mensaje de menú 2, `0x5401a8`, con la marca `0x12435687` y la versión `0x66`), y queda en el registro de jugador (`0xb1b8d8`, 22 × 0x1c: `+0` DPID, `+0x14` estado, `+0x17` orden, `+0x18` tiempo). `0x541b03` numera a los jugadores por ese tiempo, de menor a mayor; `0x541730` copia el número a cada coche humano (`0x7c2a32`, con `0x7c2a1c` = jugador de cada coche) y `0x4456c7` reparte el orden del anillo por coches: el **PC más rápido dirige**. `[net] race_director` sustituye el tiempo justo antes de `Open` (1 para preferir, `0x7ffffff0` para evitar).
- **El director sin coche propio está previsto en el código**: `0x445d60` manda reloj, semilla, eventos y un coche IA, y añade su coche (bandera 4) solo si `[0x62f8b0]` (coche local) no es 0. Ese puntero se pone a 0 cuando el coche local deja de ser controlado por la red (`0x445fb4`), y `0x44576b` no lo asigna a un coche propio con la marca `+0x7c & 0x40` (lo deja como `+0x414 |= 0x20`).
- **Pero la elección pasa por los coches**: `0x4456c7` solo marca director (`[0x7a8df8] = 0xFF`) a un PC que tiene un coche humano en la tabla. Un PC sin ningún coche no puede dirigir, y entonces dirigiría el siguiente. Para un director dedicado hay dos caminos: que tenga coche y no lo conduzca (retirado o con la marca `0x40`), o cambiar la elección en todos los PCs (todos llevan el mod).
- Por comprobar en una carrera: si un director que se retira sigue dirigiendo (las tablas de `0x4456c7` no cambian al retirarse) y qué pone la marca `+0x7c & 0x40`.

### Jugadores que se van de la carrera

- Un jugador que sale de la partida marca su paquete con `0x40` en la cabecera (`[0x7a8dfa]`). El director lo apunta en su máscara de eventos (`0x4459b7`: `bts [0x62f8e0], jugador`) y la manda a todos con la bandera `0x20` (`0x445921` / `0x445b0b` → `[0x62f8dc]`). Al final del paso, `0x446058` quita esos jugadores de la máscara de activos `[0x62f8c8]`; `0x445f75` saca sus coches de la red (`0x445fb4`: `+0x414` sin las marcas de red, `+0xe5 |= 4`) y `0x4456c7` reparte de nuevo el anillo (y el director, si era él).
- **Un jugador cuyo GP4 se cuelga no avisa**: GP4 ignora `DPSYS_DESTROYPLAYERORGROUP` y la espera original (`0x445b9a`) solo cuenta los tiempos agotados (`[0x62f954]`). Su coche se queda clavado en pista. `[net] drop_silent_after_s` (10 s) hace lo mismo que si hubiera salido: el director lo pone en su máscara de eventos, y si el callado es el director, cada PC lo pone en `[0x62f8dc]` por su cuenta.

### Cierres en carreras largas (pruebas del 10-10-2026)

- Dos cierres con el mismo patrón: el GP4 de un jugador se queda **parado 1,5-2,8 s** (sin pasos ni envíos; en uno, el hilo del FFB también se retrasó y bajó a 371 Hz), al volver procesa **cientos de paquetes de carrera atrasados** en pocos pasos (341 en 4 pasos; 503 en 13) y el proceso desaparece segundos después, sin más líneas en el log.
- En las primeras pruebas de la 1.1.0, el hilo del juego leía los mandos con el mismo cerrojo que el hilo del FFB usa para mandar la fuerza al volante: si el driver del volante tardaba en una llamada, el juego entero esperaba. Ahora un hilo propio lee los mandos y el juego nunca espera más de 4 ms (si no hay lectura nueva, usa la anterior). Las esperas largas del driver quedan en el log (`input: reading the controllers took`, `ffb: the wheel driver took`).
- Y tras una parada, el búfer de reproducción solo guarda los 4 paquetes de estado más recientes de cada jugador (más los que anuncian algo: salidas, eventos de carrera), en vez de entregarlos todos de golpe.
- **Cierre en el sonido de GP4** (informe de cierre del 10-10-2026: `0x5ad79a`, lectura de la dirección 0, en un hilo del temporizador de WINMM). El objeto de sonido (vtable `0x5f9c80`) guarda sus búferes DirectSound en una tabla en `+0x25c`. El hilo del temporizador (`0x5a8810`) cambia volúmenes (`SetVolume`, `0x5ad730`) mientras el hilo del juego libera búferes (`0x5ad9f0`, que pone la casilla a 0). Se excluyen con una marca de ocupado en `+0x3c`, pero `0x5a85a0`/`0x5a85e0` la comprueban y escriben sin operación atómica, `0x5adb07` la borra aunque no la haya cogido, y `SetVolume` lee la casilla dos veces: si se libera entre las dos lecturas, llama a través de un puntero nulo. No tiene nada que ver con la red; es más probable cuanto más dura la carrera. `[general] fix_sound_crash` (activado por defecto) sustituye cuatro funciones del objeto: marca atómica que espera a quien la tiene, y `SetVolume` que vuelve a mirar la casilla antes de usarla.

### Coches remotos: por qué saltan y cómo predecirlos

- El paso de simulación en red (`0x4362cd`): primero actualiza los coches humanos de este PC (`+0x414 & 0x40` → `0x435b61`), luego el intercambio (`0x445d60`), y después todos los demás **salvo los humanos de la red** (`+0x414 & 0x80`). Los coches de la IA se simulan en todos los PCs (y el director corrige uno por paso); los de los otros jugadores no se simulan nunca: los sustituye el registro de su paquete (`0x44580c`: corrige los punteros de pista con `0x40b186`, comprueba que caen dentro de la pista, invierte `+0x7c & 0x40` y copia `[0x62f8b8]` = 0x2AC bytes sobre el coche `+0x202`).
- Los mandos del jugador local llegan a su coche por otro camino (`0x444b59` desde el búfer `[0x62d1a8]` al coche `[0x66d828]`), no dentro de `0x435b61`: un coche remoto conserva en su registro los mandos de su último paquete (`+0x6a` dirección, `+0x6c` freno, `+0x6d` embrague, `+0x6e` acelerador).
- `[net] predict` (experimental): si en un paso ningún paquete ha cambiado el registro de un coche remoto, se le aplica `0x435b61`, como a la IA, durante `predict_max_steps` pasos como mucho. Falta comprobar en carrera los efectos secundarios (vueltas, tiempos, boxes) de actualizar un coche humano remoto.

`[net] log = 1` (activado por defecto) apunta en `gp4ultimate.log` las llamadas de DirectPlay de GP4: conexiones, direcciones, sesiones, jugadores y mensajes del sistema. Cada segundo añade un resumen del tráfico: paquetes y KB/s por jugador, separación media y máxima entre paquetes, y espera sin mensajes. Copia también los primeros paquetes de cada tipo.

## Herramientas de terceros

- **GPxPatch 4.52** (René Smit, 2019): arranca GP4 como depurador y lo modifica en memoria. Sus módulos `.gxm` y `GPxPDX8.dll` dibujan sobre el dispositivo D3D8 de GP4. No sustituye `dinput8.dll` ni `d3d8.dll`, así que puede convivir con el mod. `GPxCInfo` exporta datos de salpicadero (vuelta, posición, combustible, velocidad, marcha, revoluciones, diferencias) en memoria compartida `"GPxCInfo"`: útil para la pantalla del aro, no para el FFB.
- **GP4PP 2.2.2**: DLL inyectada con CSM, GP4 Memory Access o similar. Ninguna de sus direcciones de parche cae en el código de entrada, FFB o punteros que usa el mod.
