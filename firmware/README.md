# Firmware Emmaforo

Firmware ESP-IDF della scheda, progetto `emmaforo`, target `esp32c6` come in `sdkconfig.defaults`.

## Collegamenti

Il Bluetooth LE è il collegamento normale. Il Wi‑Fi serve solo a scaricare un firmware nuovo: si accende quando l'app lo segnala, l'utente lo approva e in memoria c'è una rete; in quel caso resta acceso il tempo del download e poi si spegne. Se la rete non si raggiunge entro cinque minuti, o il download fallisce cinque volte con attese crescenti, l'approvazione viene annullata e il Wi‑Fi si spegne; l'utente può approvare di nuovo dall'app.

Un firmware nuovo sta in https://emmaforo.michelebigi.it/firmware/ insieme a una nota di versione. La scheda lo scarica solo se quella versione è stata confermata, solo da quella cartella, e solo sulla rete già salvata. Poi si riavvia. Se la nuova immagine non parte, il bootloader torna a quella precedente. Nome, colori e rete salvata non vengono cancellati.

Il comando della lampada è `destinazione,colore,lampeggio,intervallo,livello`. Il colore può essere `rosso`, `giallo`, `verde`, `spento` oppure `#RRGGBB`. Un colore non riconosciuto non cambia la luce. Rete, nome e colori stanno nella NVS tramite `SettingsStore`.

## Controllo remoto (escluso dal firmware per ora)

Compilato solo con `CONFIG_EMMAFORO_REMOTE` (spento per default): senza, la caratteristica `FFEC` non esiste e il Wi‑Fi resta solo per gli aggiornamenti. Con l'opzione attiva si accende dall'app (caratteristica `FFEC`) e usa la rete Wi‑Fi salvata. La scheda sceglie da sola come usare la radio:

- **Sul caricatore USB** resta associata e collegata al broker: i comandi arrivano subito.
- **A batteria con un router Wi‑Fi 6 che concede il TWT individuale** resta associata ma la radio dorme tra un periodo di servizio e l'altro (10 s per default, `CONFIG_EMMAFORO_REMOTE_TWT_INTERVAL_S`): i comandi arrivano in pochi secondi.
- **A batteria con qualsiasi altro router** si collega ogni N minuti (1–60, default 10), ritira i comandi in coda e spegne il Wi‑Fi.

Il router viene provato la prima volta che la scheda lo raggiunge a batteria. Il risultato resta in NVS insieme al BSSID, quindi un router diverso viene provato di nuovo. Se un router TWT non si raggiunge per 30 s, la scheda riprova dopo un intervallo invece di insistere.

Il collegamento passa da un broker MQTT (`CONFIG_EMMAFORO_REMOTE_BROKER`, default `mqtts://emmaforo.michelebigi.it:8883`, configurazione in `server/`) con sessione persistente, così i comandi inviati mentre la scheda dorme arrivano al controllo successivo.

Privacy: nessun account e nessun identificativo hardware. L'app genera una chiave casuale a 256 bit, la tiene nel Portachiavi e la scrive sulla scheda via Bluetooth (`key:`). Dalla chiave derivano i topic `emmaforo/<id>/cmd` e `emmaforo/<id>/state` e tutti i payload sono sigillati con AES-256-GCM (`remote_crypto.h`), con una sequenza crescente contro la ripetizione. Il broker vede solo topic casuali e dati cifrati. Senza chiave il controllo remoto non si attiva. Dettagli in [docs/privacy-e-app-store.md](docs/privacy-e-app-store.md).

- Comandi: lo stesso testo del Bluetooth, cioè `destinazione,colore,lampeggio,intervallo,livello`, `pause:0|1`, `gentle:0|1`, `riposo`.
- Stato (retained): JSON con percentuale mostrata, carica, USB, lampada, riposo, modo (0 spento, 1 USB, 2 TWT, 3 intervalli), intervallo e firmware.

`FFEC` in scrittura accetta `key:<64 hex>`, `forget`, `on`, `off`, `interval:<minuti>`, `broker:<uri>`, `user:<nome>`, `pass:<segreto>`; la chiave non si rilegge mai. In lettura restituisce `1\tattivo\tminuti\tmodo\trouter\tbroker\tchiave`, con router 0 non provato, 1 senza TWT, 2 con TWT.

La vecchia pagina HTTP locale e `emmaforo.local` sono spente (`CONFIG_EMMAFORO_HTTP_PORTAL`), perché non avevano password.

## Speaker (scheda batch3)

Amplificatore PAM8302: `~SD` su GPIO22 lo accende solo mentre suona, il segnale esce dal modulatore sigma-delta su GPIO23 e passa dal filtro 1 kΩ / 10 nF verso IN+. Campioni a 8 bit con segno a 8 kHz. Si toglie con `CONFIG_EMMAFORO_AUDIO` (menu *Emmaforo*); i pin si cambiano lì.

Numeri dei suoni: `0` nessuno, `1` bip, `2` doppio bip, `3` din-don, `4` allarme, `5` melodia, `16`–`20` i cinque messaggi vocali scritti dall'app. Ogni stato della lampada (`rosso`, `giallo`, `verde`, `spento`, `altro` per ogni altro colore) ha un suono, che parte quando la lampada cambia colore con un comando. I passi di una catena hanno il loro suono.

I messaggi vocali stanno nella partizione `audio` (0x3E0000, 128 KB): cinque posti da 24 576 byte, cioè circa 3 s ciascuno. Le schede aggiornate via OTA tengono la vecchia tabella delle partizioni, quindi senza un flash via USB hanno solo i suoni incorporati (l'ultimo campo della lettura di `FFEA` vale 0).

Caratteristica `FFEA` (testo):

- scrittura `vol:<0-100>`, `map:<stato>:<suono>`, `play:<suono>`, `stop`
- scrittura `clip:begin:<posto 0-4>:<byte>` cancella il posto, poi i dati vanno su `FFE9`, poi `clip:end:<posto>:<crc32 esadecimale>` controlla e salva; `clip:del:<posto>` lo cancella
- lettura `1\tvolume\trosso,giallo,verde,spento,altro\tmaschera messaggi\tms massimi per messaggio\tpartizione`

Caratteristica `FFE9` (scrittura): `u32 offset` little-endian e poi i campioni, fino a 512 byte per scrittura. Il CRC è il CRC-32 standard (quello di `java.util.zip.CRC32`).

## Catene di colori

La lampada passa da sola attraverso fino a 16 colori. Ogni passo sfuma dal colore precedente al suo nel tempo di transizione, resta fermo per la pausa e, quando arriva, suona il suo suono. Con *ripeti* ricomincia dal primo passo. Un comando normale della luce, il riposo o una nuova catena la fermano. L'ultima catena resta in NVS.

Caratteristica `FFEB` (binaria, little-endian):

- scrittura `00` ferma, `02` riparte con la catena salvata
- scrittura `01 flags n` e poi `n` passi da 8 byte: `r g b`, `u16 transizione`, `u16 pausa` (decimi di secondo, fino a 6553,5 s), `suono`. `flags` bit 0 = ripeti
- lettura `attiva passo flags n` e poi gli stessi passi

## Batteria

La cella (LP603048, 900 mAh) non viene mai caricata oltre il 95% circa: il BQ25896 regola a 4,16 V e termina a 64 mA. Quel punto è il 100% che l'utente vede, e ogni percentuale pubblicata via Bluetooth o HTTP è scalata di conseguenza (`battery_policy.h`). Corrente di carica normale 896 mA (1C), gentile 448 mA.

Ricarica sicura: si ferma al 90% mostrato e, quando scende sotto l'80%, riparte con una ricarica molto gentile a 192 mA fino al 90%.

Il fuel gauge MAX17048G (I2C 0x36, stesso bus del BQ25896 a 0x6B) non è montato su tutte le schede. All'avvio viene cercato: se risponde, percentuale, tensione e velocità di carica/scarica vengono da lui, con avvisi a 10%, 3,40 V e 4,30 V; l'ADC del caricatore passa a una conversione per campione. Se non c'è, o smette di rispondere, la scheda usa la stima del BQ25896 come prima, ripartendo dall'ultima percentuale del gauge.

## Registro batteria per le schede di test

Con `CONFIG_EMMAFORO_BATTERY_LOG` (attivo per default, menu *Emmaforo* in `idf.py menuconfig`) la scheda registra un campione ogni 120 s, più uno a ogni cambio di carica, alimentazione o lampada, in un buffer RAM di 3072 campioni (circa quattro giorni) che sopravvive ai riavvii software ma non allo stacco della batteria.

La build Debug dell'app iOS (o una build con `EMMAFORO_BATTERY_LOG` nelle Active Compilation Conditions) mostra in Info la sezione *Registro batteria (test)*: scarica il registro via Bluetooth e lo esporta in CSV e JSON con il foglio di condivisione. La build per l'App Store non contiene questa sezione.

Protocollo, caratteristica `FFED` del servizio `FFF0`:

- lettura: `1\tcampioni\tcapacità\tperiodo_s\tuptime_s\tunix_ora\tavvio\tattivo`
- scrittura `time:<unix>`: ora del telefono, per marcare i campioni successivi
- scrittura `dump`: la scheda invia notifiche `0xB1, u16 indice, campioni da 16 byte` e chiude con `0xBE, u16 campioni, u16 capacità, u16 periodo, u32 uptime, u32 unix, u8 avvio`
- scrittura `clear`: azzera il registro

Formato del campione in `headers/battery_log.h`. La caratteristica batteria `FFF9` ha in coda tre campi nuovi (gauge presente, variazione in 0,1 %/h, avvisi del gauge) che l'app 1.0 ignora.

## Compilazione

Dalla cartella `firmware`, con `IDF_PATH` già impostato:

```bash
idf.py set-target esp32c6
idf.py build
```

`main/idf_component.yml` chiede `espressif/mdns`. Il componente viene scaricato alla build.

Idee per l'uso da remoto e con gli assistenti vocali: [docs/remoto-e-assistenti-vocali.md](docs/remoto-e-assistenti-vocali.md).

Copyright Michele Bigi 2026. Apache License 2.0.
