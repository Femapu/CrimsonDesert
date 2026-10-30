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

3. Elenco degli stadi: `qs_gen.exe --list-stages` (S U L A W e le transizioni come `A>W.2`;
   il file del pacchetto usa `-` al posto di `>`: `A-W.2.qsp`).

4. Controllo di un pacchetto: `qs_gen.exe --check-pack packs\A.qsp --game "C:\...\Crimson Desert"`
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
    ; stadio attivo all'avvio (es. A, W, A>W.2) o nome file (A-W.2.qsp); vuoto = nessuno
    Active=A
    ; cartella dei pacchetti, relativa a bin64\scripts (o percorso assoluto)
    Folder=QuattroStagioni\packs
    ; memoria massima per i dati in cache (MB)
    CacheMB=64

All'avvio il plugin controlla il pacchetto (integrita, build dell'eseguibile, hash dei `0.pamt`,
dimensioni dei `.paz`). Se qualcosa non torna il pacchetto viene ignorato e il log spiega perche
(di solito: patch del gioco, va rigenerato). Durante il gioco ogni lettura viene verificata a
blocchi di 4 KiB contro gli hash dei byte originali salvati nel pacchetto; nel dubbio i byte del
gioco restano intatti. Nel log, ogni 30 secondi, una riga `pack <stadio>: ...` riassume letture
servite, texture sostituite e quelle lasciate intatte.
