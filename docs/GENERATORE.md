# Generatore dei pacchetti stagionali (qs_gen)

`qs_gen` gira sul PC di chi installa la mod. Legge i file del gioco (mai li scrive), applica le
ricette stagionali alle texture dell'inventario per uno stadio visivo e scrive un pacchetto
`.qsp` che il plugin serve al gioco al posto delle texture originali, con la stessa dimensione
byte per byte. Il pacchetto contiene dati derivati dai file del gioco: resta sul PC e non va mai
distribuito ne messo nel repository.

Eseguibile: `build\win\qs_gen.exe` (Windows) oppure `build/qs_gen` (Linux), creati da `./build.sh`.

## Comandi

Le cartelle con spazi vanno tra virgolette. Esempi da una cartella che contiene `data\`
(la copia del repository o la cartella della mod):

1. Verifica dell'inventario (da fare dopo ogni patch del gioco):

       qs_gen.exe --game "C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert" --verify-inventory --report verify.txt

   Rilegge ogni voce di `data\vegetation_classes_v1.json` dal `0.pamt` reale (percorso, paz, offset,
   dimensioni), decodifica ogni texture in modo stretto e confronta l'header DDS (formato, misure,
   mip, dimensioni salvate). `--quick` controlla solo l'indice (pochi secondi). Codice di uscita 0 se
   tutto torna, 3 se ci sono differenze (elencate a schermo e in `verify.txt`).

2. Generazione di uno stadio:

       qs_gen.exe --game "C:\...\Crimson Desert" --stage A --out "C:\...\Crimson Desert\bin64\scripts\QuattroStagioni\packs"

   Opzioni utili: `--threads N` (default tutti i core; il risultato e identico con qualsiasi N),
   `--only <regex>` (solo i percorsi che corrispondono, utile per prove rapide),
   `--dry-run` (seleziona e controlla senza scrivere), `--transform tint` (prova dell'intera catena
   con una tinta semplice, senza ricette), `--recipes <cartella>`, `--classes <file>`, `--steps N`.
   A fine lavoro stampa il riepilogo: quante texture rientrano a piena qualita, quante con
   `fitBias`, quante a forza ridotta, quante restano originali, il TASSO DI FIT, MB e tempo.
   Lo stesso testo finisce in `qs_gen_<stadio>.log` nella cartella di uscita.

   Piu stadi in un colpo solo (ogni texture viene letta e decodificata una volta sola):
   `--stage A,A>W.1,W` oppure `--stage @mancanti.txt` (un id per riga, `#` per i commenti).
   Con piu stadi il log si chiama `qs_gen_batch.log`. `--config <QuattroStagioni.ini>` prende dal
   plugin il numero di stadi di transizione (`TransitionSteps`) se `--steps` non e indicato.
   Se dopo una patch una voce dell'inventario e solo spostata dentro il suo `.paz` (stesse dimensioni),
   il generatore usa l'offset del `0.pamt` reale e lo annota nel log (`NOTE ...`) invece di saltarla.

3. Pianificazione (non serve `--game`):

       qs_gen.exe --plan --days 7 --state "<bin64>\scripts\QuattroStagioni\state.ini" --config "<bin64>\scripts\QuattroStagioni.ini" --packs "<bin64>\scripts\QuattroStagioni\packs" --stages-needed-file mancanti.txt

   Elenca gli stadi che possono servire da oggi (`--day D` o il giorno salvato in `state.ini`) per i
   prossimi N giorni, considerando tutte le regioni e la varieta per asset (+-`AssetJitterDays`):
   quali pacchetti ci sono, quali mancano e quanti MB stimati. `--stages-needed-file` scrive gli
   stadi mancanti, da generare poi con `--stage @mancanti.txt`.

4. Elenco degli stadi: `qs_gen.exe --list-stages` (S U L A W e le transizioni come `A>W.2`;
   il file del pacchetto usa `-` al posto di `>`: `A-W.2.qsp`).

5. Controllo di un pacchetto: `qs_gen.exe --check-pack packs\A.qsp --game "C:\...\Crimson Desert"`
   verifica gli hash di tutti i dati e che il pacchetto corrisponda alla versione installata.

## Il "fitter"

Ogni texture sostitutiva deve occupare esattamente quanto l'originale, mip per mip. Se una mip
modificata si comprime peggio dell'originale, il generatore la ricodifica con `fitBias` crescente
(0, 2 ... 10: dati BC piu comprimibili, qualita un po' minore); se ancora non entra riprova l'intera
texture con l'effetto attenuato (forza 0,75, 0,5, 0,25); se nulla entra la texture resta quella
originale. Ogni risultato viene decodificato di nuovo e confrontato byte per byte prima di entrare
nel pacchetto.

## Plugin: sezione [Packs] di QuattroStagioni.ini

    [Packs]
    ; vuoto = automatico: ogni texture usa lo stadio del suo calendario (consigliato)
    ; A, W, A>W.2 o A-W.2.qsp = stadio fisso per tutto il mondo; U = texture originali
    Active=
    ; cartella dei pacchetti, relativa a bin64\scripts (o percorso assoluto)
    Folder=QuattroStagioni\packs
    ; memoria massima per i dati in cache (MB), divisa tra i pacchetti aperti
    CacheMB=64

All'avvio il plugin controlla il pacchetto (integrita, build dell'eseguibile, hash dei `0.pamt`,
dimensioni dei `.paz`). Se qualcosa non torna il pacchetto viene ignorato e il log spiega perche
(di solito: patch del gioco, va rigenerato). Durante il gioco ogni lettura viene verificata a
blocchi di 4 KiB contro gli hash dei byte originali salvati nel pacchetto; nel dubbio i byte del
gioco restano intatti. Nel log, ogni 30 secondi, una riga `pack <stadio>: ...` riassume letture
servite, texture sostituite e quelle lasciate intatte.

## Stadi per texture (pacchetti v2)

I pacchetti restano uno per stadio, ma il plugin ne tiene aperti piu di uno insieme (di solito 2-4:
quelli che servono adesso e nelle prossime ore di gioco) e sceglie lo stadio TEXTURE PER TEXTURE, con
il calendario della sua regione e lo spostamento di qualche giorno proprio di ogni asset: cosi alberi
vicini non cambiano tutti nello stesso istante. Per questo ogni voce del pacchetto (formato v2) salva
regione e seme dell'asset presi dall'inventario (`region` "by_tile" e "world" valgono per ora come
regione temperata). I pacchetti v1 si leggono ancora: le loro texture seguono il calendario temperato.

Se manca il pacchetto dello stadio di una texture, il plugin usa lo stadio disponibile piu vicino
PRIMA nel calendario (es. manca `A-W.2.qsp`: si usa `A-W.1.qsp`); se tornando indietro si arriva
all'estate, la texture resta originale. Il log dice sempre cosa viene servito (`packs: day ...:
open [...]`, `... served (... by a fallback stage) ...; missing stages [...]`). Con `Active=` impostato
lo stadio resta fisso finche non lo si cambia dal menu.
