# Firmware Emmaforo

Firmware ESP-IDF della scheda, progetto `emmaforo`, target `esp32c6` come in `sdkconfig.defaults`.

Il Bluetooth è il collegamento normale. Il Wi‑Fi parte solo se non c’è una sessione Bluetooth e in memoria c’è una rete. Quando il Bluetooth torna, il Wi‑Fi si spegne. Durante il trasferimento di un firmware già confermato il Wi‑Fi resta acceso fino alla fine.

Un firmware nuovo sta in https://emmaforo.michelebigi.it/firmware/ insieme a una nota di versione. La scheda lo scarica solo se quella versione è stata confermata, solo da quella cartella, e solo sulla rete già salvata. Poi si riavvia. Se la nuova immagine non parte, il bootloader torna a quella precedente. Nome, colori e rete salvata non vengono cancellati.

Il comando della lampada è `destinazione,colore,lampeggio,intervallo,livello`. Il colore può essere `rosso`, `giallo`, `verde`, `spento` oppure `#RRGGBB`. Un colore non riconosciuto non cambia la luce. Rete, nome e colori stanno nella NVS tramite `SettingsStore`.

Dalla cartella `firmware`, con `IDF_PATH` già impostato:

```bash
idf.py set-target esp32c6
idf.py build
```

`main/idf_component.yml` chiede `espressif/mdns`. Il componente viene scaricato alla build.

Copyright Michele Bigi 2026. Apache License 2.0.
