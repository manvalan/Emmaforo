# Emmaforo

Emmaforo è una lampada a semaforo. Questa repository contiene il firmware della scheda e, come sottomoduli, le app iOS e Android.

- Firmware: `firmware/`
- App iOS: `firmware/app-ios` — https://github.com/manvalan/Emmaforo-iOS
- App Android: `firmware/app-android` — https://github.com/manvalan/Emmaforo-Android
- Sito: https://emmaforo.michelebigi.it

Le app del telefono si aggiornano solo dall’App Store e dal Play Store. Il sito non le sostituisce.

Il firmware della scheda è pubblicato in https://emmaforo.michelebigi.it/firmware/ come file binario e una breve nota di versione. L’app avvisa quando in quella cartella c’è una versione più recente. Non scrive nulla sulla scheda finché non si conferma. Dopo il sì, la scheda lontana dal telefono scarica solo quel file sulla rete Wi‑Fi già salvata, lo applica e si riavvia. Se la nuova immagine non parte, torna a quella precedente. Il Bluetooth resta il collegamento normale. Il Wi‑Fi serve solo al trasferimento. Nome, colori e rete salvata restano nella NVS.

Per scaricare anche le app:

```bash
git clone --recurse-submodules https://github.com/manvalan/Emmaforo.git
```

Copyright Michele Bigi 2026. Apache License 2.0.
