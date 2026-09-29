# Japanese glossary (lang/ja)

Our own translation, made from the English alone (the `en` field of
`src/assets/ntsc-final/lang`, `lang/_source/port.json` and
`lang/_source/ge.json`). No other Perfect Dark or GoldenEye translation was
opened - not Rare's JPN or PAL text, not the other fields of the ROM JSON, not
GoldenEye's Japanese text, not another pack. Natural game Japanese in the
style of a 2000-era console action game. Keep these terms in `pd/*.json`,
`port.json` and `ge.json`.

## The font, and what it means for the text

- Japanese is drawn from M PLUS Rounded 1c baked into the game's own glyph
  format (`tools/langfont/bake.py`): every kana and kanji is **12 px wide** in
  the small fonts (14 px in the big one), while an English letter is 5-7 px.
  One Japanese character is about two English letters wide, so a label that
  must not wrap may hold about **0.6 of the English letter count** in
  Japanese characters ("Game Options", 12 letters -> "ゲーム設定", 5).
- The baked set is JIS X 0208 rows 1-5 and **JIS level 1 kanji**. A rarer
  kanji is baked in by `bake.py --extra-from lang/ja`, so it is allowed, but
  common words and kana are preferred; a dense kanji at 11 px is hard to read.
- **Half-width katakana are never used.** Digits and Latin letters are ASCII
  (half width): "ファルコン2", "G5ビル", "30秒". Full-width punctuation:
  、。！？「」『』…・ー～（）：. The English "..." is "…" (or "……" for a
  long trailing pause). No spaces between Japanese words; a space is kept
  only where the English layout needs one (a column, a leading/trailing
  space, between two Latin words).
- Lines break between any two Japanese characters; the wrapper keeps
  。、！？」』…ー and small kana off the start of a line (kinsoku).

## Control sequences (never change them)

Every `\n` (count and place, the trailing one too, a leading one too), `|`
briefing markers and the text after them up to " - ", printf conversions
(`%s`, `%d`, `%02d` ...: same ones, same order - the game has no positional
arguments, so Japanese word order is phrased around them), `\0` (the hangar
bios' "name\0|subheading": keep the NUL between the two halves), leading and
trailing spaces.

## Register

- **Menus, the system, help, training instructions, the port's own text:**
  polite です/ます ("～してください", "～しますか？"). Labels are nouns
  ("ゲーム設定", "セーブ"). On/Off values: オン/オフ.
- **Objectives** (the list in a briefing, the "Objective" screens):
  dictionary form, no ending punctuation: "セキュリティシステムを無効化する".
- **HUD messages**: short plain form, 。 at the end: "キャロル博士を発見した。",
  "重要な目標物が破壊された。", instructions "ディスクを入手せよ。".
- **Carrington (mission control) -> Joanna:** polite です/ます, gentlemanly,
  calls her "ジョアンナ": "ジョアンナ、よくやってくれました。".
  **Joanna -> Carrington:** polite, "了解しました", "司令" is not used - she
  says "キャリントン" or nothing.
- **Joanna** elsewhere: plain, cool and confident, lightly feminine
  ("～わ", "～ね" now and then, never cute).
- **Jonathan, Elvis, Institute staff <-> Joanna:** casual. Elvis is cheerful
  and odd ("ボクは…"? no: Elvis says "オレ"; he calls her "ジョー").
- **Dr. Caroll:** old-fashioned, polite ("～ですな"), "わたし".
- **The President:** dignified plain/polite; aides polite.
- **Cassandra De Vries:** haughty, feminine ("～わ", "～かしら", "あなた").
  **Trent Easton:** arrogant, "俺"/"私" to superiors. **Mr. Blonde:** cold,
  formal, "私".
- **Guards:** rough male speech: "いたぞ！", "撃て！", "何だ？", "侵入者だ！".
- **Skedar:** menacing, archaic ("愚かな人間どもよ"). **Maians/Elvis:** friendly.

## Names

Katakana for people and places; brand/model designations keep their
Latin/digit code. dataDyne is written **データダイン** (the corporation reads
better in katakana in running text); its product lines keep their code.

| English | Japanese |
|---|---|
| Joanna Dark / Jo | ジョアンナ・ダーク / ジョー |
| Daniel Carrington | ダニエル・キャリントン |
| Carrington Institute (CI) | キャリントン研究所 (CI) |
| Cassandra De Vries | カサンドラ・デ・ヴリーズ |
| Trent Easton | トレント・イーストン |
| Jonathan | ジョナサン |
| Elvis | エルヴィス |
| Mr. Blonde | ミスター・ブロンド |
| Dr. Caroll | キャロル博士 |
| Velvet Dark | ヴェルヴェット・ダーク |
| Grimshaw / Foster / Carrington's staff | グリムショウ / フォスター |
| The President | 大統領 |
| NSA / CIA / FBI | NSA / CIA / FBI |
| dataDyne | データダイン |
| Skedar | スケダール (人), スケダールの船 |
| Maian | マイアン |
| Cetan | セタン |
| Greys | グレイ |
| G5 / G5 Building | G5 / G5ビル |
| Area 51 / A51 | エリア51 |
| Air Force One | エアフォースワン |
| Pelagic II | ペラジックII |
| Perfect Dark | パーフェクト・ダーク |
| sims' names (MeatSim, EasySim ... DarkSim) | kept in Latin |

## Places and levels (Perfect Dark)

| English | Japanese |
|---|---|
| dataDyne Central / Research / Extraction | データダイン本社 / データダイン研究所 / データダイン本社 (脱出) |
| Lucerne Tower | ルツェルン・タワー |
| Laboratory Basement | 研究所地下 |
| Carrington Villa | キャリントン邸 |
| Chicago | シカゴ |
| G5 Building | G5ビル |
| Area 51 | エリア51 |
| Air Base / Alaskan Air Base | 空軍基地 / アラスカ空軍基地 |
| Crash Site | 墜落現場 |
| Deep Sea | 深海 |
| Attack Ship | 攻撃艦 |
| Skedar Ruins | スケダール遺跡 |
| Mission subtitles | Defection 亡命, Investigation 調査, Extraction 脱出, Hostage One 人質救出, Stealth 隠密, Reconnaissance 偵察, Infiltration 潜入, Rescue 救出, Escape 逃走, Espionage 諜報, Antiterrorism 対テロ, Confrontation 対決, Exploration 探索, Nullify Threat 脅威排除, Defense 防衛, Covert Assault 奇襲, Battle Shrine 戦いの神殿 |
| Special assignments | Mr. Blonde's Revenge ミスター・ブロンドの復讐, Maian SOS マイアンSOS, Retaking the Institute 研究所奪還, WAR! 戦争！, The Duel 決闘 |
| Firing Range | 射撃場 |
| Device Lab / Holo room / Info room / Hangar | デバイス研究室 / ホロルーム / 情報室 / 格納庫 |

## Modes and menus

| English | Japanese |
|---|---|
| Perfect Menu | パーフェクトメニュー |
| Solo Missions | ソロミッション |
| Combat Simulator | コンバットシミュレーター |
| Co-Operative / Counter-Operative | 協力プレイ / 対抗プレイ |
| Simulant / sim | シミュラント / シム |
| Agent / Special Agent / Perfect Agent | エージェント / スペシャルエージェント / パーフェクトエージェント |
| Briefing | ブリーフィング |
| Objective / Completed / Incomplete / Failed | 目標 / 達成 / 未達成 / 失敗 |
| Mission Status | ミッション状況 |
| Challenge | チャレンジ |
| Cheats | チート |
| Options / Game Options / Audio / Video / Control | オプション / ゲーム設定 / サウンド / 映像 / 操作 |
| OK / Cancel / Back / Yes / No | OK / キャンセル / 戻る / はい / いいえ |
| Controller / Control Stick / Control Pad | コントローラー / コントロールスティック / 十字キー |
| Z Button, B Button, R Button, C Buttons, Up C Button | Zボタン, Bボタン, Rボタン, Cボタン, C上ボタン (C下, C左, C右) |
| Controller Pak / Game Pak / Transfer Pak / Rumble Pak | コントローラパック / ゲームパック / 64GBパック / 振動パック |
| King of the Hill / Hold the Briefcase / Capture the Case / Hacker Central / Pop a Cap | キング・オブ・ザ・ヒル / ケース死守 / ケース奪取 / ハッカー・セントラル / ポップ・ア・キャップ |
| Perfect Buddy | パーフェクトバディ |
| Game file / Save / Load / Copy / Delete | ゲームファイル / セーブ / ロード / コピー / 削除 |
| Player | プレイヤー (the default player name, L_MISC_437, is "PL": 10 bytes at most) |
| Team names (L_OPTIONS_008-015) | 3 characters at most (11 bytes): "赤", "黄", "青" ... + "チーム" does not fit - colour words alone |
| score / kills / deaths | スコア / キル / デス |

## Weapons and gadgets

Model codes stay as they are (CMP150, DY357-LX, AR34, RC-P120, K7, XR-20);
names become katakana.

| English | Japanese |
|---|---|
| Unarmed | 素手 |
| Falcon 2 / (silencer) / (scope) | ファルコン2 / ファルコン2消音 / ファルコン2スコープ (the inventory list holds about 9 characters) |
| MagSec 4 | マグセック4 |
| Mauler | モーラー |
| Phoenix | フェニックス |
| DY357 Magnum / DY357-LX | DY357マグナム / DY357-LX |
| CMP150 | CMP150 |
| Cyclone | サイクロン |
| Callisto NTG | カリストNTG |
| RC-P120 | RC-P120 |
| Laptop Gun | ラップトップガン |
| Dragon / SuperDragon | ドラゴン / スーパードラゴン |
| K7 Avenger | K7アベンジャー |
| AR34 | AR34 |
| Reaper / Devastator / Slayer | リーパー / デバステーター / スレイヤー |
| Rocket Launcher | ロケットランチャー |
| FarSight XR-20 | ファーサイトXR-20 |
| Shotgun / Sniper Rifle / Crossbow | ショットガン / スナイパーライフル / クロスボウ |
| Tranquilizer | トランキライザー |
| Combat Knife / Throwing Knife / Hunting Knife | コンバットナイフ / スローイングナイフ / ハンティングナイフ |
| Psychosis Gun | サイコシスガン |
| Grenade / N-Bomb | グレネード / Nボム |
| Timed / Proximity / Remote / ECM Mine | 時限マイン / 近接マイン / リモートマイン / ECMマイン |
| Night Vision / X-Ray Scanner / IR Scanner | 暗視ゴーグル / X線スキャナー / IRスキャナー |
| Door Decoder | ドアデコーダー |
| Data Uplink | データアップリンク |
| R-Tracker / Tracker | Rトラッカー / トラッカー |
| Tracer Bug | 追跡発信機 |
| Cloaking Device | クローキング装置 |
| Shield | シールド |
| Disguise | 変装 |
| Key card / Necklace | カードキー / ネックレス |
| Horizon Scanner | ホライゾンスキャナー |
| Briefcase / Suitcase | ブリーフケース / スーツケース |
| Backup Disk | バックアップディスク |
| AutoSurgeon / Alien Medpack | オートサージョン / エイリアン医療キット |
| Target Amplifier | ターゲットアンプ |
| CamSpy / DrugSpy / BombSpy | カムスパイ / ドラッグスパイ / ボムスパイ |
| Combat Boost | コンバットブースト |
| primary / secondary function | 第1機能 / 第2機能 |
| magazine / ammo / rounds | マガジン / 弾薬 (short: 弾) / 発 |
| rpm | 発/分 |

## Pickup messages (propobj)

English builds "Picked up " + "a " + name + "." from pieces. In a CJK pack the
port takes the ROM's own Japanese branch instead (propobj.c,
`PICKUP_JPN_ORDER`): name + L_PROPOBJ_000, no determiner, no plural "s", no
full stop - "グレネードを手に入れた", "ファルコン2を手に入れた". So
L_PROPOBJ_000 is "を手に入れた"; the determiners and the plural piece are
never drawn and hold an ideographic space (an empty string would fall back
to English); "Double " is "二丁：". Ammo names are bare nouns (ショットガン弾,
マグナム弾, グレネード弾 ...), and "combat " + "knife" is コンバット + ナイフ.
Whole-sentence pickups in the mission banks: "Obtain X." -> "Xを入手せよ。",
"Picked up X." -> "Xを入手した。". GoldenEye's own pickup rows (ge.propobj)
are joined to "入手" as "入手：PP7。".

## Briefings

`|背景 - `, `|キャリントン - `, `|目標1： - ` ... `|目標5： - `, and the closing
"END" is "以上". An owner piece ("Dr. Caroll's", "Guard's") is "キャロル博士の",
"警備兵の"; "A CamSpy" is the bare name.

## GoldenEye 007 (ge.json)

M briefs 007 in a brusque, formal plain style (～だ／～せよ); Q is fussy and
dry; Moneypenny light and teasing, polite. Objectives in dictionary form, as
above. Strings GoldenEye writes twice (upper and lower case) get the same
Japanese. Spaced-out titles ("D A M") are written normally.

| English | Japanese |
|---|---|
| James Bond / 007 / M / Q / Moneypenny | ジェームズ・ボンド / 007 / M / Q / マネーペニー |
| Alec Trevelyan / 006 | アレック・トレヴェルヤン / 006 |
| Ourumov / Natalya Simonova / Boris Grishenko | ウルモフ / ナターリア・シモノヴァ / ボリス・グリシェンコ |
| Xenia Onatopp / Valentin Zukovsky / Dimitri Mishkin | ゼニア・オナトップ / ヴァレンティン・ズコフスキー / ディミトリ・ミシュキン |
| Jaws / Baron Samedi / Oddjob / May Day | ジョーズ / バロン・サメディ / オッドジョブ / メイ・デイ |
| Janus / Janus Syndicate | ヤヌス / ヤヌス組織 |
| GoldenEye (satellite weapon, key) | ゴールデンアイ |
| MI6 / KGB | MI6 / KGB |
| Arkhangelsk / Dam / Facility / Runway | アルハンゲリスク / ダム / 化学兵器工場 (name on the grid and folder: 化学工場) / 滑走路 |
| Severnaya / Surface / Bunker | セヴェルナヤ / 地上施設 / 地下壕 |
| Kirghizstan / Silo | キルギスタン / サイロ |
| Monte Carlo / Frigate | モンテカルロ / フリゲート艦 (grid: フリゲート) |
| St. Petersburg / Statue (Park) | サンクトペテルブルク / 銅像公園 |
| (Military) Archives / Streets / Depot / Train | 軍公文書館 / 市街 / 車両基地 / 装甲列車 |
| Cuba / Jungle / Control (Center) | キューバ / ジャングル / 管制センター (grid: 管制施設) |
| (Water) Caverns / (Antenna) Cradle | 地下洞窟 / アンテナ |
| Aztec (Complex) / Egyptian (Temple) | アステカ / エジプト神殿 (grid: エジプト) |
| Agent / Secret Agent / 00 Agent / 007 (difficulty) | エージェント / シークレットエージェント / 00エージェント / 007 |
| PP7 / PP7 (silenced) / DD44 Dostovei / Klobb | PP7 / PP7 (消音) / DD44ドストヴェイ / クロッブ |
| KF7 Soviet / ZMG (9mm) / D5K Deutsche / Phantom / RC-P90 | KF7ソビエト / ZMG (9mm) / D5Kドイチェ / ファントム / RC-P90 |
| AR33 Assault Rifle / Sniper Rifle / Shotgun / Automatic Shotgun | AR33アサルトライフル / スナイパーライフル / ショットガン / オートショットガン |
| Cougar Magnum / Golden Gun / Silver PP7 / Gold PP7 | クーガーマグナム / 黄金銃 / シルバーPP7 / ゴールドPP7 |
| Moonraker Laser / Watch Laser | ムーンレイカー・レーザー / 腕時計レーザー |
| Hand Grenade / Grenade Launcher / Rocket Launcher | 手榴弾 / グレネードランチャー / ロケットランチャー |
| Timed / Proximity / Remote Mine / Detonator | 時限マイン / 近接マイン / リモートマイン / 起爆装置 |
| Throwing Knife / Hunting Knife / Slappers / Taser / Tank | スローイングナイフ / ハンティングナイフ / 素手 / テーザー / 戦車 |
| Plastique / Covert Modem / Datathief / Bomb Defuser | プラスチック爆弾 / 秘密モデム / データシーフ / 爆弾解除装置 |
| Key Analyzer / Door Decoder / Camera / Bug | キー解析装置 / ドアデコーダー / カメラ / 盗聴器 |
| Body Armor / Watch / Watch Magnet | ボディアーマー / 腕時計 / 腕時計マグネット |
| keycard / safe / flight recorder | カードキー / 金庫 / フライトレコーダー |
| mainframe / circuit board | メインフレーム / 回路基板 |
| Mission Failure / civilian casualties | ミッション失敗 / 民間人の犠牲 |

## The port's own strings (port.json)

| English | Japanese |
|---|---|
| Customize Character | キャラクター編集 |
| Settings Preset / Camera Preset | 設定プリセット / カメラプリセット |
| Custom / Default | カスタム / 標準 |
| Vanilla / Reset to Stock | オリジナル / 初期設定に戻す |
| Ghost Trials / ghost | ゴーストトライアル / ゴースト |
| Sign In / Create Account | サインイン / アカウント作成 |
| Offline / Online | オフライン / オンライン |
| Leaderboards | ランキング |
| Crash report / Report a Problem | クラッシュレポート / 問題を報告 |
| Texture Pack / Model Pack / Community Packs | テクスチャパック / モデルパック / コミュニティパック |
| Stage Loader / stage | ステージローダー / ステージ |
| Randomizer / seed | ランダマイザー / シード |
| Renderer / Upscaling / Supersampling | レンダラー / アップスケール / スーパーサンプリング |
| GE Plus | GE Plus |
| Language | 言語 (the language names stay in their own languages) |

## Lengths, as measured in the game

- The mission grid of GE Plus holds five characters under a photo; the
  inventory list about nine (the port widens a fixed-width list by a fifth
  in a CJK pack, menu.c); the watch's abort row holds 取消 / 決定, not
  キャンセル; the briefing folder's "Mission 1:" is "任務 1:" so the level's
  name clears the photograph.
- Team names are one to three characters (赤 黄 青 紫 水色 橙 ピンク 茶);
  the default player name is "PL".
- `pd --lang ja --lang-audit` still lists about 1400 strings over 1.3x the
  English: nearly all are HUD lines, objectives and dialogue, which wrap, or
  labels in dialogs that grow to fit. Labels that ran off a fixed box were
  shortened after screenshots (the length pass: 35 labels, and the ones above).
