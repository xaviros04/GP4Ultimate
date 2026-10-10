# GP4 Ultimate - guía de uso

[English version](GUIDE.md)

1. [Requisitos](#requisitos)
2. [Instalación](#instalación)
3. [Mandos](#mandos)
4. [Configurar GP4](#configurar-gp4)
5. [Force feedback](#force-feedback)
6. [Resolución, ventana y pantallas](#resolución-ventana-y-pantallas)
7. [Carreras online](#carreras-online)
8. [Problemas frecuentes](#problemas-frecuentes)
9. [Desinstalar](#desinstalar)

## Requisitos

- Grand Prix 4 instalado, con el `GP4.exe` estándar (6.275.072 bytes, sha256 que empieza por `2046f390`). Con otro ejecutable los mandos y el force feedback siguen funcionando, pero los parches de código (menú de resolución, volante del cockpit, online, fuerzas de frenada) se desactivan y el log lo indica.
- Windows 10 u 11.
- Para jugar online con DirectPlay (no hace falta con un [servidor de GP4 Ultimate](#carreras-online)): la característica de Windows **DirectPlay** (Panel de control → Programas → Activar o desactivar las características de Windows → Componentes heredados → DirectPlay).
- Opcional: [d3d8to9](https://github.com/crosire/d3d8to9) o dgVoodoo2 si ya los usas con GP4 (ver [Encadenar otras DLL](#encadenar-otras-dll)).

## Instalación

1. Descomprime la versión descargada en la carpeta donde está `GP4.exe`.
2. Ya está: no hay instalador y no se escribe nada fuera de esa carpeta.

Si en la carpeta ya hay un `dinput8.dll` o un `d3d8.dll` de otro mod, lee [Encadenar otras DLL](#encadenar-otras-dll) antes de sobrescribirlo.

## Mandos

Abre `gp4ultimate-config.exe` **con GP4 cerrado** (GP4 se queda los mandos para él). El idioma sigue al de Windows; se cambia con el selector de la barra superior.

### Pestaña Controles

GP4 ve un único mando, **GP4 Composite Wheel**, construido con tus aparatos.

1. Conecta todo y pulsa **Buscar mandos**. El panel de la izquierda los lista; despliega uno para ver moverse sus ejes y botones.
2. En cada función pulsa **Detectar** y mueve el control:
   - **Dirección**: gira el volante a la derecha (unos 90°).
   - **Acelerador**, **Freno**, **Embrague**: pisa cada pedal a fondo.
3. **Ajusta** un eje si hace falta: zona muerta, **curva (gamma)** (más de 1 es más suave al principio, útil en frenos de célula de carga; déjala en 1 si el software de tus pedales ya aplica una curva) e inicio/fin del recorrido (baja el fin para llegar al 100 % sin pisar a fondo).
4. **Botones**: GP4 solo lee 8. Detecta los que quieras (el botón 1 es "aceptar" en los menús de GP4) y dales función en el menú de controles del propio GP4.
5. **Guardar** (Ctrl+S). La versión anterior queda en `gp4ultimate.ini.bak`.

Si tu `gp4ultimate.ini` viene de otro PC, pulsa **Empezar de cero** y asigna tus mandos.

Los mandos que no asignes (vJoy, un freno de mano...) se ocultan a GP4, que si no los abriría en exclusiva sin usarlos.

## Configurar GP4

En las opciones de controles de GP4:

1. Elige **GP4 Composite Wheel** como mando.
2. Asigna dirección = eje X, acelerador = eje Y, freno = eje Rz, embrague = eje Z.
3. **Calibra**: gira el volante a tope a los dos lados y pisa cada pedal. GP4 calibra los ejes en bruto él mismo.
4. **Activa el force feedback.** GP4 solo calcula la fuerza de la dirección con el force feedback activado, y el mod usa esa fuerza. La fuerza FFB del menú de GP4 también escala la señal (un 60 % es un buen comienzo).

## Force feedback

1. En la pestaña **Controles**, abajo: **La fuerza va a** = tu base. Con el volante suelto, pulsa **Probar sentido**: la base empuja a un lado y luego al otro, y se marca "Invertir la fuerza" si tu base va al revés.
2. En la pestaña **Force feedback**, **Giro de tu volante**: los mismos grados que en el software de tu base (por defecto 360; p. ej. 540 o 900).
3. **Fuerza general**: con una base direct drive empieza bajo (0,3-0,5) y sube.

### Amortiguación

GP4 solo actualiza su fuerza entre 30 y 60 veces por segundo, y a alta velocidad el volante puede oscilar en recta ("ping-pong"). La amortiguación del mod crece con la velocidad para evitarlo.

- Por defecto la amortiguación general la pone el software de tu base (True Drive de Simucube, Fanatec, Moza...): ponle algo de amortiguación allí. El mod solo añade **Amortiguación a alta velocidad** (0,15), la parte que sujeta el centro en recta, que la base no puede hacer porque no sabe a qué velocidad va el coche.
- Si notas el volante "rápido y elástico", falta amortiguación en total: súbela en la base, o pon aquí **Amortiguación base** (sobre 0,03) y **Amortiguación en boxes** (sobre 0,04).

### Pestaña Efectos

Pianos, grava, hierba, arrastre de las delanteras (aviso de subviraje), tirón al frenar, vibración al bloquear, cambios de marcha, impactos y baches. Cada uno tiene su intensidad; 0 lo desactiva. Todos se escalan con la fuerza general: si los notas flojos, sube primero esa.

### Ajustar con datos

Con **Telemetría de fuerza** activada (pestaña Avanzado), el mod escribe `gp4ultimate_ffb.csv` con el estado del coche y cada componente de la fuerza, unos MB por carrera. Sirve para afinar o para informar de un problema.

## Resolución, ventana y pantallas

La pestaña **Cockpit y pantalla**.

### Cualquier resolución

El menú gráfico de GP4 solo ofrece tamaños 4:3 y 5:4 hasta 1600x1200.

1. Pon en **Resolución del juego** tu tamaño, p. ej. `1920x1080`, `2560x1080`, `3440x1440`, `5760x1080`, o `auto` (la del escritorio).
2. Arranca GP4 y elige **1600x1200** una vez en sus opciones gráficas: esa opción pasa a ser tu resolución.

En pantalla completa tu monitor tiene que admitir ese tamaño. Las tarjetas modernas listan cientos de modos y GP4 solo lee los 256 primeros, así que las resoluciones grandes podían no aparecer; cuando pasa, el mod le da a GP4 una lista más corta con la tuya la primera. `gp4ultimate_d3d.log` dice qué ha hecho y, si tu monitor no tiene ese tamaño, lista los más parecidos. En modo ventana vale cualquier tamaño.

### Modo ventana

**Jugar en ventana** abre GP4 como una ventana sin bordes del tamaño de su resolución, centrada en el monitor. Con NVIDIA Surround / AMD Eyefinity queda en la pantalla central. Útil para una ventana de 1920x1080 en un escritorio de triple pantalla.

### Imagen panorámica

Si la imagen sale estirada a lo ancho, activa **Corrección Hor+** (el log indica cuándo hace falta). **Escala del campo de visión** (Mostrar avanzados) por encima de 1 abre la vista.

### Volante del cockpit

GP4 gira el volante del cockpit como mucho 157°. **Giro del volante del cockpit** (por defecto 360) hace que gire como el tuyo: pon los grados de tu volante, o 0 para el original. Calibra el volante en GP4 girándolo a tope a los dos lados.

### Encadenar otras DLL

Si usas d3d8to9 o dgVoodoo2, renombra su `d3d8.dll` (por ejemplo a `d3d8to9.dll`), pon el `d3d8.dll` de GP4 Ultimate en su lugar y escribe el nombre nuevo en **Otro d3d8.dll encadenado**. Igual con otro `dinput8.dll` (Avanzado → Otro dinput8.dll encadenado, con Mostrar avanzados).

## Carreras online

Hay dos formas de conectarse:

- **Un servidor de GP4 Ultimate** (recomendado): sin VPN, sin DirectPlay y sin abrir puertos en los PCs de los jugadores. Alguien pone el servidor, todos escriben su dirección en **Online** → **Servidor de partidas**, y listo. El servidor puede ir dentro del propio GP4 del anfitrión (`local`). Detalles en [Online play through the GP4 Ultimate server](ONLINE.md) (en inglés); lo esencial:
  1. Quien lleve el servidor pone **Servidor de partidas** = `local`, abre (redirige) en el router el puerto **UDP 24004** hacia su PC y permite `GP4.exe` cuando lo pregunte el firewall de Windows (puede salir detrás de la ventana del juego).
  2. Los demás escriben su IP pública (busca "cuál es mi IP") en **Servidor de partidas**.
  3. En GP4, partida en red por **TCP/IP**: la partida aparece con el nombre de quien la crea (`Alex 1/22`).
  También puede ir en un PC que esté siempre encendido (`gp4ultimate-server.exe`) o en un servidor Linux en la nube, y así nadie tiene que abrir nada en casa.
- **DirectPlay**, como el juego original: por internet necesita una red local virtual como **Radmin VPN** o ZeroTier. Deja **Servidor de partidas** vacío. [Paso a paso](#paso-a-paso-directplay).

En los dos casos, en GP4 se elige **TCP/IP**, uno crea la partida y los demás entran desde la lista.

### Qué cambia GP4 Ultimate

GP4 original hace esperar a cada PC, en cada paso de la simulación, a los paquetes de los demás. Con cualquier ping la carrera va a cámara lenta: con 40 ms de ping, al 40 % de velocidad. Con GP4 Ultimate:

- cada PC va a sus fps y usa lo último que le ha llegado de los demás (**Carrera sin esperas**);
- los relojes de carrera se mantienen a la par (un PC que se adelanta espera unos milisegundos y uno que se retrasa da pasos extra);
- los coches de los demás se mueven sin tirones (**Suavizar los coches de los demás**, opción avanzada activada por defecto, guarda un paquete de reserva: más o menos un fotograma, 17 ms a 60 fps, de retraso).

Todos deben usar la **misma versión** de GP4 Ultimate. A los amigos que solo quieran jugar online les valen los valores por defecto: el configurador solo hace falta para los mandos.

### Paso a paso (DirectPlay)

1. Todos en la misma red de Radmin VPN.
2. En cada PC: DirectPlay activado, y el firewall de Windows permitiendo `GP4.exe` y `C:\Windows\SysWOW64\dplaysvr.exe` (Panel de control → Firewall de Windows Defender → Permitir una aplicación). La red de Radmin, como **Privada** (Configuración → Red → el adaptador de Radmin VPN → Perfil de red).
3. Uno crea la partida (TCP/IP); los demás la ven en la lista y entran.

### Conviene saber

- La carrera la dirige un PC (reloj y coches de la IA). Lo elige GP4, no necesariamente el que creó la partida.
- Cada PC muestra sus propios tiempos por vuelta, que pueden diferir unas centésimas entre PCs. El resultado final coincide.
- Los coches de los demás se ven con medio ping de retraso. Hasta unos 100 ms de ping se juega muy bien.
- `gp4ultimate.log` escribe cada segundo una línea `netsync` (pasos, paquetes, diferencia de reloj, esperas). Adjunta los logs de todos los jugadores al informar de un problema.

### Si alguien no puede entrar

Con un servidor de GP4 Ultimate, mira las líneas `net:` de `gp4ultimate.log`: `connected to the server` es que todo va bien; `no answer from the server` es que la dirección está mal, el servidor no está en marcha o su puerto UDP no está abierto ([más](ONLINE.md#troubleshooting)). Con DirectPlay:

| Síntoma | Causa | Solución |
|---|---|---|
| La partida no aparece en la lista | Paquetes de búsqueda bloqueados, o no estáis en la misma red | Misma red de Radmin; reglas del firewall de arriba |
| Aparece, pero al entrar se queda ~35 s y falla (`DPERR_TIMEOUT` en el log) | Firewall o antivirus bloqueando la conexión de DirectPlay | Permitir `GP4.exe` y `dplaysvr.exe`, red como Privada; si hace falta, abrir TCP 47624 y TCP/UDP 2300-2400 de entrada. Los firewalls de otros antivirus necesitan las mismas reglas |
| GP4 dice que la red no está disponible | DirectPlay no instalado | Activar la característica DirectPlay de Windows |

## Problemas frecuentes

| Problema | Qué hacer |
|---|---|
| GP4 no ve el volante | Elige **GP4 Composite Wheel** en el menú de controles de GP4. Mira `gp4ultimate.log`: lista los mandos encontrados y qué alias encajó con cada uno |
| No hay force feedback | Force feedback activado en el menú de GP4; la base correcta en **La fuerza va a**; **Fuerza general** mayor que 0 |
| El volante tira hacia dentro de las curvas | Repite **Probar sentido** (o marca **Invertir la fuerza**) |
| El volante oscila en recta | Sube **Amortiguación a alta velocidad** (0,15 → 0,2) |
| La resolución no funciona | Elige "1600x1200" en el menú gráfico de GP4. Mira `gp4ultimate_d3d.log`: si tu monitor no tiene ese modo, lista los más parecidos; usa uno de ellos o **Jugar en ventana** |
| Imagen estirada | Activa **Corrección Hor+** |
| Otra cosa | Adjunta `gp4ultimate.log` y `gp4ultimate_d3d.log` en un issue |

## Desinstalar

Primero, si pusiste una resolución propia, elige un tamaño normal (p. ej. 1600x1200 o 1024x768) en las opciones gráficas de GP4: GP4 guarda el tamaño elegido en `f1graphics.cfg` y, sin el mod, no encontraría un tamaño panorámico y bajaría a 640x480. (O borra las dos líneas `Game 3D ... Resolution` de `f1graphics.cfg`.)

Después borra `dinput8.dll`, `d3d8.dll`, los archivos `gp4ultimate*` y los logs. Si habías encadenado otra DLL, devuélvele su nombre original.
