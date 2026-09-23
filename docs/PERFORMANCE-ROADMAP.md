# Performance-Roadmap für Demon's Souls (Stand 2026-09-23)

Ziel: vom heutigen Stand (~6,3 Bilder pro Sekunde im Tutorial-Tunnel) zu spielbaren 30 und später 60 Bildern pro
Sekunde. Dieses Dokument hält fest, **wohin die Zeit heute geht**, **welche Hebel gemessen wurden** und **welche
Wege es von hier aus gibt** – mit ehrlichen Schätzungen. Grundlage sind die Läufe 108–110 (Tracy-Zonen, 20 s im Tunnel,
ohne Hintergrundlast) und die Sampler-Aufnahme aus Lauf 109.

## 1. Wo die Zeit hingeht

Der Emulator verarbeitet alle GPU-Befehle des Spiels in **einem** Thread (Command-Processor, CP). Dieser Thread ist zu
~90 % ausgelastet und bestimmt die Bildrate. Pro Bild (~160 ms bei 6,3 fps):

| Posten | pro Bild | Anteil | Art |
|---|---|---|---|
| Shader-Suche + Deskriptor-Auflösung (Draws + Dispatches) | ~30 ms | 19 % | reine Berechnung |
| Textur-Cache-Suchen (Render-Targets, Texturen) | ~20 ms | 13 % | Suche mit Seiteneffekten |
| Buffer-Cache (Suchen, Synchronisieren, Konstanten hochladen) | ~30 ms | 19 % | Suche mit Seiteneffekten |
| Vulkan-Befehle aufzeichnen (Deskriptor-Sets, Draw/Dispatch) | ~15 ms | 9 % | Aufzeichnung |
| Warten auf die GPU (Rücklesen von GPU-Ergebnissen) | ~34 ms | 21 % | Synchronisation |
| Rest (Abschicken, Register, Verwaltung) | ~30 ms | 19 % | gemischt |

Pro Bild sind das rund **6200 Draws** (davon ~53 % Wiederholungen, die dank Schnellpfad nur 0,13 µs kosten) und
**2800 Dispatches**. Ein „voller“ Draw kostet ~23 µs, ein Dispatch ~17,5 µs.

**Die unbequeme Rechnung:** Für 30 fps stehen pro Bild 33 ms zur Verfügung – heute brauchen wir ~160 ms. Das ist
Faktor 5. Kein einzelner Posten ist größer als ein Viertel; selbst wenn einer komplett verschwände, blieben >120 ms.

## 2. Was gemessen und verworfen wurde

| Idee | Ergebnis |
|---|---|
| Eigener Thread für die Compute-Warteschlangen | Compute-Warteschlangen = 2,1 s von 18 s. Die meisten Dispatches kommen über die Grafik-Warteschlange. Gewinn ≤ 10 %. |
| Labels erst bei GPU-Fertigstellung schreiben | GPU-Waits fast weg, aber das Spiel pausiert den CP 130 000× pro 25 s an `WAIT_REG_MEM` → langsamer (4,5 statt 6 fps). Als `KYTY_DEFERRED_EOP=1` verfügbar. |
| DCC-Metadaten aus gemerkten Fills ableiten | Ein Compute-Shader des Spiels schreibt die Metadaten selbst; der Merker kann nie gültig sein. |
| Nur die reine Berechnung (Shader + Deskriptoren) auf Worker-Threads | Sie macht ~19 % aus. Selbst perfekt parallelisiert: höchstens ~1,25×. |

## 3. Kleine, sichere Schritte (zusammen geschätzt +10–20 %)

1. **Speicheranforderungen pro Draw vermeiden** (`operator new` 3,4 %): `PreparedBindings` legt pro Stufe und Draw
   fünf `std::vector` an. Wiederverwendbare Puffer im Executor. Aufwand klein, Risiko klein.
2. **`vkQueueSubmit` auf einen eigenen Thread** (5 %): Der CP übergibt fertige Befehlspuffer, ein Submit-Thread reicht
   sie ein. Die Tick-Nummern werden weiter sofort vergeben; Timeline-Semaphoren erlauben Warten vor dem Signal.
   Aufwand mittel, Risiko mittel (Reihenfolge, Herunterfahren).
3. **DCC-Clear-Erkennung auf der GPU** (~7 %): Statt die Metadaten zurückzulesen, prüft ein kleiner Compute-Shader,
   ob ein Bereich komplett „gelöscht“ ist, und löscht das Bild dann selbst. Aufwand mittel bis groß (Formate).
4. **Konstanten-Uploads bündeln**: Dispatches verbringen 4 µs in `RebindBuffers` (Seitenschutz umschalten, Konstanten
   in den Stream-Buffer kopieren). Aufwand mittel, Gewinn unklar – erst genauer messen.

Nicht empfohlen: Express-Kopien auf eine eigene Transfer-Warteschlange legen. Das würde 8 % Warten sparen, aber
Shader schreiben über Zeiger (BDA) in Buffer, ohne dass der Emulator das pro Buffer weiß. Heute schützt die Reihenfolge
der einen Warteschlange davor; auf einer zweiten entstünden seltene, schwer findbare Datenfehler.

## 4. Die großen Wege (je mehrere Sessions)

### A. Parallele Übersetzung mit nebenläufigen Caches
Der CP-Thread dekodiert nur noch PM4 und schreibt pro Draw einen kompakten Zustands-Schnappschuss. Mehrere
Worker übersetzen Draws parallel (Shader, Deskriptoren, Cache-Suchen), ein Aufzeichnungs-Thread setzt die Ergebnisse
in Reihenfolge in Vulkan-Befehle um. Voraussetzung: Textur-, Buffer- und Pipeline-Cache müssen parallele *Suchen*
erlauben (Lesepfad ohne globale Sperre), Neuanlagen und Uploads bleiben serialisiert.
- Möglicher Gewinn: 2–3× auf den übersetzenden Teil, real vielleicht **1,7–2,5×** (→ 10–15 fps).
- Aufwand: groß. Die Caches sind heute durchgehend für einen Thread gebaut (Seiteneffekte beim Suchen:
  Layout-Übergänge, Uploads, Aufräumen).

### B. Übersetzungen über Bilder hinweg wiederverwenden
Das Spiel schickt jedes Bild weitgehend dieselben Draws mit anderen Konstanten. Wie beim Adressplan-Memo für die
Deskriptoren (98,6 % Treffer) könnte ein Memo den *gesamten* übersetzten Draw (Pipeline, gebundene Images/Buffer,
Deskriptor-Set) wiederverwenden, wenn nur Konstanten-Adressen und -Werte gewechselt haben.
- Möglicher Gewinn: groß, wenn die Trefferquote hoch ist – ein Treffer könnte ~3–5 µs statt ~20 µs kosten
  (**2–4×** auf Draws/Dispatches).
- Aufwand: groß, Risiko hoch: Die Gültigkeit hängt an Cache-Zuständen (neu angelegte/verdrängte Images), die sauber
  mitgeprüft werden müssen.

### C. GPU-Synchronisation grundsätzlich verringern
21 % sind Warten auf GPU-Ergebnisse, die das Spiel oder der Emulator zurückliest. Teile davon sind vermeidbar (DCC,
siehe oben), andere nicht ohne Wissen über das Spiel (Abfrage-Ergebnisse, indirekte Argumente).
- Möglicher Gewinn: bis ~1,3×. Aufwand: mittel, verteilt auf viele Einzelfälle.

## 5. Empfehlung

1. Zuerst die kleinen Schritte 1 und 2 (sicher, messbar, ~10 %).
2. Dann **Weg B als Experiment**: messen, wie viele volle Draws pro Bild sich zum Vorbild exakt wiederholen würden
   (Zähler ohne Verhaltensänderung, wie `KYTY_DRAW_STATS`). Liegt die Quote über ~70 %, ist B der größte Hebel;
   liegt sie darunter, ist A der Weg.
3. Weg A nur mit vorher geschriebenem Detailplan für die Caches.

Realistische Erwartung: Mit 1–3 zusammen **10–15 fps**. 30 fps brauchen voraussichtlich A *und* B. 60 fps sind
nach heutigem Stand ein Vorhaben über viele Monate.
