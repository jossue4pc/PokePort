# PokePort — PokéPark Wii para PC / PokéPark Wii PC port (unofficial)

**Español** · [English](#english)

Port nativo para PC de *PokéPark Wii: Pikachu's Adventure* (USA), hecho por fans mediante
**recompilación estática**: el código del juego se traduce a C++ y se compila para Windows, en vez de emularse.

> Este repositorio **no contiene ningún archivo del juego**. Necesitas tu propia copia de *PokéPark Wii: Pikachu's
> Adventure* (USA, `R8AE01`) en formato ISO, RVZ, WBFS, WIA o CISO (por ejemplo, volcada de tu disco con tu Wii).
> El lanzador la lee **solo en tu PC** y compila el port en tu equipo.

## Características
- **60 fps nativos** en todo el juego (el original va a 30 en el parque) sin cambiar la velocidad del juego.
- **Panorámico 16:9** real y **resolución interna** a la de tu pantalla (o 2x, 3x, 4x…).
- Mando o teclado del PC como **Wiimote virtual** (horizontal por defecto), configurable; los Wiimotes reales
  emparejados por Bluetooth deberían funcionar (sin probar todavía).
- Sonido, vídeos, guardado y menú HOME. Ajustes dentro del juego con **F10**.

## Requisitos
- Windows 10/11 de 64 bits y una GPU con DirectX 12 o Vulkan.
- Unos **10 GB libres** y conexión a internet durante la primera instalación (descarga el compilador y las bibliotecas).
- Tu imagen de disco de *PokéPark Wii: Pikachu's Adventure* (USA, revisión 0).

## Cómo jugar
1. Descarga la última versión desde **[Releases](../../releases)** (archivo `PokePort-vX.Y.Z.zip`) o con **Code → Download ZIP** y descomprímelo en una carpeta **sin espacios raros ni
   tildes** (por ejemplo `C:\Juegos\PokePort`).
2. Abre **`PokeParkLauncher.bat`**.
3. Pulsa **Elegir disco…** y selecciona tu ISO/RVZ (o **Carpeta…** si lo extrajiste con Dolphin: debe contener
   `sys\main.dol` y `files\`).
4. Pulsa **Instalar**. La primera vez tarda entre 15 y 40 minutos (descarga herramientas y compila el juego).
5. Pulsa **▶ JUGAR**. Puedes crear un acceso directo en el escritorio desde el lanzador.

Todo queda dentro de la carpeta del repositorio: `PokePark\` es el juego listo (con tus partidas en
`PokePark\UserData`), `game\` los datos extraídos de tu disco y `.build\` las herramientas y la compilación.

## Controles (por defecto, mando tipo Xbox / teclado configurable en F10)
| Wiimote (horizontal) | Mando del PC |
|---|---|
| Cruceta | Stick izquierdo o cruceta |
| 2 | A |
| 1 | B |
| A | X |
| B | Y |
| + | Start |
| − | Z (botón de hombro) |
| HOME | L |
| Sacudir | R o stick derecho |

Se cambian en **F10 → Controller settings → Wii Remote (virtual)**.

## Actualizaciones
Al abrirse, el lanzador comprueba si hay una versión nueva en GitHub (también con el botón **Buscar actualizaciones**).
Si la hay, muestra un aviso con **Ver notas**, **Actualizar**, **Ahora no** u **Omitir esta versión**. Al actualizar:
- descarga el release y comprueba su integridad (SHA-256) antes de tocar nada;
- guarda una copia de seguridad (`.build\backup`) y, si algo falla a mitad, restaura la versión anterior;
- **no toca** tus partidas ni ajustes (`PokePark\UserData`), tu disco extraído ni las herramientas descargadas;
- recompila solo lo que cambió; el `PokePark.exe` anterior sigue funcionando hasta que el nuevo compila bien.

## Rendimiento y diagnóstico
- **API gráfica** (lanzador → Opciones): *Auto*, *Direct3D 12* o *Vulkan*. En tarjetas NVIDIA GTX 10xx (Pascal) prueba Vulkan.
- **Resolución:** si tu GPU va justa, usa *1x (Wii)* o *2x*.
- **Modo diagnóstico** (botón naranja): abre el juego registrando el rendimiento. Al cerrarlo muestra un resumen
  (CPU/GPU, trabajo por fotograma y tirones por causa: shaders, GPU o juego) y el archivo para enviarlo en un *issue*.
- El port incluye una **caché inicial de shaders de PokéPark**: la primera vez que arrancas verás «Preparing shaders
  X / Y» unos segundos y después los efectos ya están listos. Durante la partida se compila con pocos hilos para no
  quitarle CPU al juego.
- **Compartir caché de shaders** (botón morado): crea en tu escritorio un ZIP con la lista de efectos que tu PC ya
  preparó. Si lo envías (o lo adjuntas en un *issue*), se añade a la caché del port y nadie tendrá tirones en esas
  zonas. Solo contiene configuraciones gráficas, nada de tus partidas ni del juego.

## Problemas conocidos
- En el **primer arranque** puede aparecer «Game 1 data is corrupted». Elige **Yes**: se crea la partida y no vuelve a salir.
- Las miniaturas de «Choose Your Picture» se ven vacías en una partida nueva.
- El registro de cada instalación queda en `.build\build_*.log` (botón «Ver registro» del lanzador).

## Créditos
- [WiiCompiled](https://github.com/patchzyy/Wiicompiled) — traductor y runtime de recompilación estática de Wii (GPL-3.0).
- [aurora](https://github.com/encounter/aurora) — GX sobre WebGPU/Dawn (MIT).
- [nod](https://github.com/encounter/nod) — lectura de imágenes de disco de Wii (nodtool).
- [Dolphin](https://github.com/dolphin-emu/dolphin) — fuentes de sistema libres (GPL-2.0-or-later).
- Proyecto de decompilación de PokéPark Wii (SephDB) — mapa de símbolos.

## Aviso legal
Proyecto no oficial y sin ánimo de lucro, sin relación con Nintendo, The Pokémon Company, Creatures ni Game Freak.
*PokéPark*, *Pokémon* y *Wii* son marcas de sus respectivos dueños. No se distribuye ningún archivo, código ni
recurso del juego; cada jugador usa su propia copia legal.

Licencia: **GPL-3.0** (ver `LICENSE`); componentes de terceros en `THIRD_PARTY_NOTICES.md`.

---

## English

A fan-made native PC port of *PokéPark Wii: Pikachu's Adventure* (USA) built with **static recompilation**: the game's
code is translated to C++ and compiled for Windows instead of being emulated.

> **No game files are included.** You need your own copy of *PokéPark Wii: Pikachu's Adventure* (USA, `R8AE01`) as an
> ISO, RVZ, WBFS, WIA or CISO image. The launcher reads it **only on your PC** and builds the port locally.

**Features:** native 60 fps everywhere (the original runs the park at 30) at unchanged game speed · real 16:9 widescreen
· internal resolution up to your display (or 2x–4x) · PC controller/keyboard as a configurable virtual Wii Remote (real
Wii Remotes over Bluetooth should work, untested) · audio, videos, saving, HOME menu · in-game settings with **F10**.

**Requirements:** Windows 10/11 x64, a DirectX 12/Vulkan GPU, ~10 GB free, internet during the first install, and your
PokéPark Wii (USA, Rev 0) disc image.

**How to play:** download the latest `PokePort-vX.Y.Z.zip` from **[Releases](../../releases)** (or Code → Download ZIP), extract it to a simple path (e.g.
`C:\Games\PokePort`), run `PokeParkLauncher.bat`, pick your disc image (or an extracted folder with `sys\main.dol`
and `files\`), press **Install** (15–40 minutes the first time) and then **▶ PLAY**.

**Updates:** the launcher checks GitHub for a new release on start (or with **Check for updates**) and offers **Release
notes / Update / Not now / Skip this version**. Updating verifies the download (SHA-256), backs up the current version
and restores it if anything fails, never touches your saves, settings, extracted disc or toolchain, and rebuilds only
what changed — the previous `PokePark.exe` keeps working until the new one builds.

**Performance:** choose the graphics API (Auto / Direct3D 12 / Vulkan; try Vulkan on GTX 10-series cards) and a lower
resolution on weak GPUs. **Diagnostic mode** (orange button) records performance while you play and shows a summary
(CPU/GPU, work per frame, stutters by cause) plus the file to attach to an issue. A PokéPark shader seed is bundled and compiled
on first start ("Preparing shaders X / Y"); during gameplay few compile threads are used. **Share shader cache** (purple
button) zips the effects your PC prepared so they can be added to the seed for everyone.

**Known issues:** on the very first boot the game may say "Game 1 data is corrupted" — answer **Yes**, it creates the
save and won't ask again. "Choose Your Picture" thumbnails are empty on a new game.

Unofficial, non-commercial project, not affiliated with Nintendo, The Pokémon Company, Creatures or Game Freak. No game
code or assets are distributed. Licensed under **GPL-3.0**; see `THIRD_PARTY_NOTICES.md`.
