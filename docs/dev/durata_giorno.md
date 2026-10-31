# Durata del giorno: trovare il campo del sole (guida per Stefano)

Obiettivo: alba e tramonto che seguono la stagione (giorni lunghi d'estate, corti d'inverno) senza
toccare la velocità dell'orologio del gioco. Il modulo `daylength` (src/win/apply/daylength_win.cpp,
parte portabile src/core/apply/daylength.cpp) non cambia l'ora: dopo che il gioco ha composto
l'atmosfera riscrive solo il campo del sole con il valore che il gioco stesso avrebbe a un'altra ora
(l'"ora del sole"). In estate l'ora del sole coincide con quella del gioco e non si scrive nulla.

Manca un dato: **dove** sta il sole nell'atmosfera composta. Finché `[daylength]` di `targets.ini` è
spento, il modulo scrive nel log solo l'alba e il tramonto stagionali calcolati.

## 1. Avviare la probe

In `bin64\scripts\QuattroStagioni\probes.ini`:

```ini
[General]
Enabled=1
AllowHooks=0

[probe.layer_atmosphere]
Enabled=1
```

La probe `layer_atmosphere` legge il blocco `[[climate manager + 0x60] + 0x28]` (AtmosphereConstant,
contiene `_sunDirX/Y`, `_moonDirX/Y`, `_earthAxisTilt`, `_latitude`) dal modulo meteo: serve che le righe
`weather: hooks on` compaiano in `QuattroStagioni.log` (anche con `[Weather] Enabled=0`). Fa un dump di
0x400 byte, scrive le dword cambiate e ogni tanto un riepilogo dei float che cambiano negli intervalli
0..1, -1..1, 0..24.

## 2. In gioco

- Caricare una partita all'aperto, in una zona temperata (non nel deserto), e restare fermi.
- Lasciar scorrere almeno 2 o 3 ore di gioco, meglio un giorno intero: aspettare, oppure
  dormire/attendere più volte di qualche ora (ogni salto è utile: la probe vede valori diversi).
- **Annotare l'ora dell'alba e del tramonto visibili** (il sole tocca l'orizzonte), leggendo l'ora
  del gioco nell'interfaccia o nella riga periodica `daylength: ... game hour` del log. Servono per `GameSunrise` e `GameSunset`
  in `[DayLength]` di `QuattroStagioni.ini` (default 5.5 e 19.5, cioè 5:30 e 19:30, da verificare).

## 3. Leggere probe.log

Cercare le righe `probe layer_atmosphere: #0 changed +0x...` e `summary(...) layer_atmosphere #0 floats in [...]`.

**Direzione del sole** (Kind=sundir): 2 o 3 float consecutivi (offset distanti 4 byte) con valori in
-1..1, che
- cambiano in modo liscio e continuo con l'ora (un po' a ogni campionamento, non a scatti);
- si ripetono uguali alla stessa ora del giorno successivo;
- dopo un salto di ore (dormire) cambiano di colpo e poi riprendono a variare piano;
- se sono 3 e formano un vettore unitario, x*x + y*y + z*z vale circa 1.
Attenzione a non confondere il sole con la luna (`_moonDir`, simile ma opposta di notte, ciclo diverso)
o con il vento (cambia anche senza che passi l'ora).

**Un'ora del cielo** (Kind=hour): un float che vale quanto l'ora del gioco (0..24, per esempio 14.5
alle 14:30) e che torna a 0 a mezzanotte. Va bene solo se il gioco lo ricalcola a ogni tick: NON usare
l'orologio vero e proprio (GameTime), che cambierebbe l'ora del mondo.

## 4. Scrivere targets.ini

In `bin64\scripts\QuattroStagioni\targets.ini`, sezione `[daylength]` (indipendente da `[General]
Enabled` dello stesso file):

```ini
[daylength]
Enabled=1
Field=atmosphere:0x1A0:3   ; esempio: tre float da +0x1A0
Kind=sundir
```

oppure `Field=atmosphere:0x1C4` e `Kind=hour`. Formato di Field: `<livello>:<offset>[:<quanti float>]`,
livelli `atmosphere`, `weather`, `manager`; al massimo 4 float.

Con `Kind=sundir` il modulo impara dal gioco il valore di ogni fascia di 15 minuti (96 fasce) e lo
salva in `bin64\scripts\QuattroStagioni\sun_table.ini`, legato al TimeDateStamp dell'exe (con una patch
del gioco si rifà da solo). È un dato locale del tuo PC: non va nel repo (è in `.gitignore`).
All'inizio la tabella è vuota e il modulo non scrive: lasciar scorrere un giorno di gioco (o dormire
a più riprese) per riempirla. Dove mancano fasce vicine lascia il sole del gioco.

## 5. Righe del log da controllare (QuattroStagioni.log)

- `daylength: start in ... field atmosphere:0x1A0:3 sundir` (oppure `not set`): configurazione letta.
- `daylength: seasonal sunrise 7:15, sunset 17:45 (10.50 h of day, phase Winter ...)`: orari stagionali.
- `daylength: sun table loaded (N/96 bins known)` oppure `no sun table yet`.
- `daylength: writing ... after the compose`: il modulo scrive.
- `daylength: active; in use sunrise ..., game hour 16:00 -> sun hour 17:20, table 60/96 bins, N writes`
  (ogni LogIntervalSec): d'inverno, nel pomeriggio, l'ora del sole è più avanti di quella del gioco.
- `daylength: sun table saved (N/96 bins known)`.
- `daylength: DISABLED: <motivo>`: il modulo si è spento da solo. Motivi: il gioco non ricompone il
  campo (offset sbagliato o campo non ricalcolato a ogni tick), valori non finiti, scritture rifiutate,
  campo `Kind=hour` che non torna all'ora del gioco. In tutti i casi il gioco resta com'è.

Nel menu F5, scheda Info, la riga "Durata del giorno" mostra lo stato, l'alba e il tramonto in uso e
quante fasce della tabella sono note.

## Verifiche in gioco

1. Estate (o calendario fermo in estate dal menu): nessuna scrittura, sole identico al gioco.
2. Inverno: all'ora `GameSunrise` del gioco è ancora buio, il sole sorge verso l'alba stagionale e
   tramonta prima; mezzogiorno resta mezzogiorno (spostamenti simmetrici).
3. Spegnere "Durata del giorno" dal menu: il sole torna gradualmente a quello del gioco (RampPerHour).
4. Dormire alcune ore: nessun salto strano del sole dopo il risveglio.
