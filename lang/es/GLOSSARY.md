# Spanish glossary (lang/es)

Our own translation, made from the English alone (the `en` field of
`src/assets/ntsc-final/lang`, `lang/_source/port.json` and
`lang/_source/ge.json`). No other Perfect Dark or GoldenEye translation was
opened - not Rare's PAL/JPN text, not the `es`/`fr`/`de`/... fields of the ROM
JSON, not another pack. Spanish of Spain, kept neutral enough for Latin
America where that costs nothing. Keep these terms in `pd/*.json`,
`port.json` and `ge.json`.

## Register

- The game speaks to the player with **tú** (menus, help, training, HUD,
  objectives): "Pulsa", "Selecciona", "Recoge".
- **Carrington -> Joanna: tú** (paternal, he calls her Joanna).
  **Joanna -> Carrington: usted** ("señor").
- **Joanna <-> Elvis, Jonathan, Velvet, Grimshaw, Foster, Institute staff: tú.**
- **The President <-> Joanna: usted** both ways; his aides and the NSA
  director to the President: usted.
- Villains to Joanna (Cassandra, Trent, Mr. Blonde, guards, the Skedar):
  **tú** (contempt). Trent <-> Cassandra: tú. Mr. Blonde -> Trent/Cassandra:
  usted (cold, formal).
- Guards among themselves: tú.
- A plural "you" is avoided where a rephrasing is easy; where it is not,
  **vosotros** (Spain).
- Words that read badly in Latin America are avoided: no "coger" (use
  "recoger", "tomar", "agarrar"), no "tío/guay/vale" in narration (a guard's
  "¡Vale!" is fine). "Ordenador" for a computer.
- Objective list lines are infinitives: "Desactivar el sistema de seguridad".
  HUD instructions are imperatives in tú: "Recoge el disco".

## Typography

- Opening ¿ and ¡ always. Quotes « » (or "..." ASCII where a string already
  has them in the English and they are code-like).
- `...` three ASCII dots, as the English writes them.
- Capitals keep their accents (Á É Í Ó Ú Ñ): the font composes them.
- Strings the English writes in ALL CAPS stay ALL CAPS.
- Every control sequence as the English has it: each `\n` (count and place,
  the trailing one too), `|` briefing markers, printf conversions (%s %d
  %02d ... in the same order and number), `\0` (the hangar bios'
  "name\0|subheading": keep the NUL between the two halves), leading and
  trailing spaces.
- Time: "02:30", dates "15 AGO 2023"; "s" for seconds on the HUD.

## Names kept as they are

Joanna Dark, Jo, Daniel Carrington, Cassandra De Vries, Trent Easton,
Jonathan, Elvis, Mr. Blonde, Dr. Caroll ("el Dr. Caroll" in prose), Velvet
Dark, Grimshaw, Foster, dataDyne (no article: "dataDyne sabe..."; "la
empresa dataDyne" when one is needed), G5, NSA, CIA, FBI, Air Force One,
Pelagic II, Skedar ("los Skedar", adjective "skedar": "una nave skedar"),
Maian ("los maians", adjective "maian": "la nave maian"), Cetan ("la nave
cetan"), CamSpy, DrugSpy, BombSpy, Holo, Perfect Dark, Combat Boost (a
product name: "Combat Boosts" in the plural), and the sims' names (MeatSim,
EasySim ... DarkSim, PeaceSim ...). GoldenEye: James Bond, 007, M, Q,
Moneypenny, Alec Trevelyan, 006, Ourumov, Natalya Simonova, Boris
Grishenko, Xenia Onatopp, Valentin Zukovsky, Jaws, Baron Samedi, Oddjob,
May Day, Janus, Severnaya, GoldenEye (the satellite and the key), MI6, KGB,
Arkhangelsk, Kirghizstan, Monte Carlo, St. Petersburg (San Petersburgo),
Cuba.

## Places and levels (Perfect Dark)

| English | Spanish |
|---|---|
| Carrington Institute (CI) | Instituto Carrington (IC) |
| dataDyne Central / dataDyne Research / dataDyne Extraction | Central de dataDyne / Laboratorios dataDyne / dataDyne (Extracción) |
| Lucerne Tower | Torre Lucerne |
| Laboratory Basement | Sótano de laboratorios |
| Carrington Villa | Villa Carrington |
| Chicago | Chicago |
| G5 Building | Edificio G5 |
| Area 51 / A51 | Área 51 / A51 |
| Air Base / Alaskan Air Base | Base aérea / Base aérea de Alaska |
| Air Force One | Air Force One |
| Crash Site | Lugar del accidente |
| Pelagic II | Pelagic II |
| Deep Sea | Fondo marino |
| Attack Ship / Skedar assault ship | Nave de ataque / nave de asalto skedar |
| Skedar Ruins | Ruinas Skedar |
| Mission subtitles | Deserción, Investigación, Extracción, Rehén Uno, Sigilo, Reconocimiento, Infiltración, Rescate, Huida, Espionaje, Antiterrorismo, Confrontación, Exploración, Neutralizar amenaza, Defensa, Asalto encubierto, Santuario de batalla |
| Special assignments | La venganza de Mr. Blonde, SOS maian, Recuperar el Instituto, ¡GUERRA!, El duelo |
| Firing Range | Galería de tiro |
| Device Lab / Holo room / Info room / Hangar | Laboratorio de artefactos / Sala holo / Sala de información / Hangar |

## Modes and menus

| English | Spanish |
|---|---|
| Perfect Menu | Menú Perfect |
| Solo Missions | Misiones en solitario |
| Combat Simulator | Simulador de combate |
| Co-Operative / Counter-Operative | Cooperativo / Contraoperativo |
| Simulant | Simulante (sims keep their English names) |
| Agent / Special Agent / Perfect Agent | Agente / Agente Especial / Agente Perfecto |
| Briefing | Informe |
| Objective / Completed / Incomplete / Failed | Objetivo / Completado / Incompleto / Fallido |
| Challenge | Desafío |
| Cheats | Trucos |
| Controller / Control Stick / Control Pad | Mando / Palanca de control / Cruceta |
| Z Button, B Button, R Button, C Buttons, Up C Button | Botón Z, Botón B, Botón R, Botones C, Botón C arriba (abajo, izquierda, derecha) |
| Controller Pak / Game Pak / Transfer Pak / Rumble Pak | kept |
| King of the Hill / Hold the Briefcase / Capture the Case / Hacker Central / Pop a Cap | Rey de la colina / Guarda el maletín / Captura el maletín / Central hacker / Caza al blanco |
| Perfect Buddy | Compañero Perfect |
| Game file / Save | Partida / Guardar |

## Weapons and gadgets

Brand names stay: Falcon 2, MagSec 4, Mauler, Phoenix, DY357 Magnum,
DY357-LX, CMP150, Cyclone, Callisto NTG, RC-P120, Dragon, SuperDragon, K7
Avenger, AR34, Reaper, Devastator, Slayer, FarSight XR-20, CamSpy, and
GoldenEye's PP7, DD44 Dostovei, Klobb, KF7 Soviet, ZMG (9mm), D5K
Deutsche, Phantom, AR33 Assault Rifle (Rifle de asalto AR33), RC-P90,
Moonraker Laser (Láser Moonraker), Golden Gun (Pistola de oro), Silver PP7
(PP7 de plata), Gold PP7 (PP7 de oro).

| English | Spanish |
|---|---|
| Unarmed | Desarmado |
| Laptop Gun | Arma portátil |
| Shotgun | Escopeta |
| Rocket Launcher / R-Launcher | Lanzacohetes |
| Grenade Launcher | Lanzagranadas |
| Sniper Rifle | Rifle de francotirador (inventory: "Rifle francotirador") |
| Crossbow | Ballesta |
| Tranquilizer | Tranquilizante |
| Combat Knife / Throwing Knife / Hunting Knife | Cuchillo de combate / Cuchillo arrojadizo / Cuchillo de caza |
| Psychosis Gun | Pistola psicosis |
| Grenade / MaianGrenade / N-Bomb | Granada / Granada maian / Bomba N |
| Timed / Proximity / Remote / ECM Mine | Mina temporizada / Mina de proximidad / Mina remota / Mina ECM |
| Night Vision / X-Ray Scanner / IR Scanner | Visión nocturna / Escáner de rayos X / Escáner IR |
| Door Decoder | Descodificador |
| Data Uplink | Enlace de datos |
| R-Tracker / Tracker | Rastreador R / rastreador |
| Tracer Bug | Baliza rastreadora |
| Cloaking Device | Camuflaje óptico |
| Shield | Escudo |
| Disguise | Disfraz |
| Key card / Necklace | Tarjeta de acceso / Collar |
| Explosives | Explosivos |
| Horizon Scanner | Escáner Horizon |
| Suitcase / Briefcase | Maleta / Maletín |
| Backup Disk | Disco de respaldo |
| AutoSurgeon / Alien Medpack | AutoCirujano / Botiquín alienígena |
| Target Amplifier | Amplificador de blanco (inventory: "Amplif. de blanco") |
| Comms Rider / comms hub | Relé de comms / centro de comunicaciones |
| secondary function / mode | función secundaria / modo |
| magazine / ammo | cargador / munición |
| rpm | dpm (disparos por minuto) |
| Watch (GoldenEye) / Watch Laser | Reloj / Láser del reloj |

## Characters' titles and people

| English | Spanish |
|---|---|
| The President | el presidente |
| NSA director | el director de la NSA |
| Protector (Maian bodyguard) | Protector |
| Skedar King / Leader | rey skedar / líder skedar |
| guard / trooper / shock trooper | guardia / soldado / tropa de choque |
| lab technician / scientist | técnico de laboratorio / científico |
| hacker | hacker (pl. hackers) |
| Greys | los Grises |

## Briefings, HUD and objectives

| English | Spanish |
|---|---|
| `\|Background - ` | `\|Antecedentes - ` |
| `\|Carrington - ` | `\|Carrington - ` |
| `\|Objective One: - X` | `\|Objetivo uno: - X` (dos, tres, cuatro, cinco) |
| `END` | `FIN` |
| `\|Profile -` / `\|Updated Profile -` / `\|Analyst note -` | `\|Perfil -` / `\|Perfil actualizado -` / `\|Nota del analista -` |
| `\|CI File #027 -` | `\|Archivo IC n.º 027 -` |
| "Obtain X." (HUD) | "Consigue X." |
| "Picked up X." | "Recogido: X." |
| "Critical mission object destroyed" | "Objeto crítico para la misión destruido" |
| "Critical mission personnel killed" | "Personal crítico para la misión eliminado" |
| owner labels ("Dr. Caroll's", "Guard's") | bare name: "Dr. Caroll", "Guardia" |
| Description / Training Instructions / Operation | Descripción / Instrucciones de entrenamiento / Uso |

## Pickup messages (propobj)

Perfect Dark builds "Picked up " + "a " + name + "." (full screen) and "A " +
name + "." (split screen) from pieces, and a pack cannot leave a piece empty
(an empty string falls back to English). So: "Picked up " = "Recogido",
the full-screen determiners (a, an, some, the) = ": ", "your " = ": tu ",
giving "Recogido: granada."; the split-screen determiners are "+1 " (A, An,
The), "+ " (Some) and "Tu " (Your): "+1 granada.". "Double " = "Doble: ".
The game appends the plural "s" to an ammo name, so every ammo name ends on
a bare noun that takes a plain "s" (cartucho, bala, granada, cohete, mina,
virote, orbe, dardo, bomba, píldora, micrófono, microcámara, ficha,
explosivo); "combat " + "knife"/"knives" is built "cuchillo" + " de
combate" / "s de combate".

## Lengths

- Inventory names 19 characters or fewer.
- Team names (L_OPTIONS_008-015) 11 bytes or fewer; "Player" (L_MISC_437) 10.
- Menu labels do not wrap: keep them within about 1.3x the English. Drop
  articles, use short synonyms: "Opciones partida", "Añadir simulante",
  "Borrar partida".

## More Perfect Dark terms (chosen while translating the levels)

| English | Spanish |
|---|---|
| hovercab / cab | aerotaxi / taxi |
| hovercopter | aerocóptero |
| hoverbike / jetbike / HovBike | aeromoto |
| Hovercrate | caja flotante |
| hover trolley / hoverbed | carretilla flotante / camilla flotante |
| gunship / jumpship / dropship | cañonera / lanzadera / nave de transporte |
| sapient (Dr. Caroll's AI) | el sapiente |
| Reprogrammer | Reprogramador |
| foyer / helipad / storm drain | vestíbulo / helipuerto / alcantarilla |
| Moon Pool | piscina lunar |
| escape pod / distress beacon | cápsula de escape / baliza de socorro |
| jamming device | inhibidor |
| laser grid | rejilla láser |
| Teleportals | teleportales |
| Inner Sanctum | sanctasanctórum |
| Maian High Command | Alto Mando maian |
| UFO | OVNI |
| Holoprogram / holograph (verb) | Holoprograma / holografiar |
| Regicide Part One | Regicidio, primera parte |
| MISSION SUCCESSFUL / MISSION FAILED | MISIÓN CUMPLIDA / MISIÓN FALLIDA |
| Lift / Office key card | Tarjeta de ascensor / Tarjeta de oficina |
| Door Exploder | revientapuertas |

Register notes from the levels: Dr. Caroll and Joanna use usted both
ways (he is old-fashioned and formal); the President and Trent use usted.

## GoldenEye 007 (ge.json)

The game speaks to the player with **tú**, as above. M, Q and the
briefings speak to 007 with **usted**; Moneypenny <-> James: tú; Bond <->
Natalya, Trevelyan, Boris, Ourumov, Xenia: tú. Strings GoldenEye writes all
in lower case (its second, lower-case copy of each briefing) stay all lower
case; spaced-out titles ("B Y E L O M O R Y E  D A M") stay spaced.
"Teotihuaca'n" is GoldenEye's way of writing an accent: write "Teotihuacán".

| English | Spanish |
|---|---|
| Arkangelsk / Dam / Facility / Runway | Arkangelsk / Presa / Instalaciones / Pista |
| Severnaya / Surface / Bunker | Severnaya / Superficie / Búnker |
| Kirghizstan / Launch Silo #4 / Silo | Kirguistán / Silo de lanzamiento n.º 4 / Silo |
| Monte Carlo / Frigate | Montecarlo / Fragata |
| St. Petersburg / Statue Park / Statue | San Petersburgo / Parque de las estatuas / Estatuas |
| Military Archives / Archives / Streets / Depot / Train | Archivos militares / Archivos / Calles / Depósito / Tren |
| Cuba / Jungle / Control Center / Control | Cuba / Jungla / Centro de control / Control |
| Water Caverns / Caverns / Antenna Cradle / Cradle | Cavernas acuáticas / Cavernas / Cuna de la antena / Cuna |
| Aztec Complex / Aztec / Egyptian Temple / Egyptian | Complejo azteca / Azteca / Templo egipcio / Egipcio |
| Agent / Secret Agent / 00 Agent / 007 | Agente / Agente secreto / Agente 00 / 007 |
| M / Q / Q Branch / Moneypenny | M / Q / la sección Q / Moneypenny |
| Janus / Janus Syndicate | Janus / el sindicato Janus |
| Mission Status / Objectives | Estado de la misión / Objetivos |
| PP7 (silenced) / DD44 Dostovei / Klobb / KF7 Soviet / ZMG (9mm) / D5K Deutsche / Phantom / RC-P90 | PP7 (silenciada) / DD44 Dostovei / Klobb / KF7 Soviet / ZMG (9mm) / D5K Deutsche / Phantom / RC-P90 |
| AR33 Assault Rifle / Sniper Rifle / Shotgun / Automatic Shotgun | Rifle de asalto AR33 / Rifle de francotirador / Escopeta / Escopeta automática |
| Cougar Magnum / Golden Gun / Silver PP7 / Gold PP7 | Magnum Cougar / Pistola de oro / PP7 de plata / PP7 de oro |
| Moonraker Laser / Watch Laser | Láser Moonraker / Láser del reloj |
| Hand Grenade / Grenade Launcher / Rocket Launcher | Granada de mano / Lanzagranadas / Lanzacohetes |
| Timed / Proximity / Remote Mine / Detonator | Mina temporizada / Mina de proximidad / Mina remota / Detonador |
| Throwing Knife / Hunting Knife / Slappers / Taser / Tank | Cuchillo arrojadizo / Cuchillo de caza / Manos / Táser / Tanque |
| Plastique / Covert Modem / Datathief / Bomb Defuser | Explosivo plástico / Módem encubierto / Ladrón de datos / Desactivador de bombas |
| Key Analyzer / Door Decoder / Camera / Bug | Analizador de llaves / Descodificador / Cámara / Micrófono |
| Body Armor / Watch / Watch Magnet | Blindaje / Reloj / Imán del reloj |
| bungee jump | salto elástico |
| nerve gas / chemical warfare facility | gas nervioso / planta de armas químicas |
| GoldenEye (the satellite weapon, the key) | GoldenEye (el arma GoldenEye, la llave GoldenEye) |
| keycard / safe / flight recorder | tarjeta / caja fuerte / caja negra |
| mainframe / circuit board | ordenador central / placa de circuitos |
| gas tanks (Facility) | depósitos (never "tanques": that is the Tank) |
| Pirate helicopter | helicóptero Pirate |
| "Picked up X" (GE HUD) | "Recogido: X." as in Perfect Dark |
| Mission Failure / civilian casualties | Misión fallida / víctimas civiles |

GoldenEye's film titles as scenario names use the Spanish release titles:
Solo se vive dos veces, Alta tensión, El hombre de la pistola de oro,
Licencia para matar.

## The port's own strings (port.json)

| English | Spanish |
|---|---|
| Customize Character | Editar personaje |
| Setup (MP settings file) | configuración ("config." in short labels) |
| Settings Preset / Camera Preset | Perfil de ajustes / Preajuste cámara |
| Custom / Default | A medida / Estándar |
| Vanilla / Reset to Stock | Original / Valores de serie |
| On / Off as option values | Sí / No; "X On/Off" toggles "X: sí" / "X: no" |
| Ghost Trials | Ghost Trials (the mode's name); a ghost = fantasma; Ghost Time Trial = Contrarreloj fantasma; My Ghosts = Mis fantasmas |
| run (ghost race / randomizer) | carrera / partida |
| Sign In / Create Account | Entrar / Crear cuenta |
| Offline / Online | Local / En línea |
| Leaderboards | Clasificaciones |
| Crash report / Report a Problem | informe de fallo / Informar problema |
| Texture Pack / Model Pack / Community Packs | Pack texturas / Pack modelos / Packs de la comunidad |
| Stage Loader / stage | Cargador fases / fase |
| Randomizer / seed | Aleatorizador / semilla |
| Akimbo | Dos armas |
| Combat Roll / Melee Combos / Aim Lock | Voltereta / Combos melé / Fijar mira |
| crosshair | mira |
| Renderer / Upscaling / Supersampling | Render / Reescalado / Supermuestreo |
| Tracker Bug (GE gadget) | Micro rastreador |
| US / Japan (region) | EE.UU. / Japón |

"golden bullet" (the Golden Gun's ammunition, a port string) is "bala": the
game appends the plural "s", so "bala de oro" would come out "bala de oros".

## The length audit

`pd --lang es --lang-audit` (2026-09-29) lists 649 strings over 1.3x their
English in the small font. Nearly all are objectives, HUD messages and
dialogue (which wrap) or short words against shorter English ("OK" ->
"Aceptar", "Back" -> "Volver", "Cancel" -> "Cancelar"), which fit their
boxes. Labels that ran into a value column or the dialog's edge were
shortened after screenshots ("Zona muerta mira", "Tamaño mira", "Editar
personaje", "Balanceo", "Autoapuntado", "Formato").
