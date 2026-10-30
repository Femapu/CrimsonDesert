# Test locali sul PC di Stefano (branch `local/test-in-gioco`)

Risultati dei passi 1-5 di "Prossimi passi" in `HANDOFF.md`, eseguiti con i file veri del gioco.
Qui vanno solo esiti, conteggi e osservazioni: mai hash, offset, estratti o anteprime dei file del
gioco (repo pubblico). I bug trovati vanno in `docs/dev/BUGS.md`.

## 0. Ambiente (30/10/2026)

- Gioco: Steam, `C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert`, build Steam
  aggiornata il 30/10 alle 10:47. L'exe corrisponde al hotfix del 30/10 già in `kKnownExes`
  (stessa dimensione e timestamp), `meta/0.paver` = 2.04.00 con DLC `dlc_01`.
- Gli archivi del DLC stanno in `dlc/dlc_01/<gruppo>/` (visto nel log del plugin), non solo nelle
  cartelle numerate della radice: da tenere presente per la verifica dell'inventario.
- Cartelle archivio in radice: da 0000 a 0035, senza 0018 e 0034.
- Strumenti: Git 2.56, Python 3.12.10. Gli eseguibili arrivano dall'artifact CI `QuattroStagioni-dist`
  in `dist-ci/` (ignorata da git).

### Mod manager DMM (`Desktop\Desert Mods`), controllo in sola lettura

- `config.json`: nessuna mod di archivio attiva (`activeMods` vuoto), nessuna texture
  (`activeTextures` vuoto). Unica ASI attiva: `Seasons` (il vecchio `Seasons.asi`, in `bin64\`).
- Ultima attività di DMM: 4/10. Il suo backup "vanilla" dei `.pamt` è di quella data, quindi
  precede le patch del 29-30/10: tutti i 37 file (pamt e meta) risultano diversi dal backup, ma
  hanno tutti data di modifica 29/10 sera o 30/10 mattina, cioè gli aggiornamenti Steam. Non sono
  modifiche di DMM.
- `winmm.ini` ha `LoadFromScriptsOnly=1`, quindi `bin64\Seasons.asi` non viene caricato.
- Conclusione: DMM non altera archivi né texture; non serve disattivarlo per i test. Per una
  conferma definitiva basta "Verifica integrità dei file" di Steam prima del passo 1.

### Stato di `bin64\scripts` trovato all'inizio

- È installato `QuattroStagioni.asi` 0.2.0 (fase 2, sostituzione della sola texture di test del
  logo di caricamento), installato il 30/10 alle 10:50.
- Log del 30/10: il controllo di versione è passato con l'exe del hotfix, gli hook sono stati
  installati in circa 170 ms e la texture di test è stata servita correttamente (HIT, patch in
  0,004 ms). Prima del riavvio, con l'elenco versioni vecchio, il plugin si era spento da solo
  come previsto (controllo di versione: MISMATCH).
- Al passo 3 questa installazione va sostituita con quella dell'artifact CI.

## 1. Verifica inventario

Da fare (serve `qs_gen.exe` da `dist-ci/`).

## 2. Anteprime vere

Da fare.

## 3. Primo pacchetto in gioco

Da fare.

## 4. Menu F5 in gioco

Da fare.

## 5. Probe (A1-b)

Da fare.
