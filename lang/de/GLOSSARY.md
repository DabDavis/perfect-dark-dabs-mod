# German glossary (lang/de)

Our own translation, made from the English alone. No existing German text of
Perfect Dark or GoldenEye 007 (Rare's PAL German included) was opened or
consulted. Keep these terms when translating the port's strings (`port.json`)
and GE Plus (`ge.json`), and when editing any bank.

## Register (Sie / du)

- The game speaks to the player with **Sie**: menus, help texts, training
  instructions, HUD orders ("Beschaffen Sie die Diskette."), options.
- **Carrington <-> Joanna: Sie** both ways (employer, formal and fatherly:
  "Joanna, Sie müssen ...").
- **Joanna <-> Elvis, Jonathan, Velvet, Grimshaw, Foster, CI staff, Mr.
  Blonde's Skedar at the end: du.** Allies in the field say du to each other.
- Guards and soldiers among themselves: du ("Hast du das gehört?").
- Villains to Joanna: Cassandra De Vries and Trent Easton use **Sie** (cold,
  haughty); Mr. Blonde, guards, thugs and the Skedar use **du**.
- The President <-> Joanna: Sie. Mission control/briefing voices: Sie.
- GoldenEye (ge.json): M, Q, Moneypenny, Bond's briefings: **Sie** to Bond
  ("007, Sie ..."); Bond <-> Natalya: du after they meet; villains to Bond:
  Sie (Trevelyan and Ourumov may say du in taunts, but stay consistent per
  speaker within one level). Menus and the watch: Sie.

## Typography

- Quotes: » « (guillemets pointing inwards, German style) or plain "...".
  Never the low-high German quotes (kept out of the packs by choice).
- `'` ASCII apostrophe; `...` three dots, never the ellipsis character.
- Only Latin-1 letters: ä ö ü Ä Ö Ü ß é etc. No en/em dashes: use `-`.
- Strings the English writes in ALL CAPS stay ALL CAPS, umlauts as Ä Ö Ü;
  ß becomes SS in capitals ("GRÖSSE").
- Keep every `\n` (count and trailing one), `|` briefing markers, `\0`
  separators (`Carrington-Institut\0|Operationsbasis\n`), printf conversions
  in order, leading/trailing spaces.
- Numbers: decimal comma only where the English writes a decimal in prose;
  times "02:30", dates "15. AUGUST 2023" in caps where the English is caps.

## Names kept as they are

Joanna Dark, Jo, Daniel Carrington, Cassandra De Vries, Trent Easton,
Jonathan, Elvis, Mr. Blonde, Dr. Caroll, Velvet Dark, Grimshaw, Foster,
dataDyne (no article in running text where possible: "bei dataDyne", "die
dataDyne-Zentrale"), G5, NSA, CIA, FBI, Air Force One, Pelagic II, Skedar
(plural "die Skedar", adjective in compounds: "Skedar-Schiff"), Maian ("die
Maians", "Maian-Schiff"), Cetan ("das Cetan-Schiff"), CamSpy, DrugSpy,
BombSpy, Perfect Dark, Lucerne Tower, Chicago, Area 51 / A51.
GoldenEye: James Bond, 007, M, Q, Moneypenny, Alec Trevelyan (006), Ourumov,
Natalya Simonova, Boris Grishenko, Xenia Onatopp, Valentin Zukovsky, Jaws,
Baron Samedi, Oddjob, Mayday, Janus, GoldenEye, Severnaya, Arkhangelsk
(Archangelsk in prose), Monte Carlo, MI6, KGB, Tiger helicopter.

## Places and levels

| English | German |
|---|---|
| Carrington Institute (CI) | Carrington-Institut (CI) |
| dataDyne Central / dD Central | dataDyne-Zentrale / dD-Zentrale |
| dataDyne Research / dD Research | dataDyne-Forschung / dD-Forschung |
| dD Extraction | dD-Extraktion |
| Carrington Villa | Carrington-Villa |
| G5 Building | G5-Gebäude |
| Area 51 / A51 Infiltration, Rescue, Escape | Area 51 / A51-Infiltration, A51-Rettung, A51-Flucht |
| Air Base / Alaskan Air Base | Luftbasis / Luftbasis in Alaska |
| Crash Site | Absturzstelle |
| Deep Sea | Tiefsee |
| Institute Defense | Institutsabwehr |
| Attack Ship / Skedar Assault Ship | Angriffsschiff / Skedar-Angriffsschiff |
| Skedar Ruins / Skedar Homeworld | Skedar-Ruinen / Skedar-Heimatwelt |
| Battle Shrine | Kampfschrein |
| Mission subtitles | Überlaufen (Defection), Ermittlung, Extraktion, Geisel Eins (Hostage One), Tarnung (Stealth), Aufklärung (Reconnaissance), Infiltration, Rettung, Flucht, Spionage, Terrorabwehr, Konfrontation, Erkundung, Bedrohung beseitigen, Verteidigung, Verdeckter Angriff, Kampfschrein |
| Special assignments | Mr. Blondes Rache, Maian-SOS, Rückeroberung des Instituts (menu: Institut zurückerobern), KRIEG!, Das Duell |
| Firing Range | Schießstand |
| Device Lab / Holo room / Info room | Gerätelabor / Holoraum / Inforaum |
| Training | Training |
| Laboratory Basement | Laborkeller |
| End Credits | Abspann |

## Modes and menus

| English | German |
|---|---|
| Perfect Menu | Perfect-Menü |
| Solo Missions / Mission | Solo-Missionen / Mission |
| Combat Simulator (Combat Sim) | Kampfsimulator (Kampfsim) |
| Co-Operative / Counter-Operative | Kooperativ / Konter-Operativ |
| Simulant(s), Sim | Simulant(en), Sim; sim type names stay English (MeatSim, DarkSim ...) |
| Agent / Special Agent / Perfect Agent | Agent / Spezialagent / Perfect Agent |
| Easy / Normal / Hard / Perfect / Dark (sim) | Leicht / Normal / Schwer / Perfekt / Dark |
| Briefing | Briefing |
| Objective / Completed / Incomplete / Failed | Ziel / Erfüllt / Unerfüllt / Gescheitert |
| Challenge(s) | Challenge(s) (kept, it is short; prose may say Herausforderung) |
| Controller Pak / Game Pak / Transfer Pak / Rumble Pak | kept |
| Control Stick / Control Pad | Control Stick / Steuerkreuz |
| Z Button, B Button, R Button, C Buttons, Up C Button | Z-Taste, B-Taste, R-Taste, C-Tasten, C-oben-Taste (C-unten, C-links, C-rechts) |
| START | START |
| Controller | Controller |
| Player / Player 1 | Spieler / Spieler 1 |
| Team | Team |
| Settings / Options | Einstellungen / Optionen |
| Load / Save / Delete / Copy | Laden / Speichern / Löschen / Kopieren |
| Back / Cancel / OK / Yes / No | Zurück / Abbrechen / OK / Ja / Nein |
| Cheat(s) | Cheat(s); "Cheated" = "Gecheatet" |
| Perfect Buddy | Perfect Buddy |
| Scenarios | Kampf (Combat), Koffer halten (Hold the Briefcase), Hackerzentrale (Hacker Central), Kopfgeld (Pop a Cap), King of the Hill (Hügelkönig in prose), Koffer erobern (Capture the Case) |
| Combat Sim ranks | Anfänger, Lehrling, Amateur, Rekrut, Neuling, Soldat, Agent, Staragent, Spezialagent, Experte, Veteran, Profi, Gefährlich, Tödlich, Killer, Assassine, Todbringend (Lethal), Elite, Unbesiegbar, Fast perfekt, Perfekt |

## Weapons and gadgets

Brand names stay: Falcon 2, MagSec 4, Mauler, Phoenix, DY357 Magnum,
DY357-LX, CMP150, Cyclone, MagSec SMG, Callisto NTG, RC-P120, Laptop Gun,
Dragon, SuperDragon, K7 Avenger, AR34, Reaper, Devastator, Slayer, FarSight
XR-20, PP9i, CC13, KL01313, KF7 Special, ZZT (9mm), DMC, AR53, RC-P45,
R-Tracker. GoldenEye: PP7, DD44 Dostovei, Klobb, KF7 Soviet, ZMG (9mm), D5K
Deutsche, Phantom, AR33, RC-P90, Cougar Magnum, Moonraker-Laser.

| English | German |
|---|---|
| Unarmed | Unbewaffnet |
| Falcon 2 (silencer) / (scope) | Falcon 2 (Dämpfer) / Falcon 2 (Optik) |
| silencer / silenced | Schalldämpfer / schallgedämpft |
| Shotgun / Automatic Shotgun | Schrotflinte / Automatik-Schrotflinte |
| Rocket Launcher / R-Launcher | Raketenwerfer / R-Werfer |
| Grenade Launcher | Granatwerfer |
| Sniper Rifle | Präzisionsgewehr |
| Assault Rifle | Sturmgewehr |
| Crossbow / Bolt | Armbrust / Bolzen |
| Tranquilizer | Betäubungspistole |
| Psychosis Gun | Psychose-Pistole |
| Combat Knife / Knife / Throwing Knife / Hunting Knife | Kampfmesser / Messer / Wurfmesser / Jagdmesser |
| Grenade / MaianGrenade / FlashBang | Granate / Maian-Granate / Blendgranate |
| N-Bomb | N-Bombe |
| Timed / Proximity / Remote / ECM Mine | Zeitzündermine / Näherungsmine / Fernzündmine / ECM-Mine |
| Rocket / Homing Rocket / Grenade Round | Rakete / Lenkrakete / Granatpatrone |
| Night Vision | Nachtsichtgerät |
| X-Ray Scanner / IR Scanner / Horizon Scanner | Röntgenscanner / IR-Scanner / Horizont-Scanner |
| Door Decoder | Türdecoder |
| Explosives | Sprengstoff |
| Data Uplink | Daten-Uplink |
| Cloaking Device / Cloak | Tarngerät / Tarnung |
| Combat Boost / Boost | Kampf-Boost / Boost |
| Shield | Schild |
| Disguise | Verkleidung |
| Suicide Pill | Selbstmordpille |
| Alien Medpack | Alien-Medipack |
| Suitcase / Briefcase | Koffer / Aktenkoffer |
| Necklace / Key Card / Keycard | Halskette / Schlüsselkarte |
| Backup Disk | Sicherungsdisk |
| Research Tape / Flight Plans | Forschungsband / Flugpläne |
| Tracer Bug | Peilsender |
| Comms Rider | Funk-Rider |
| Target Amplifier / Target Amp | Zielverstärker |
| AutoSurgeon | AutoChirurg |
| President Scanner | Präsidentenscanner |
| Skedar Bomb | Skedar-Bombe |
| Body Armor | Schutzweste |
| Golden Gun / Silver PP7 / Gold PP7 | Goldene Waffe / Silberne PP7 / Goldene PP7 |
| Watch Laser / Tazer | Uhrenlaser / Taser |
| Plastique | Plastiksprengstoff |
| magazine / ammo / rounds | Magazin / Munition / Schuss |
| primary / secondary function | Primärfunktion / Sekundärfunktion |
| rpm | Schuss/min |

## Characters and people

| English | German |
|---|---|
| The President | der Präsident |
| NSA director | NSA-Direktor |
| Protector (Maian bodyguard) | Beschützer |
| Skedar King | Skedar-König |
| Ambassador | Botschafter |
| guard / trooper / shock trooper | Wache / Soldat / Stoßtruppler |
| lab technician / scientist | Labortechniker / Wissenschaftler |
| sapient (Dr. Caroll) | die Sapient-KI / das Sapient |
| Greys | die »Grauen« |
| hackers | Hacker |

## Briefings, HUD and objectives

| English | German |
|---|---|
| `\|Background - ` | `\|Hintergrund - ` |
| `\|Carrington - ` | `\|Carrington - ` |
| `\|Objective One: - X` | `\|Ziel eins: - X` (zwei, drei, vier, fünf) |
| `END` | `ENDE` |
| `\|Profile -` / `\|Updated Profile -` / `\|Analyst note -` | `\|Profil -` / `\|Aktualisiertes Profil -` / `\|Anmerkung des Analysten -` |
| `\|CI File #027 -` | `\|CI-Akte Nr. 027 -` |
| objective list lines ("Disable the hub") | infinitive: "Relais deaktivieren" |
| HUD orders ("Obtain X.") | Sie-imperative: "Beschaffen Sie X." |
| "X has been destroyed/killed" | "X wurde zerstört/getötet." |
| "Critical mission object destroyed" | "Missionskritisches Objekt zerstört." |
| "Critical mission personnel killed" | "Missionskritische Person getötet." |
| Description / Training Instructions / Operation | Beschreibung / Trainingsanleitung / Bedienung |
| owner labels ("Dr. Caroll's", "Guard's") | bare name: "Dr. Caroll", "Wache" |
| Holo 1 - Looking Around | Holo 1 - Umsehen |

## Pickup messages (propobj)

Perfect Dark builds "Picked up " + "a " + name + "." from pieces (propobj.c,
`ammotypeGetPickupMessage()` / the weapon pickup), and an empty piece falls
back to English, so:

- `Picked up ` -> `Erhalten` (no trailing space: the next piece brings the
  colon), and the lower-case determiners `a `, `an `, `some `, `the `,
  `your ` -> `: `  => "Erhalten: Granate." / "Erhalten: Falcon 2."
- the capitalised determiners (split screen, no "Picked up") `A `, `An `,
  `Some `, `The `, `Your ` -> `Neu: ` => "Neu: Granaten."
- `Double ` -> `Doppelt: ` => "Doppelt: Falcon 2."
- the plural piece `s` -> `n`, so every countable ammo name ends in -e or
  -el: Schrotpatrone, Magnum-Kugel, Granate, Granatpatrone, Rakete,
  Lenkrakete, Fernzündmine, Näherungsmine, Zeitzündermine, Bolzenpatrone
  (crossbow ammo), Energiekugel (FarSight orb), N-Bombe, Wanze, Mikrokamera,
  Marke (token), Plastikbombe, Boost-Pille, goldene Kugel. Uncounted:
  Munition (ammo), Beruhigungsmittel (sedatives), Reaper-Munition, Tarngerät.
- knives: `combat ` + `knife`/`knives` -> `Kampf` + `messer`/`messer`
  (GoldenEye: `throwing ` -> `Wurf`).
- The weapon pickup appends a hard "s" for Combat Boost ("Kampf-Boosts").
- GoldenEye's own `a PP7.\n` rows follow "Picked up " -> `: PP7.\n`.

## Lengths

- Inventory names 19 characters or fewer (the list cuts at about 20):
  "Falcon 2 (Dämpfer)", "Präsidentenscanner", "Sicherungsdisk".
- Team names 11 bytes or fewer; "Player" = "Spieler" (10 max).
- Menu labels about 1.3x the English at most; drop articles first, then
  use a shorter synonym, abbreviate only as a last resort ("Einst.").
- Controller diagram: LOOK = BLICK, STRAFE = SEITW., WALK = GEHEN, FIRE = FEUER,
  AIM = ZIELEN, RELOAD = NACHLADEN.

## Register, as settled while translating

- Scientists, executives and technicians <-> Joanna: Sie both ways. Dr.
  Caroll <-> Joanna: Sie. Grimshaw and the CI training staff -> Joanna: du;
  Carrington's tour stays Sie. Elvis's own briefing sections: du; the
  objective paragraphs under them keep the briefing's Sie.
- Villains among themselves: Mr. Blonde says du to Cassandra and Trent; they
  say Sie to him and to each other (Trent, angry: "du skandinavischer Freak").
- GoldenEye: Trevelyan (also as Janus), Natalya (after the escape) and Jack
  Wade say du to Bond; Ourumov, Mishkin, Valentin, Boris, Xenia, guards, M, Q
  and Moneypenny say Sie.
- Multiplayer HUD lines avoid the pronoun where they can ("Ein Punkt fürs
  Überleben!"); where one is needed it is Sie.

## More terms

| English | German |
|---|---|
| Jumpship / dropship | Sprungschiff / Landungsschiff |
| Hovercopter / gunship | Hoverkopter / Kampfhubschrauber |
| HoverBike / HoverCrate / hover trolley / hoverbed / hovercab | Hoverbike / Schwebekiste / Schwebewagen / Schwebebahre / Schwebetaxi |
| Cleaning Hovbot | Putz-Hovbot |
| (comms) hub | Relais |
| communications bug | Abhörwanze |
| distress beacon / escape pod, capsule | Notsender / Rettungskapsel |
| Presidential clone | Präsidentenklon |
| laser grid / damping field generator | Lasergitter / Dämpfungsfeldgenerator |
| Door Exploder / Detonator | Türsprenger / Zünder |
| Moon Pool / deep submersible | Moonpool / Tiefseetauchboot |
| key cards (level 1, office, lift, medlab 2, op room) | Ebene-1-Karte, Büro-Schlüsselkarte, Liftkarte, Medlabor-2-Karte, OP-Saal-Karte |
| hit squad / remote comlink | Killerkommando / Fern-Comlink |
| holograph (verb) | holografieren |
| air intercept radar / robot interceptor | Abfangradar / Abfangroboter |
| containment unit / lab | Aufbewahrungseinheit / Isolierlabor |
| reprogrammer / drop point / adrenaline pill | Umprogrammierer / Ablageort / Adrenalinpille |
| Teleportals / Inner Sanctum | Teleportale / Allerheiligstes |
| Megaweapon / sapient shell | Megawaffe / Sapient-Hülle |
| umbilical (AF1 to the UFO) | Verbindungsschlauch |
| Regicide Part One | Königsmord, Teil eins |
| Maian High Command / saucer | Maian-Oberkommando / Maian-Untertasse |
| Skedar Leader | Skedar-Anführer |
| Team names (Red, Yellow ...) | Rot, Gelb, Blau, Magenta, Cyan, Orange, Pink, Braun |
| Ghost Trials / ghost (a recorded run) | Ghost Trials / Geist |
| Texture Pack / Model Pack / Community Packs | Texturpaket / Modellpaket / Community-Pakete |
| Load Mods / Stage Loader | Mods laden / Stage-Loader |
| Randomizer / run / room | Randomizer / Lauf / Raum |
| Report a Problem / crash report | Problem melden / Absturzbericht |
| Customize Character | Figur anpassen |
| GoldenEye gadgets | Hakenpistole (Piton Gun), Leuchtpistole, Türsprenger/Schlosssprenger, Zylinderschlüssel/Riegelschlüssel, Safeknacker, Uhr-Kommunikator, Uhr-Geigerzähler, Uhr-Identifikator, Uhrenmagnet (Anziehen/Abstoßen), Militärlaser, Datendieb, Schlüsselanalysator, Bombenentschärfer, Verdecktes Modem, Panzer (Tank) |
| GoldenEye "(silenced)" | "(gedämpft)" in names (PP7, D5K), "schallgedämpft" in prose |
| GoldenEye Q BRANCH / Novice, Rookie, Hero | ABTEILUNG Q / Anfänger, Neuling, Held |
| GoldenEye multiplayer scenarios | the German film titles: Man lebt nur zweimal, Der Hauch des Todes, Der Mann mit dem goldenen Colt, Lizenz zum Töten |
| Spetznaz / Politburo | Speznas / Politbüro |

GoldenEye's levels (the mission grid holds about nine capitals a picture):
Staudamm, Anlage, Startbahn, Gelände (Surface), Bunker, Silo, Fregatte,
Statue, Archiv, Straßen, Depot, Zug, Dschungel, Zentrale (Control), Höhlen,
Wiege, Azteken, Ägypten; the long forms (Kontrollzentrum, Wasserhöhlen,
Antennenwiege ...) stay in the briefings.

The port's options pages put a slider or value at mid-row, so their labels
stay near the English's length: Crosshair ... = Visier ... (Visiergröße,
Visier-Schwanken), Simulant Skill = Sim-Können.

GoldenEye's watch splits two-line gun names at the English's own break
(Präzisions- / gewehr); "Grenade" is both the hand grenade and the top half
of "Grenade Launcher", so the launchers read "Granate / Werfer".

The port string `%s|Patch %d%s%s\n\n` (the patch notes' header) is left out
of port.json: it reads the same in German, and check.py takes its `|` for a
`ctx|` key.
