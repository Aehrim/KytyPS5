# Fork-Fortschritt: Demon's Souls (PPSA01341) auf KytyPS5

Dieser Fork (github.com/Aehrim/KytyPS5) verfolgt ein konkretes Langzeitziel: **Demon's Souls (PS5, Bluepoint)
flüssig spielbar** – mindestens 60 fps, ohne Stottern und Glitches, auf Mittelklasse-Hardware.
Alles, was dabei allgemein nützlich ist, soll auch anderen Titeln zugutekommen.

Die Datei ist das Logbuch: Was wurde wann warum geändert, wo stehen wir, was ist offen.
Upstream-Bezug: [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5), Issues #550, #614, #626 (Demon's Souls),
#507 (Design-Diskussion zu dynamischen Deskriptor-Indizes).

## Testumgebung

| | |
|---|---|
| CPU / RAM | Ryzen 7 5800X, 32 GB |
| GPU | AMD Radeon RX 9060 XT (RDNA 4), Windows-Treiber 32.0.31041.1004 |
| OS | Windows 11 |
| Toolchain | VS 2022 Build Tools + clang-cl 19.1.5, CMake 4.4.3, Ninja 1.13.2, Qt 6.8.3, Vulkan SDK 1.4.357.0 |
| Spiel | Demon's Souls v01.007, app0-Dump |

Build: siehe README (Windows). Konfiguration `-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64`, Build-Ordner `_Build/windows`.
Logs der Testläufe liegen lokal unter `_Build/logs/runN/` (nicht im Repo).

**Startoptionen für Demon's Souls:** `--redzone` ist Pflicht (siehe Meilenstein 9); für Diagnose `--printf-direction File`,
bei Shader-Problemen zusätzlich `--shader-log-direction File`.

**RenderDoc-Workflow:** RenderDoc (winget `BaldurKarlsson.RenderDoc`) installieren; Emulator mit `--rd` starten (findet die DLL
über die Registry, Log: „RenderDoc: API 1.6.0 bound“); **F1** im Spielfenster nimmt zwei Guest-Flips auf →
`_Build/windows/_RenderDoc/kyty_<id>_capture.rdc` (~2 GB). Während des Captures kopiert RenderDoc alle GPU-Ressourcen:
RAM-Bedarf des Emulators steigt stark, keine Auswertung parallel laufen lassen (32 GB waren voll → Vulkan-OOM im
Stream-Buffer, Capture verloren). Für Capture-Läufe `--printf-direction Silent` (das Guest-Log wuchs auf 4 GB).
Auswertung headless: `qrenderdoc.exe --python <script.py>` mit dem `renderdoc`-Modul (Skripte im Scratchpad:
`rd_overview.py` = Aktionsliste, `rd_passes.py` = Pipeline-State pro Pass-Gruppe).

## Stand

| Bereich | Status |
|---|---|
| Boot bis Hauptmenü | ✅ läuft (upstream: crasht) |
| Intro-/Logo-Videos (Bink) | ✅ mit Ton |
| Hauptmenü | ✅ bedienbar, **kein Ton** |
| Neues Spiel → Charakter-Editor | ✅ vollständig durchlaufen (Texturen größtenteils schwarz) |
| Charakter-Editor → Spielwelt | ✅ **Im Spiel**, Kamera und Bewegung funktionieren; HUD korrekt; **Spielwelt rendert** (Lauf 41: Tunnel, Lichtschacht, Figur, HUD, Menüs); Stabilität des Bildes ohne Tracer noch in Klärung (Problem 12) |
| Performance | 1–2 fps in der Welt – Shader-Kompilierung, kein Pipeline-Cache, CPU-seitige Tabellen-Materialisierung pro Dispatch; noch nicht aussagekräftig |

## Meilensteine

### 2026-09-16 – Setup, Boot-Crash gelöst, Charakter-Editor erreicht

**Ausgangslage.** Upstream-Build `63fb822` bricht nach ~54 Compute-Shadern ab:
`shader resource tracking: hash=0x9fb5cc274b5e4f82 stage=compute pc=0x1044 GetImageResource dword 0 is not a valid runtime value`
(identisch zu Issue #626).

**Analyse.** Der Shader macht einen zweistufigen Material-Lookup, wie er in modernen Engines üblich ist:

```
V_READFIRSTLANE_B32  vcc_lo, v6            ; Objekt-/Material-Index, wave-uniform gemacht
S_MUL_I32            s0, vcc_lo, 0xe0      ; × 224 Byte = Record-Offset im Material-Buffer
S_BUFFER_LOAD_DWORDX2 vcc_lo, s[8:11], s0  offset=4   ; Textur-Key aus Record+4
S_LSHL_B32           s1, vcc_lo, 5         ; Key × 32 Byte = T#-Offset im Heap
S_BUFFER_LOAD_DWORDX8 s[0:7], s[16:19], s1 ; T# aus dem Textur-Heap
image_sample_l ...                         ; Cubemap, als 2D-Array gesampelt (V_CUBE*-Befehle davor)
```

KytyPS5 löst Deskriptoren auf der CPU auf (`SrtWalker`) und bindet sie in ein festes Vulkan-Layout; echtes Bindless
gibt es nicht. Für genau dieses Muster existiert `TryMakeIndirectImage` (Material → Key → Heap → T#), das alle
erreichbaren Keys vorab enumeriert und den Shader per Binary-Search wählen lässt. Es scheiterte an drei Details:

1. **Key-Immediate** (`ResourceTracking.cpp`): Der Key liegt bei Record-Offset 4, und der Compiler hat die +4 als
   Instruktions-Immediate kodiert statt als eigenes `S_ADD`. Der Tracker verlangte Immediate 0.
   → Immediate wird in `selector_offset` gefaltet; die Hardware bricht `soffset + imm` ebenfalls auf 32 Bit um,
   die Semantik bleibt identisch. Test `wrapped_immediate` entsprechend umgedreht. Commit `f03e189`.
2. **Probe-Enumeration** (`ResourceMaterialization.cpp`): Die Wrap-sichere Enumeration lief in
   `gcd(224, 2³²) = 32`-Byte-Schritten und las damit 6 fremde Record-Felder pro Record als „Key“. Auf echten
   Material-Buffern ergibt das Müll-Kandidaten (im Charakter-Editor: eine 2D-Textur in einer Cubemap-Tabelle →
   `incompatible candidates`) und sprengt das 64-Image-Limit.
   → Probes nur an echten Record-Positionen `offset + k·stride`. Commit `8fd21cd`.
3. **Inkompatible Kandidaten**: Ein Record, dessen Key auf eine anders geformte Textur zeigt, führte zum Fatal.
   → Kandidat wird als Null-Image gebunden (Warnung im Log, gedeckelt). Commit `8fd21cd`.

Zusätzlich: Bei einem Tracking-Fatal wird jetzt die Abhängigkeitskette des abgelehnten Dwords und das komplette IR
ausgegeben (`02bd07c`), damit die nächsten Fälle dieser Klasse ohne Debugger lesbar sind.

4. **Sampler-Phi nach Kill-Pfad** (`ResourceTracking.cpp`, Pixel-Shader `0x8205bcef6a135cee` pc `0x294`): Die
   Diagnose zeigte `GetSamplerResource dword 0 = Phi(0x0, ReadConst[SRT slot 0x1a])`. Der Structurizer leitet den
   „alle Pixel verworfen“-Ausgang (`exec = 0`, pc `0x654`) durch die Merge-Blöcke des restlichen Codes; auf diesem Arm
   wurde das Sampler-Register nie geladen (SGPR-Startwert 0). Kein Lane erreicht das Sample über diesen Arm.
   → `LowerDescriptorPhi` überspringt Arme mit dem unbeschriebenen Wert 0 und nimmt den einzigen beschriebenen Arm.
   Regressionstest `unwritten descriptor phi arm`. Commit `8a023ba`.

5. **Gestreamte Mip-Ketten** (`descriptors.cpp`, Host): T# mit `LAST_LEVEL` > `MAX_MIP` (8192×128, Tile 27,
   base=1 last=4 max=3) – die volle Mip-Kette ist deklariert, resident sind nur `MAX_MIP + 1` Level. Der Host brach
   ab, wenn die zusätzlichen Level das Tiling-Layout ändern würden. → View auf die residenten Level clampen (Abbruch
   nur noch, wenn `BASE_LEVEL` selbst außerhalb liegt). Commit `6414e01`.
6. **Waterfall-Loop über Bitmaske** (`ResourceTracking.cpp` / `ResourceMaterialization.cpp`, Pixel-Shader
   `0xc509ed46b415549b` pc `0xfc`): `T# = *(SRT + 344 + (s_ff1(mask) << 5))` – pro gesetztem Bit ein Deskriptor
   aus einer statischen Tabelle. Der Index ist beweisbar beschränkt (0–31), die Adresse statisch.
   → Neue „indexed“-Form des Indirect-Image-Plans (`IndirectImage::key_count`): Heap ist eine 2-Dword-Adresse,
   Key = Index; die Materialisierung liest alle Tabelleneinträge, Mapping und Binary-Search im SPIR-V wie bisher.
   Index-Bound erkannt für `FindILsb32` (32), `BitwiseAnd32`/`UMin32` mit Konstante (Maske + 1). Regressionstest
   `indexed image table`. Commit `50ea6f7`.
7. **Base-Array außerhalb der Layer** (`descriptors.cpp`, Host): T# mit `BASE_ARRAY` ≥ Layer-Anzahl → Abbruch.
   → Clamp auf den letzten Layer, Deskriptor geloggt (gedeckelt). Commit `54049c6`.

**Ergebnis.** Boot → Logos → Hauptmenü → Neues Spiel → Charakter-Editor bis zur Namens-/Klassenwahl.
Shader-Zähler beim letzten Lauf: VS 26 / PS 53 / CS 133.

**Beobachtungen.**
- Das Spiel öffnet über `open()` nur ~35–42 Dateien (Configs, Videos, Menü-Sounds); Level-/Modelldaten laufen
  über einen anderen I/O-Pfad der Engine. Noch nicht untersucht.
- `--graphics-debug-dump True` erzeugt Multi-GB-Logs (CP-Paket-Trace). Für normale Läufe weglassen;
  `--shader-log-direction File` reicht für Shader-Listings (landen inline im printf-Log).
- Vulkan-Pipeline-Cache ist bei „dirty“ Builds abgeschaltet.

### 2026-09-17 – Charakter-Editor durchlaufen, Welt lädt

8. **Müll-Deskriptoren aus Tabellen-Probes** (`ResourceMaterialization.cpp`): Die Material-Probe liest Offset 4
   jedes Records; Records anderen Typs liefern Keys, die im Heap auf Fremddaten zeigen, und 32 zufällige Bytes
   bestehen den Typ-/Format-Check (`ValidImageDescriptor`) oft genug. Sichtbar als „T#“ mit 2186 Layern am Anfang des
   Direct-Memory, Base-Array 560–6664 auf 64-Layer-Texturen, gefolgt von Texture-Cache-Alias-Konflikten und
   ungültigen Image-Views. → `PlausibleImageDescriptor`: Depth < 2048, Base-Array ≤ Depth, bei Tabellen-Kandidaten
   zusätzlich Base-Level ≤ Last-Level; Verstöße werden Null-Kandidaten. Dazu Null-Fallbacks im Texture-Cache
   (`c96f597`) und für nicht sampelbare Formate (`k16UScaled`, `ab1a305`). Commits `c96f597`, `d171b7b`, `ab1a305`.
9. **Red Zone** (Lauf 14): Guest-Fault auf `BPE JobWorkerThread`: `mov rax,[rsp-0x10]; mov [rax+0x18],ebp` mit
   `rax = 0` – das Spiel liest einen Zeiger aus der SysV-Red-Zone (128 Byte unter `rsp`). Windows liefert
   Exceptions (hier: Page-Protection-Faults der Speicherüberwachung) auf dem Stack des faultenden Threads aus und
   überschreibt genau diesen Bereich. Dieselbe Klasse wie upstream #614 (Linux, `address=0xa8`).
   → Kein Code-Fix nötig: Startoption `--redzone` (Loader leitet faultfähige Zugriffe auf stackwechselnde
   Trampoline um). Ab jetzt Pflicht für diesen Titel. **Ergebnis Lauf 15: Spielwelt erreicht** (Nebel, ~2 fps,
   VS 47 / PS 81 / CS 333 nach 107 s).
10. **Tabellen-Loop im Vertex-Shader** (`ResourceTracking.cpp`, `0x3b9e5a334aadc61c` pc `0x264`): T#-Tabelle bei
    `SRT + 1712`, Index ist eine Schleifenvariable `Phi [0, entry], [i + 1, latch]`. → Induktions-Phi als Index
    akzeptiert; Grenze aus der Schleifenbedingung (`i < n`, `i + 1 != n` mit Konstante), sonst Fallback 32 Einträge.
    Commit `2a3e748`.
11. **Depth-Feedback** (`descriptors.cpp`, Host, Lauf 16 nach 166 s in der Welt, VS 159 / PS 261 / CS 491): Ein
    Shader sampelt den Tiefenpuffer, während er als beschreibbares Attachment gebunden ist (Nebel-/Partikel-Pässe);
    der Feedback-Loop-Pfad deckte nur Pixel-Shader ab. → Warnung (Stage/Layout/Aspect, gedeckelt) statt Abbruch;
    der Treiber löst den Feedback auf. Commit `541d11f`. Bildqualität dieser Pässe offen.
12. **Autosave-Dialog-Schleife** (`dialog.cpp`): `SaveDataDialogOpen` meldete jeden Dialog sofort als beendet. Das
    Spiel hält den Fortschrittsdialog (`mode 3`, Sys-Message-Typ `PROGRESS`) offen und öffnet ihn bei „beendet“
    jede Iteration neu – 2,5–6,5 Mio. Aufrufe pro Sitzung mit je neun Log-Zeilen; Lauf 17 blieb darin hängen
    (weißes Hauptmenü, Glitches, „Freeze“ – kein Code-Unterschied zu Lauf 16/18, reines Timing).
    → Fortschrittsdialog bleibt `RUNNING` bis `Close`; alle anderen Dialoge weiterhin Auto-OK; Dump nur für die
    ersten 32 Aufrufe. Commit `c14ec62`.
13. **Unbeschränkter Tabellenindex** (`ResourceTracking.cpp`, Compute `0x59775bb47ad2848a` pc `0x1a0c`, Lauf 18 nach
    122 s, VS 160 / PS 259 / CS 587): Index = `ReadFirstLane(Select(…, LoadAddress[Global]))` – zur Laufzeit aus
    dem Speicher geladen, statisch nicht beschränkbar (Fall aus #507). → Da die Form (8 Adress-Loads, Stride 32)
    die Tabelle bereits identifiziert, bekommt ein unbeschränkter Index dasselbe 32-Einträge-Budget wie eine
    Schleife ohne sichtbare Grenze (Log mit Index-Ausdruck). Commit `2ee0ab1`.
14. **View-Layer jenseits des Bildes** (`imageView.cpp`, Host, Lauf 19 nach 293 s im Spiel): 2D-Array-View
    `80+193` auf einem 256-Layer-Bild – der Deskriptor deklariert mehr Slices, als das gecachte Bild hat.
    → Layer-Anzahl auf die vorhandenen Slices clampen, Log. Commit `cf02bfa`.

**Ergebnis 2026-09-17.** Vom Charakter-Editor bis **ins Spiel**: HUD, Item-Slots, Gebietsname rendern korrekt;
die 3D-Welt ist schwarz/rot, weil die Material-Tabellen größtenteils Null-Images liefern. Lauf 19 lief 293 s in der
Welt mit VS 184 / PS 292 / CS 717 Shadern. Fallback-Häufigkeiten in Lauf 19: Material-Tabellen-Nulls 32
(pc `0xfc`: 24, pc `0x1470`: 8), Mip-Clamps 16, Depth-Feedback 16, nicht sampelbare Formate 4
(`0x3a2eb41bacc0239f`: Formate 17, 39; `0xc509ed46b415549b`: Formate 16, 84), unbeschränkte Tabellenindizes 3.

**Phase 1 (Booten bis ins Spiel) ist damit abgeschlossen. Phase 2 (korrekt rendern) beginnt.**

### 2026-09-17 – Phase 2: korrekt rendern

15. **2D-Texturen in 2D-Array-Tabellen** (`ResourceMaterialization.cpp`): Die Licht-Tabelle bei pc `0xfc` nullte
    24× pro Lauf Einträge, weil normale 2D-Shadowmaps neben 2D-Array-Shadowmaps liegen und der Shader alle mit
    `2d_array`-Koordinaten sampelt; `DescriptorDimension` mappte `kColor2D` stur auf `Dim2D` → Form-Konflikt → Null.
    Eine 2D-Textur mit Array-Koordinaten ist auf der Hardware ein 1-Layer-Array (Layer clampt auf 0), Vulkan erlaubt
    eine 2DArray-View auf ein 2D-Bild. → Bei angefragtem `Dim2DArray` bleibt es `Dim2DArray`. Echter Fix, kein
    Fallback. Commit `f5efe1a`. Nicht anwendbar auf Cubemap-Tabellen (pc `0x1470`): `cube` ändert die
    Koordinatenberechnung im Code; der dortige 2D-Kandidat ist ein 1×4-Platzhalter, Null ist korrekt.
16. **View-Layer-Clamp** (`imageView.cpp`, Commit `cf02bfa`), siehe 14. Nachtrag zu 15: Die Promotion gilt nur für
    *gesampelte* Bilder – Storage-Views müssen den Deskriptor-Typ behalten (Lauf 21 brach sonst beim Start ab). Commit
    `3490629`. Ergebnis Lauf 22: keine Nulls mehr bei `0xfc`; übrig sind die Cubemap-Tabellen (`0x1044`, `0x1470`).
17. **USCALED/SSCALED-Texturen** (`gpu_format.cpp`, `spirvEmitterImage.cpp`): `k16UScaled`, `k8_8UScaled`,
    `k8_8SScaled`, `k10_11_11UScaled` werden jetzt über den Konvertierungspfad gesampelt – Backing `R16_UINT`
    bzw. `R32_UINT`, Bitfeld-Extraktion wie bisher, danach `OpConvertUToF`/`OpConvertSToF` + `Bitcast`, sodass der
    Shader die Float-Bits bekommt, die die Hardware für Scaled-Formate liefert; Swizzle-Konstante „1“ wird `1.0f`.
    Weitere „unsupported format“-Nummern (4, 32, 49, 52, 84) wechseln pro Lauf → Zufallsbits aus
    Tabellen-Fremdeinträgen, korrekt genullt. Commit `0cbe4f5`.

**RenderDoc-Befund (Capture 1, Lauf 24, 2026-09-17):** Frame-Struktur von Demon's Souls (pro Frame ~5 800 Draws, ~3 600 Dispatches):
Schatten (1024² D32-Arrays, 80 + 4 Layer) → Depth-Prepass 1440p (BC4-Alphamasken) → Volumen-Nebel (214×120×64
RGBA16F, 8 Slices je Draw) → Visibility-/G-Buffer (4× R32_UINT IDs + RG16F Motion-Vectors, >1 000 Draws) → Hi-Z/SSAO
(Compute) → Clustered Lighting (Cluster-Gitter 32×24×24 und 256×128×64, Schatten-Atlas 384² × 360) → Material-/Decal-Pass
(Draw 174246, echte BC1/BC7-Texturen gebunden) → **Lighting-Resolve (Fullscreen-Draw 1449 → HDR RGBA16F)** →
Bloom/Post 720p, Auto-Exposure (Dispatch 1464) → UI + Upscale auf **3840×2160** (das Spiel legt seine Ausgabepuffer
immer in 4K an; intern 1440p). Pixel-Sonde: **Der Lighting-Resolve schreibt an allen Probe-Punkten NaN** – das ist die
Wurzel der bunten Flächen (NaN → Tonemapping → gesättigt/schwarz). Das RG16F-Target sind Motion-Vectors (~10⁻³), keine
Normalen. Visibility-IDs plausibel. Nächster Schritt: RenderDoc-Pixel-Debugger auf Draw 1449, erste NaN-erzeugende
Instruktion finden. Werkzeuge: `rd_overview.py`, `rd_passes.py`, `rd_dump.py` (PNG-Dump), `rd_probe.py` (PickPixel),
`rd_debug.py` (DebugPixel) im Scratchpad.

18. **DX10-Clamp** (`spirvEmitterAlu.cpp`, aus der RenderDoc-Analyse): Der Pixel-Debugger zeigte, dass der
    Lighting-Resolve (Draw 1449) sein NaN aus dem **Nebelvolumen** (3D-Textur 214×120×72) übernimmt; das Volumen wird von
    Dispatch 1204 temporal integriert (liest das Volumen des Vorframes) – ein NaN bleibt darin für immer. Der
    RDNA-`clamp`-Modifier macht im DX10-Clamp-Modus, in dem PS5-Shader laufen, **NaN zu 0**; der Emitter übersetzte ihn als
    reines `FClamp(x, 0, 1)`, das NaN durchlässt. → `select(isnan(x), 0, clamp(x, 0, 1))`. Commit `90917f7`.
    **Noch nicht verifiziert:** Lauf 25 (erster Lauf mit dem Fix) zeigte von Anfang an nur Schwarz (auch Logo-Video
    und HUD), obwohl das Spiel bis ins Tutorial lief (310 s, VS 160 / PS 247 / CS 594, Tunnel-Ambience geladen).
    Ob das der Fix oder die bekannte Video-/Textur-Flakiness (Problem 5) ist, klärt ein A/B-Lauf mit revertiertem
    Commit – **erster Punkt der nächsten Sitzung.**

19. **A/B-Test DX10-Clamp** (Läufe 27 ohne / 28 mit `90917f7`, sonst identische Binaries): beide zeigen dasselbe
    Bild (HUD korrekt, Welt in Reinfarben Magenta/Rot/Blau/Grün/Schwarz = NaN pro Farbkanal). Der Clamp-Commit ist
    also **nicht** schuld am Schwarz von Lauf 25 (das war Problem 5), behebt die Farben aber auch nicht. Bleibt drin
    (korrekte Semantik).
20. **Mip-View jenseits von `MAX_MIP`** (`descriptors.cpp`, Lauf 26, Absturz beim Laden des Tutorial-Tunnels):
    T# 512×1024 mit `base=2 last=10 max=0`. `MAX_MIP` kann den Speicher nicht beschreiben, wenn der View dahinter
    beginnt; weil Mips **kleinste zuerst** im Speicher liegen (`TileGetTiledTextureLayout`: Mip 0 liegt hinten),
    bestimmt die Level-Zahl die Adresse jedes Mips. → In diesem Fall zählt die Level-Zahl des Views (`last + 1`)
    statt Abbruch; Format und Adresse werden geloggt. In den Läufen 27–30 nicht erneut getroffen (Streaming-Timing).
    Auffällig im selben Log: RGBA16F-Textur 8192×128 (Render-Target-Tiling, 2 Adressen) mit gleitenden Mip-Fenstern
    1–4 / 2–5 / 3–6 bei `max=3` – vermutlich Reflexions-Probe-Atlas (64 × 128²); der bestehende Clamp schneidet dort
    Level ab (offen).
21. **NaN-Quelle eingegrenzt: GI-Probes im Nebel-Lichtshader** (RenderDoc-Capture Lauf 24, Skripte `rd_fog.py`,
    `rd_fogcmp.py`, `rd_bufdump.py`, `rd_bufevo.py`; Diagnose-Läufe 29/30):
    - Das gerenderte Nebel-Array (214×120, 72 Layer) ist sauber (0 NaN, Werte 0–0,08). Die 3D-Volumen (History
      `1014`, Ausgabe `119301`, Filter `119304`) sind zu 97 % NaN und enthalten negative Radianz.
    - Der Nebel-Lichtshader (CS `0xf0dc79c3467d5e0b`, 3122 Instruktionen, Dispatch 129471 im Capture) erzeugt
      **selbst** neue NaNs: 14 975 Voxel mit sauberer History und NaN-Ausgabe, fast nur in RGB. Die temporale
      Rückkopplung verteilt sie danach über das ganze Volumen.
    - Quelle im Shader: GI-Probe-Interpolation (8 Nachbarn, Oktaeder-Sichtbarkeitstest gegen D16-Atlas 384²×360):
      `BUFFER_LOAD_DWORD id ← s40[cluster*8+corner]`, `BUFFER_LOAD_FORMAT_X w ← s12[id]`,
      `BUFFER_LOAD_FORMAT_XYZ rgb ← s8[id*2]`. Deskriptoren (Diagnose Lauf 30): s12 = Format 56 (RGBA8 UNorm,
      Stride 4), s8 = **Format 71 (RGBA16 Float, Stride 8)**, Swizzle Identität.
    - Der Inhalt von s8 (1,47 MB = 92 160 Probes × 16 Byte) ist **byteidentisch mit Abschnitten der Dateien
      `globalillumination/worlds/.../*.cgpd`** (971 184 Byte am Stück aus `tower_end_11b05752.cgpd`). Statistik der
      Daten: alle 16 Bit gleichverteilt (auch die Exponent-Bits), räumlich korreliert (r ≈ 0,6), viertes u16 meist 0
      → das sind **16-Bit-Integer (UNorm/SNorm-artig), keine Half-Floats**. Als Half dekodiert ergeben sie negative
      Werte, Riesenwerte und NaN – genau unser Bild. `NaN × Gewicht 0` bleibt NaN.
    - Ausgeschlossen: falscher Buffer gebunden (Größen 92 160 × 4 / × 16 passen), Min/Max-NaN-Semantik (upstream
      korrekt), unausgerichtete Sub-Word-Loads (Host bricht ab), 2D-Array↔3D-Alias (Volumen werden von Compute
      beschrieben), Subgroup-Größe (64 angefordert und unterstützt).
    - **Offen:** Warum deklariert das Spiel Half-Float für Daten, die keine sind? Kandidaten: (a) Dump mischt
      Daten und eboot verschiedener Versionen (Spiel-Log: `BuildVersion=2025-10-15`), (b) ein CPU-seitiger
      Dekodierschritt fehlt/läuft über eine falsche HLE-Funktion, (c) virtuelles Remapping (AMM) zeigt auf die
      Rohdaten statt auf dekodierte Seiten, (d) Format-Semantik formatierter Buffer-Loads weicht ab.
    - RenderDoc-Hinweise: `DebugThread` stürzt bei Compute-Shadern mit Subgroup-Ops ab (Segfault) → Speicherinhalte
      analysieren; Skripte mit `os._exit(0)` beenden (Replay-Shutdown hängt sonst minutenlang).

22. **Formatierte Buffer-Stores kodieren nicht** (`spirvEmitterMemory.cpp` / `spirvEmitterMemoryHelpers.cpp`) –
    **die eigentliche NaN-Ursache aus Meilenstein 21.** Korrektur der dortigen Annahme: die von den Shadern benutzten
    Probe-Records stammen *nicht* aus den `.cgpd`-Dateien (nur der ungenutzte Rest des Heaps war Dateiinhalt), sondern
    werden zur Laufzeit geschrieben (~250 Records pro Frame, GI-Probe-Relighting). `BUFFER_LOAD_FORMAT_*` normalisiert
    über das Deskriptor-Format (`NormalizeFormatComponent`), `BUFFER_STORE_FORMAT_*` schrieb dagegen die unteren
    8/16 Bit des Registers unverändert – bei Float-Registern also Mantissen-Rauschen. Daher „gleichverteilte 16-Bit-
    Werte“ in einem RGBA16F-Buffer. Upstreams Tests decken nur Integer- und 32-Bit-Formate ab (dort ist roh korrekt).
    → `EncodeFormatComponent` als Umkehrung: UNorm/SNorm klemmen, skalieren, runden (NaN → 0); UScaled/SScaled
    klemmen und konvertieren; Float16 mit Round-toward-zero (`EmitF32ToF16RtzBits`); Integer, 32 Bit und 10/11-Bit-
    Float bleiben roh. Tests `BufferStoreFormatXResource16FloatEncodesHalf`, `…8UnormEncodesByte` (gebaut, **noch nicht
    ausgeführt**: die Test-Binary reserviert 13,8 GB Commit, die beim Testen nicht frei waren). Commit `19536cf`.
    **Ergebnis Lauf 32:** Cutscene nach dem Charakter-Editor und die In-Game-Cutscene rendern korrekt, die
    Reinfarben sind weg. In der Welt danach: weißes Bild, kurz Texturen, dann Schwarz (neues offenes Problem 12).
23. **Controller-Removal ohne Connect** (`controller.cpp`, Lauf 31 nach 36 s): SDL meldete „removed“ für Pads 1 und 2,
    verbunden war nur Pad 0 → `EXIT_IF`. Jetzt ignoriert. Commit `30d8dad`.
24. **GPU→CPU-Download > 32 MiB** (`bufferCache.cpp`, Lauf 33 während eines RenderDoc-Captures):
    `BufferCache: download exceeds 32 MiB staging buffer capacity`. → GPU-modifizierte Bereiche werden in Stücke
    ≤ 16 MiB zerlegt und batchweise über den Download-Ring geholt; der Command-Buffer wird pro Batch neu geholt, weil
    das Mappen auf den Ring warten und dabei submitten kann. Lauf 34 kam über die Stelle hinaus (noch nicht committet).

25. **Unbekannte Bits im Tiefen-Deskriptor** (`descriptors.cpp`, Lauf 36): Szenen-Tiefe (2560×1440 D32) wird über
    einen 2D-Deskriptor mit `DEPTH=1216`/`BASE_ARRAY=48` gesampelt → `unsupported sampled depth image … encoding=0`.
    Bild und View sind gültig → Warnung statt Abbruch. Commit `c8ee705`.
26. **Werkzeuge:** zweites F1 beendet ein RenderDoc-Capture (`14bf2db`; ein Guest, der nicht präsentiert, hielt das
    2-Flip-Capture sonst endlos offen – Lauf 34: 30 min, RAM voll). `Present heartbeat`-Zeile (max. 1/s) zeigt in
    jedem Log, ob und wie schnell der Guest präsentiert; ATRAC9-Init-Meldungen gedeckelt (vorher Millionen Zeilen
    über die gemeinsame Log-Sperre, Guest-Log > 1 GB) – Commit `e076806`.
27. **NaN-Tracer `KYTY_NAN_TRACE=<Shader-Hash>`** (`c25f9e0`): instrumentiert einen Shader – nach jeder F32-
    Instruktion markiert ein verzweigungsfreier Store den Slot, wenn das Ergebnis NaN (bzw. ±Inf) ist, obwohl kein
    Float-Operand es war. BitCasts zählen nur, wenn die Bits direkt aus Load/Sample/ReadConst kommen. Der Emitter
    druckt eine Legende (`nan-trace legend: slot, op, Guest-pc-Bereich des Blocks`) nach stderr, der Host liest den
    Trace-Buffer (neue Binding-Art `NanTrace`) 1×/s und loggt die Menge aktiver Quellen bei jeder Änderung.
    Hintergrund: RenderDocs `DebugThread` stürzt bei Shadern mit Subgroup-Ops ab.
    Befund im Nebel-Lichtshader `0xf0dc79c3467d5e0b`: (a) Rückprojektion ins Vorbild – `V_RCP_F32` (1/w = Inf),
    `V_LOG_F32` (NaN) – vom Spiel per `V_CMP_CLASS_F32 …, 56` (isfinite) und geordneten Vergleichen abgefangen;
    (b) Licht-Records (Buffer, Stride 384): Feld +12 einzelner Records ist NaN/Inf, vom Spiel per `V_CMP_NGT_F32`
    (unordered) abgefangen; (c) `1/Σw`, `log(0)` in den Schleifen, ebenfalls geschützt.
28. **Teilweise gemappte Bildquelle** (`bufferCache.cpp`, Lauf 40): `failed to read mapped guest image backing` →
    vorhandenen Teil hochladen, Rest nullen, Fall loggen. Commit `c25f9e0`.

### 2026-09-17 – Durchbruch: die Spielwelt rendert

**Lauf 41** (Binary `c25f9e0`, mit aktivem NaN-Tracer): Tutorial-Tunnel mit volumetrischem Lichtschacht, Laub, Pfütze,
Spielfigur mit Schild und Axt, HUD; Ausrüstungsmenü vollständig (Icons, Attribute, Modell). 1–2 fps, 2427 Flips,
sauberes Ende, VS 208 / PS 347 / CS 743. Entscheidend waren die formatierten Buffer-Stores (Meilenstein 22).
**Offen:** Lauf 37 (gleicher Stand ohne Tracer und ohne Meilenstein 28) wurde in der Welt noch schwarz (HDR-Target
RGB = NaN). Ob der Tracer das Bild beeinflusst (Compiler-Optimierung der NaN-Vergleiche) oder es Zufall/Meilenstein
28 war, klärt Kontrolllauf 42 ohne Tracer.

**Kontrollläufe:** Lauf 42 (gleiche Binary, ohne Tracer) rendert die Welt ebenfalls korrekt und bleibt über 1523 Flips
stabil → der Tracer beeinflusst das Bild nicht. Lauf 43 (gleiche Binary, `--profile`) wurde in der Welt wieder
**schwarz**, Lauf 44 (Tracer + `--profile`) war gut: das Schwarz ist **intermittierend** (Problem 12 bleibt offen,
Timing-Verdacht). Die im guten Lauf aktiven NaN-Quellen des Nebel-Shaders (Rückprojektion, Licht-Record +12/+320,
`rsqrt(0)` in der GI-Probe-Schleife bei Voxeln exakt auf einem Probe-Gitterpunkt) sind alle vom Spiel geschützt
(`V_CMP_CLASS`, unordered Compares, NaN-sicheres `V_MAX`) – die Schwarz-Ursache liegt woanders.

### 2026-09-17 – Erste Performance-Messung (Tracy)

Setup: `winget install wolfpld.tracy` (0.14.1 = Version des eingebundenen Clients), Emulator mit `--profile`
(Tracy on-demand), Aufnahme headless: `tracy-capture -o tunnel.tracy -f -s 25`, Auswertung:
`tracy-csvexport tunnel.tracy > zones.csv` (liegt unter `_Build/logs/run44/`).

Tutorial-Tunnel, 25 s, 43 Presents (**1,7 fps**): `CommandProcessor::Process` 22,7 s (91 % eines Threads) – der
Emulator ist **CPU-gebunden im Command-Processor-Thread**, die GPU wartet.

| Zone | Anzahl | Mittel | Summe |
|---|---|---|---|
| `RenderExecutor::DrawIndex` (in `CpOpDrawIndirect` 242k, `CpOpDrawIndexOffset` 27k) | 270k | 45 µs | 12,1 s |
| `CpOpDispatchDirect` | 107k | 45 µs | 4,8 s |
| `CpOpDispatchIndirect` | 10,8k | 277 µs | 3,0 s |
| `DrawAuto` | 7,6k | 230 µs | 1,8 s |
| `ResolveRenderColor` | 310k | 5,4 µs | 1,7 s |
| `Image::Image` (Vulkan-Image-Erzeugung) | 1,8k | 284 µs | 0,5 s |
| `PrepareBinding` / `FindBuffers` / `RebindBuffers` / `CommitBindings` | je ~650k | ~1 µs | je 0,4–0,9 s |

≈ 6300 Draws + 2500 Dispatches pro präsentiertem Bild × 45 µs ≈ 400–500 ms/Bild. Ziel 60 fps ⇒ ≈ 2 µs pro Befehl
(Faktor ~25). Die instrumentierten Teilschritte erklären nur ~11 der 45 µs pro Draw; der Rest (Deskriptor-
Materialisierung/`SrtWalker`, Textur-Auflösung, Vulkan-Aufrufe, Speicher-Tracking) braucht feinere Zonen.

**Feinprofil (Lauf 45, Zonen aus `7fe86b6`):** Draw 40 µs, davon `RefreshShaders` 26 µs, davon
`MaterializeResources` 25 µs (VS 17 + PS 8); Dispatch 52 µs, davon `MaterializeResources` 26 µs, Binden/Absetzen 26 µs.
`MaterializeResources` = **47 % der gesamten Command-Processor-Zeit** – der Emulator läuft für jeden Draw/Dispatch
den kompletten Deskriptor-Graphen des Shaders ab (SRT-Zeiger, Tabellen, *und* alle Shader-Konstanten fürs
`flattened_srt`).

| Schritt | Commit | Wirkung |
|---|---|---|
| Geprüfter Memo für `MaterializeResources` (gelesene Guest-Worte mitschneiden, bei gleichen User-Data per Vergleich validieren; 256 Einträge/Shader, gehasht) | `31ecde1` | Trefferquote im Tunnel 58 % (PS 72 %, VS 53 %, CS ~2–10 %); Draw 40 → 30 µs. Grenze: der Snapshot enthält auch Konstanten (Matrizen), die sich pro Draw/Bild ändern → 16 % „Speicher geändert“, 26 % „andere User-Data“ |
| Evaluator ohne Allokationen (dichter `EvaluationIndex` pro Plan-Instruktion, gepoolte Arrays mit Generationsstempel statt `unordered_map` pro Aufruf) | `90005a4` | Materialisierung ~25 % schneller (CS 30,7 → 22,2 µs, VS 20,6 → 16,1 µs) |

Messung mit temporären Stoppuhren (nicht committet): **94–96 % der Materialisierungszeit liegen in
`EvaluateRuntimeSources`** (rekursiver Baum-Interpreter, ~40–60 rohe Speicherlesungen und einige hundert
IR-Knoten pro Aufruf); Snapshot-Aufbau und `BuildResourceSpecialization` je nur 1–3 %. Geprüfte („clean“) Lesungen
spielen keine Rolle (≈ 0–1 pro Aufruf). fps bisher unverändert ~2,2 – der große Hebel steht noch aus:

**Plan (nächster Performance-Schritt), zwei Varianten:**
1. *Adressplan-Memo:* beim Aufzeichnen unterscheiden zwischen **strukturellen** Lesungen (Wert fließt als Operand
   in Adressen, Bedingungen, Deskriptoren) und **Blatt-Lesungen** (Wert landet nur in einem Ausgabeslot). Bei einem
   Treffer nur die strukturellen Lesungen vergleichen, die Blatt-Adressen direkt neu lesen und
   `BuildResourceSpecialization` (billig) neu laufen lassen. Erwartung: Trefferquote nahe 100 %, Kosten ~1–2 µs.
2. *Linearer Auswerter:* den Plan einmal in ein topologisch sortiertes Band übersetzen (vorverdrahtete
   Operanden-Indizes), Auswertung als Schleife ohne Rekursion/Visiting-Liste.
Variante 1 verspricht mehr, Variante 2 ist unabhängig davon sinnvoll. Danach: `Dispatch::BindAndEmit` (26 µs),
`CpOpDispatchIndirect` (224 µs), Image-Erzeugung (0,5 ms pro Stück, ~100/s).

**Adressplan-Memo umgesetzt** (`88f5996`, ersetzt den Ergebnis-Memo `31ecde1`): Der Evaluator schneidet seine
Eingaben mit (User-Data-Register, Guest-Worte) und klassifiziert sie – *strukturell* (als Operand benutzt: Bedingungen,
Rechnungen) oder *Blatt* (Wert erreicht nur Ausgabe-Slots, auch durch Weiterreicher `ReadConst`/BitCast/Select/Phi).
Speicher-Eingaben behalten das **Rezept ihrer Adresse** (low/high/offset/records + Immediate), sodass ein Zeiger, der
nur als Basisadresse dient (Konstanten-Blöcke aus dem Ring-Allokator, pro Dispatch neu), sich ändern darf. Ein
Treffer spielt die Eingaben in Auswertungsreihenfolge ab, prüft die strukturellen, berechnet Adressen neu und setzt
die Blatt-Werte in die gespeicherten Ausgaben ein. Tests: `TestRuntimeSourcesMemo(false/true)`.

| Messung (Tunnel, 25 s) | Start (Lauf 44) | jetzt (Lauf 54) |
|---|---|---|
| Presents | 43 (1,7 fps) | **70 (2,8 fps)** |
| verarbeitete Draws / Dispatches | 270k / 107k | 434k / 173k |
| Memo-Trefferquote | – | **98,6 %** (Zwischenstufe ohne Adressrezept: 68 %) |
| `MaterializeResources` VS / CS | 17 / 26 µs | 4,0 / 14,5 µs |
| `DrawIndex` | 45 µs | 21 µs |
| `DispatchDirect` | 45–52 µs | 44 µs |

Nächste Brocken laut Profil: `Dispatch::BindAndEmit` 29 µs × 184k (5,3 s von 25 s), `CpOpDispatchIndirect` 256 µs ×
17,6k (4,5 s), `ExecutePreparedDraw` 9,7 µs × 440k (4,3 s), Rest von `PrepareDrawRenderState`
(`ResolveRenderColorTarget` 5,6 µs), CS-Materialisierung trotz Treffer 14,5 µs (Uniform-Fill-Auswertung, indirekte
Tabellen, Vektor-Kopien).

**Dispatch-Pfad (Läufe 55/56):** feinere Zonen zeigten `DispatchIndirect::ReadArgs` ≈ 0,5 ms (CPU liest GPU-geschriebene
Gruppenzahlen → Seitenfehler → Download) und `Dispatch::PrepareBda` 286 µs (läuft vor jedem DMA-Dispatch über alle
Buffer aller gemappten Bereiche). Fixes: `vkCmdDispatchIndirect` für GPU-dirty Argumente (`d60a7e8`,
`CpOpDispatchIndirect` 256 → 109 µs) und Überspringen des BDA-Abgleichs, solange CPU-Dirty-Epoche, Buffer-
Registrierungen und Mappings stillstehen (`a4c4b54`, Zone verschwindet aus dem Profil; `BindAndEmit` 29 → 21 µs).
Bild unverändert (Sichtprüfung im Tunnel). **fps trotzdem ~2,8** (69 vs. 70 Presents/25 s): die eingesparten ~4 s
wurden in diesem Lauf von langsameren anderen Zonen aufgezehrt – die Lauf-zu-Lauf-Streuung liegt bei ±15 %
(Hintergrundlast, Takt). Per-Thread-Auswertung (`tracy-csvexport -u`, nach `thread` aggregiert): **ein einziger
Command-Processor-Thread** trägt alles (22,9 s von 25 s), der Flip-Thread ist idle.
Konsequenzen: (1) für belastbare Vergleiche künftig eine feste Szene mehrfach messen und Zeit *pro Befehl* statt fps
vergleichen; (2) strukturell hilft nur, die Arbeit pro Draw weiter zu senken (Image-Churn der Nebel-Layer:
~70–200 `Image::Image`/s à 0,5–1,4 ms; `ResolveRenderColorTarget`; `ExecutePreparedDraw`) oder sie auf mehrere
Threads zu verteilen (Deskriptor-Vorbereitung parallel zur Vulkan-Aufzeichnung).

**Weitere Fixes dieser Runde** (`a5b52e8`): Upload-Quelle mit entmapptem Ende (Absturz in `memcpy`, Lauf 50) → nur
den gemappten Teil kopieren; 561-MiB-Image-Upload aus unplausiblem Deskriptor (Lauf 52) → Upload überspringen.

**Schwarz nach dem Reinladen – Stand:** intermittierend; laut Beobachtung ausgelöst durch den **Spawn-Effekt** (weißes
Aufleuchten beim Erscheinen), **Menü auf/zu stellt das Bild wieder her** (Spiel setzt die Verlaufsspeicher zurück).
Gemessen (Lauf 37): HDR-Target RGB = NaN. Nächster Schritt dafür: `--rd`, F1 direkt beim Spawn, zweites F1 beendet,
`rd_nanscan.py`. **Speichern und beenden hängt** (Lauf 49: Bild schwarz, keine SaveData-Aufrufe, Spiel präsentiert
weiter) – kein Spielstand, „Fortsetzen“ daher noch nicht möglich; braucht einen Lauf mit Funktions-Log.

**RenderDoc-Praxis:** Mit `--rd` belegt der Emulator in der Welt 22–24 GB statt ~10 GB; bei 32 GB RAM und weiteren
offenen Programmen lagert Windows aus und ein 2-Flip-Capture dauert > 20 min. Vor Captures alles schließen;
ggf. `renderDoc.cpp` auf 1 Flip umstellen. Der clang-format-Hook (v22.1.3) formatiert ganze Dateien anders als
upstream (z. B. `struct A: B`), was die Diffs aufbläht – vor Upstream-PRs Version angleichen.

**Beobachtung:** Prozessspeicher wächst im Spiel auf > 11 GB (Lauf 20 nach 150 s). Vermutlich Texture-/Buffer-Cache
ohne Verdrängung; für längere Sessions relevant.

## Offene Probleme

| # | Problem | Stand |
|---|---|---|
| 1 | Base-Array-Clamp (`54049c6`) im Spiel verifizieren; Herkunft der Deskriptoren mit `BASE_ARRAY` ≥ Layer klären | Lauf 11 |
| 2 | Kein Ton ab Hauptmenü (Logo-Video hat Ton; SDL-Gerät offen; ATRAC9 dekodiert) | nicht untersucht – vermutlich anderer Ausgabepfad des Spiel-Mixers (`cp11_groupmix`) |
| 3 | **Texturen im Charakter-Editor größtenteils schwarz** (nur einige Rüstungsteile korrekt). Sichtbare Folge der Null-Fallbacks: Material-Tabelle pc `0xfc` nullt denselben Kandidaten 25×, Indexed-Tables nullen Fremdeinträge, Texture-Cache bindet Null bei Alias-Konflikten. Nächste große Baustelle nach dem Spielstart. | beobachtet in Lauf 11 |
| 4 | Linux: Crash im Runtime-Linker (upstream #614) | nicht relevant für uns, Windows primär |
| 5 | **Menü-Video mal weiß, mal schwarz, mal korrekt** (Läufe 17, 19 defekt; 15, 16, 18 korrekt, teils identisches Binary); Cutscene nach dem Editor dann voller Glitches. Bink-Thread-Lebenszyklus ist in guten und schlechten Läufen identisch → das Video wird dekodiert, nur die Übernahme als Textur scheitert (Texture-Cache / Speicherüberwachung, timing-abhängig). Erst ab Lauf 15 (`--redzone`) beobachtet. | nicht untersucht |
| 6 | `k16UScaled`-Texturen werden als Null gebunden (`ab1a305`); Shader-seitige Konvertierung (als `R16_UINT` sampeln, `OpConvertUToF`) fehlt | offen |
| 7 | Depth-Feedback-Pässe laufen ohne Layout-Übergang (`541d11f`); Bildqualität dieser Pässe (Nebel, Partikel) unklar | offen |
| 8 | Indexed-Tables mit unbeschränktem Index nutzen ein festes 32-Einträge-Budget (`2ee0ab1`); Einträge jenseits der echten Tabelle werden genullt, Indizes ≥ 32 fallen auf Kandidat 0 zurück | akzeptiert, beobachten |
| 9 | ~~Lauf 25 komplett schwarz~~ – A/B-Test (Läufe 27/28): `90917f7` ist unschuldig, Ursache war Problem 5 | erledigt |
| 10 | ~~NaN-Ursprung im Nebel~~ – formatierte Buffer-Stores schrieben rohe Float-Bits (Meilenstein 22, `19536cf`) | erledigt, Cutscenes korrekt |
| 11 | Albedo-Texturen (BC1 2048², Material-Pass 174246) liefern an Mip 0 Nullen – bei gestreamten Texturen evtl. nur Mip 0 nicht resident; auf residentem Mip nachprüfen (`rd_probe.py` mit Mip 3–5). | offen |
| 12 | **Welt nach dem Laden: weiß → kurz Texturen → schwarz** (Lauf 32, nach dem Store-Fix). Verdacht: Auto-Exposure-/TAA-History oder weitere formatierte Stores/Loads (10/11-Bit-Float, Swizzle beim Store). Neues Capture nötig, NaN-Scan pro Pass (`rd_fog.py`-Prinzip) | offen |

## Geplante Themen

- **Deskriptoren aus Records generalisieren.** T# funktioniert jetzt, S# (Sampler) und V# (Buffer) aus demselben
  Muster fehlen. Statt drei Sonderfälle: „beliebiger Deskriptor aus beschränktem Record-Zugriff“.
- **Echtes Bindless** (`VK_EXT_descriptor_indexing`): der architektonisch richtige Weg für Material-Heaps und die
  spätere Performance; großer Umbau (Bindings-Layout, Emitter, Host-Descriptor-Management).
- **Shader-Liste mit Vorkompilierung** beim Start (wie DX12-Titel): Bytecode + Spezialisierung + Pipeline-Key
  aufzeichnen, beim nächsten Start alles vorkompilieren, Liste teilbar. Erst sinnvoll, wenn der Recompiler
  stabil ist.
- **Upstream regelmäßig mergen** (sehr hohe Upstream-Aktivität, ResourceTracking wird täglich angefasst).
  Fork-Änderungen klein und rebase-freundlich halten.

## Branches

| Branch | Inhalt |
|---|---|
| `main` | Upstream-Stand + lokale VS-Code-/Toolchain-Anpassungen |
| `fix/indirect-image-material-immediate` | Alle Recompiler-Fixes dieses Logbuchs |
