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

## Stand

| Bereich | Status |
|---|---|
| Boot bis Hauptmenü | ✅ läuft (upstream: crasht) |
| Intro-/Logo-Videos (Bink) | ✅ mit Ton |
| Hauptmenü | ✅ bedienbar, **kein Ton** |
| Neues Spiel → Charakter-Editor | ✅ erreicht, Name/Klasse sichtbar |
| Charakter-Editor → Spielwelt | ❌ noch nicht erreicht; Host-Backend-Abbrüche werden nacheinander in Clamps mit Log umgewandelt |
| Performance | – noch nicht bewertbar |

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

## Offene Probleme

| # | Problem | Stand |
|---|---|---|
| 1 | Base-Array-Clamp (`54049c6`) im Spiel verifizieren; Herkunft der Deskriptoren mit `BASE_ARRAY` ≥ Layer klären | Lauf 11 |
| 2 | Kein Ton ab Hauptmenü (Logo-Video hat Ton; SDL-Gerät offen; ATRAC9 dekodiert) | nicht untersucht – vermutlich anderer Ausgabepfad des Spiel-Mixers (`cp11_groupmix`) |
| 3 | **Texturen im Charakter-Editor größtenteils schwarz** (nur einige Rüstungsteile korrekt). Sichtbare Folge der Null-Fallbacks: Material-Tabelle pc `0xfc` nullt denselben Kandidaten 25×, Indexed-Tables nullen Fremdeinträge, Texture-Cache bindet Null bei Alias-Konflikten. Nächste große Baustelle nach dem Spielstart. | beobachtet in Lauf 11 |
| 4 | Linux: Crash im Runtime-Linker (upstream #614) | nicht relevant für uns, Windows primär |

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
