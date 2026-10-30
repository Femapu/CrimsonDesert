# Dalla sessione locale alla sessione cloud (30/10/2026)

Branch `local/test-in-gioco` (da `claude/crimson-desert-handoff-to1i0s` @ `7666317`), tutto pushato.
Dettagli: `docs/recon/test_locali.md`. Bug: `docs/dev/BUGS.md`. Uso di Stefano: 12%.

## Fatto sul PC di Stefano
- **Ambiente**: gioco Steam con hotfix del 30/10 (exe già in `kKnownExes`). DMM non tocca archivi
  né texture; il vecchio `Seasons.asi` non viene caricato (`LoadFromScriptsOnly=1`). Verifica
  d'integrità Steam non fatta.
- **Strumenti**: artifact CI della build 9 (sha256 dello zip = digest di GitHub) in `dist-ci/`.
- **Tappa 1, verifica inventario**: 3686/3688 OK, 0 mancanti, 0 errori di decodifica o header,
  0 shadowed dal DLC. Il DLC non ha vegetazione propria (solo tile di settore del terreno, oggetti e
  personaggi).
- **Tappa 2, anteprime vere**: 52 texture (3 per profilo), fitter senza nessun mip oltre la
  dimensione originale, mai servito il fitBias. Inverno spoglio e terreno buoni; primavera quasi
  uguale all'estate e fine estate debole sugli alberi; salvia viola in `grass` non cambia mai; un
  tile `terrain_color` quasi nero già in vanilla.
- **Tappa 3, in gioco**: `A.qsp` con fit 100% (2971 texture, 726 MB, 43 s), `--check-pack` OK.
  In gioco circa 5000 record serviti, 0 rifiutati, 0 errori, nessun rallentamento dello streaming;
  menu F5 funzionante con frame generation XeSS attiva. Stefano: autunno ben visibile, estate
  corretta, inverno poco diverso (per B3, vedi sotto). Il personaggio non cambia (atteso).
- **Tappa 4**: saltata. Non verificati: blocco dell'input, FG spenta, persistenza dopo riavvio.
- **Tappa 5 (probe)**: non iniziata.

## Per la sessione cloud
1. **B1**: inventario da riallineare al hotfix (2 erbe del terreno con offset spostato); meglio se il
   generatore rilegge l'offset dal `0.pamt` vivo quando il resto coincide.
2. **B2**: 2 normal map classificate come colore (`deciduous`); filtro sul suffisso nel
   classificatore e rete di sicurezza nel generatore.
3. **B3**: se lo stadio non ha pacchetto resta attivo il precedente, mentre il log dice "vanilla";
   `Active` nell'ini viene scavalcato subito dal calendario.
4. Ricette da tarare: primavera e fine estate più distinte sugli alberi; salvia e simili in `flower`.
5. Inventario: valutare l'esclusione dei tile neri e l'inclusione dei tile di colore del DLC.
6. Log: una riga INFO per apertura/chiusura del menu F5 e per lo stato del blocco input.
7. Domanda: state toccando `src/win/probe/` o `probes.ini`? Se no, la sessione locale fa subito la
   tappa 5, che vi serve per gli hook di meteo, neve, luce e durata del giorno.

## Dopo il vostro lavoro (sessione locale)
Nuovo artifact, ripetere la verifica dell'inventario, generare `A.qsp` e `W.qsp`, riprovare in gioco
il cambio Autunno/Inverno.
