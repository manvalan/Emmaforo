# Emmaforo da remoto e con Alexa, Google, Apple Casa

Nota di progetto, ottobre 2026. Nessun codice ancora: serve a scegliere la strada.

## Vincoli

- Batteria LP603048 da 900 mAh, che l'utente vede come 100% al 95% reale.
- Da questo firmware il Wi‑Fi è spento per default e si accende solo per scaricare un firmware approvato. Il collegamento normale è il Bluetooth LE.
- La scheda è un ESP32‑C6: oltre a Wi‑Fi 6 e BLE 5 ha una radio IEEE 802.15.4, quindi parla **Thread** (e Zigbee).
- Le schede già consegnate hanno la tabella delle partizioni attuale: due slot OTA da 0x1E0000 (1,875 MB). Un aggiornamento OTA non può cambiare la tabella, quindi qualsiasi firmware futuro deve stare in 1,875 MB.
- L'app 1.0 è in revisione: il servizio GATT `FFF0` resta com'è.

## Il punto chiave

Alexa, Google Home e Apple Casa **non comandano un dispositivo BLE qualsiasi**. Parlano con dispositivi Matter (via Wi‑Fi, Ethernet o Thread) oppure con un cloud del produttore. Una "integrazione BLE con Alexa" diretta non esiste: serve sempre un ponte che sia sempre acceso. La domanda è quale ponte costa meno batteria e meno lavoro.

## Strade possibili

### A. Matter su Thread (consigliata)

La scheda diventa una lampada Matter (tipo *Extended Color Light*: On/Off, livello, colore) con il cluster *Power Source* per la percentuale e lo stato di carica, che le app di casa mostrano da sole.

- **Messa in servizio via BLE.** Matter usa proprio il Bluetooth per l'abbinamento, quindi resta "solo BLE" finché l'utente non la aggiunge alla casa.
- **Il ponte ce l'ha già l'utente.** Echo di 4ª generazione, Nest Hub di 2ª generazione, Nest Wifi Pro, HomePod mini e Apple TV 4K fanno da border router Thread. Il controllo da fuori casa passa dal cloud dell'ecosistema, senza un server nostro.
- **Basso consumo.** Thread ha i dispositivi *sleepy end device*: la radio dorme e interroga il router padre a intervalli. Matter 1.2+ lo formalizza come ICD (Intermittently Connected Device). Il prezzo è la latenza: con un'interrogazione ogni 1–2 s la lampada risponde in un paio di secondi, con intervalli più lunghi si consuma meno. Il registro batteria di questo firmware serve proprio a misurare la differenza invece di indovinarla.
- **Un'implementazione per tutti.** Lo stesso firmware funziona con Alexa, Google, Apple Casa e SmartThings.
- **Aggiornamenti.** Matter ha un suo OTA (provider OTA). Con Thread si potrebbe togliere del tutto Wi‑Fi, HTTP e mDNS, recuperando lo spazio che serve a esp‑matter.

Rischi e costi:

- **Spazio.** esp‑matter con Thread e BLE va verificato in 1,875 MB. Va prototipato su una scheda di prova flashata via USB prima di promettere un OTA alle schede consegnate.
- **Convivenza con il servizio `FFF0`.** Lo stack BLE di Matter e il nostro servizio GATT devono condividere NimBLE. Si fa, ma va provato.
- **Certificazione.** Per sviluppare bastano credenziali di test (le app mostrano "dispositivo non certificato"). Per vendere con i loghi servono l'adesione alla CSA, un Vendor ID, i certificati di attestazione e la certificazione. Costi da verificare con la CSA.
- **App iOS.** Il pulsante "Aggiungi a Casa/Alexa/Google" si fa con il framework pubblico MatterSupport (`MatterAddDeviceRequest`), ammesso dall'App Store.

### B. Ponte BLE con Home Assistant

Un ESP32 con ESPHome in modalità *Bluetooth proxy*, oppure un Raspberry Pi, parla con il servizio `FFF0` e pubblica la lampada in Home Assistant, che la espone ad Alexa e Google (Nabu Casa) o come bridge Matter.

- Nessuna modifica al firmware della lampada e consumo identico a oggi.
- Va bene per uso personale o per chi ha già Home Assistant, non come prodotto per tutti.

### C. Wi‑Fi sempre acceso con cloud Alexa/Google

Skill Smart Home e integrazione cloud‑to‑cloud su un server nostro.

- Scartata: richiede il Wi‑Fi sempre associato e una connessione persistente al cloud. Su 900 mAh è la scelta che consuma di più, ed è l'opposto della decisione di tenere il Wi‑Fi spento.

### D. Il telefono come ponte

L'app ha già gli App Intents, quindi Siri e Comandi Rapidi funzionano oggi via BLE quando il telefono è vicino. Alexa e Google non possono avviare azioni di un'app iOS, e da fuori casa il telefono non raggiunge la lampada. Utile, ma non risolve il "da remoto".

## Licenze (non è un parere legale)

- **Codice.** connectedhomeip ed esp-matter sono Apache 2.0, OpenThread è BSD-3, ESP-IDF è Apache 2.0. Usare e modificare il codice non viola nessuna licenza, basta conservare gli avvisi di copyright.
- **Nomi, loghi e brevetti.** "Matter" e "Thread" sono marchi. Per scriverli sulla scatola o nell'app, o per usare i loghi, il prodotto va certificato: Matter con la CSA, Thread con il Thread Group (la certificazione Matter su Thread richiede anche quella Thread). La licenza sui brevetti della specifica vale tra i membri della CSA. Quindi per **vendere** un prodotto Matter la strada pulita è aderire alla CSA e certificare.
- **Sviluppo e uso personale.** Il prototipo con le credenziali di test non pone problemi.
- **Strada B** (ponte Home Assistant via BLE) non usa né Matter né Thread nella lampada: nessun vincolo.

## Scelta finale (11 ottobre 2026)

Solo l'hardware attuale, nessuna licenza a pagamento: Wi‑Fi sotto USB, TWT con i router Wi‑Fi 6, controllo a intervalli con gli altri. Attivabile dall'utente, spento per default. Implementato nel firmware (`remote_link`), vedi il README. Restano da fare il server (broker MQTT con login) e le integrazioni Alexa Smart Home e Google Home cloud-to-cloud.

## Proposta senza licenze commerciali (superata dalla scelta finale)

Michele ha chiesto una strada semplice che non richieda adesioni o certificazioni a pagamento. Matter e Thread restano fuori dalla lampada.

1. **Siri, subito.** Gli App Intents dell'app funzionano già via BLE quando il telefono è vicino. Nessun vincolo.
2. **Emmaforo Bridge.** Un ESP32 sempre alimentato via USB (anche una scheda Emmaforo senza batteria, con un firmware "ponte") parla BLE con le lampade di casa, usando lo stesso servizio `FFF0`, e si collega in uscita al server `emmaforo.michelebigi.it`. La lampada resta solo BLE e la batteria non cambia.
3. **Alexa e Google dal server.** Una skill Alexa Smart Home e un'integrazione Google Home cloud-to-cloud chiamano il server, che inoltra il comando al ponte. I programmi sviluppatori di Amazon e Google sono gratuiti, senza royalty; la revisione della skill è gratuita. Il bollino "Works with Alexa/Google" è facoltativo. Serve solo rispettare le linee guida sui marchi quando si scrive "funziona con Alexa".
4. **Da remoto.** L'app usa lo stesso server quando la lampada è fuori portata BLE, al posto del vecchio ripiego Wi-Fi.
5. **Home Assistant**, per chi lo usa: un'integrazione open source che parla `FFF0` via BLE. Facoltativa.

Cosa serve costruire: firmware del ponte (BLE centrale + Wi-Fi + MQTT/HTTPS verso il server), un piccolo server con login OAuth per il collegamento account di Alexa e Google, la skill Alexa e il progetto Google Home. Costo ricorrente: solo l'hosting.

## Proposta precedente (Matter, solo come riferimento)


1. **Misurare il consumo attuale** con il registro batteria (scheda di test, build Debug dell'app): BLE inattivo, riposo, lampada accesa a vari livelli, con e senza MAX17048G.
2. **Prototipo Matter su Thread** su una scheda flashata via USB partendo dall'esempio *light* di esp‑matter per ESP32‑C6: dimensione dell'immagine contro 1,875 MB, abbinamento via BLE con Alexa, Google e Apple Casa, consumo con due o tre intervalli ICD.
3. **Se sta nello spazio e i consumi reggono:** firmware unico con servizio `FFF0` per l'app più Matter, cluster Power Source per la batteria, OTA via Matter al posto del Wi‑Fi, e pulsante MatterSupport nell'app.
4. **Se non sta nello spazio:** nuova tabella delle partizioni solo per le schede nuove (flash via USB), e strada B per quelle già consegnate.

Decisione che serve da Michele: procedere con il prototipo del punto 2.
