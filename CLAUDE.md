# QuattroStagioni: istruzioni per Claude

Progetto: mod di stagioni dinamiche per Crimson Desert (PC Steam), di Stefano (GitHub Femapu).
Prima di tutto leggi `HANDOFF.md`: contiene lo stato del lavoro e i prossimi passi.

## Regole di lavoro
- Rispondi in italiano. Mai usare il trattino lungo (em dash); usa virgole, due punti, parentesi.
- Prima di ogni attività che consuma molti token, presenta un piano e chiedi il permesso esplicito.
- Budget: Stefano ha un limite di utilizzo. Lavora a tappe con checkpoint; dopo ogni tappa fermati e chiedi a che percentuale di utilizzo è. Fermati prima del 90%. Gli agenti devono avere compiti delimitati e lavorare in modo parsimonioso.
- Repo PUBBLICO: non committare mai file del gioco né derivati (paz, pamt, dds, texture estratte o modificate, eseguibile, binari di test). Vedi `.gitignore`.
- Codice di terzi: usa solo licenze permissive (MIT/BSD: MinHook, LZ4, pycrimson, CDUMM, CrimsonForge, DetourModKit). Seasons-of-Pywel (Khione95) e CrimsonWeather (Nostyxx) NON hanno licenza: leggerli per idee, mai copiarne codice o dati.
- Sul PC di Stefano: non modificare né cancellare i file del gioco. Installare solo in `bin64\scripts\` e `bin64\winmm.ini`.
