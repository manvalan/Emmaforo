# Privacy e regole App Store per il controllo remoto

Nota di progetto, ottobre 2026. Non è un parere legale.

## Scelte

- **Nessun account.** Il controllo remoto si abbina via Bluetooth: l'app genera una chiave casuale a 256 bit, la salva nel Portachiavi di quel telefono (solo su quel dispositivo, non sincronizzata) e la scrive sulla lampada. Non serve registrarsi, quindi non serve nemmeno la cancellazione dell'account (linea guida 5.1.1(v)).
- **Nessun identificativo hardware in rete.** I topic MQTT e il client id derivano dalla chiave, non dal MAC o dal numero di serie.
- **Cifratura end-to-end.** Comandi e stato sono sigillati con AES-256-GCM e una sequenza crescente contro la ripetizione. Il broker inoltra soltanto (`server/README.md`).
- **Spento per default.** L'utente lo attiva da Configurazione; "Dimentica abbinamento" cancella la chiave dalla lampada e dal telefono.
- **Rete Wi‑Fi.** La password resta sulla lampada; l'app non la invia a nessun server e non legge l'SSID del telefono (niente permesso di posizione): la lista delle reti la fa la lampada.
- **Pagina HTTP locale tolta** (non aveva password).

## Cosa dichiarare ad Apple

- **Etichetta privacy (App Privacy):** secondo le definizioni di Apple, "raccogliere" significa trasmettere dati fuori dal dispositivo in modo che lo sviluppatore possa accedervi. Con questo schema lo sviluppatore non può leggere niente, quindi la risposta attesa è "Dati non raccolti". Va ricontrollata quando arriveranno Alexa e Google.
- **Esportazione della crittografia:** l'app usa solo la crittografia del sistema operativo (CryptoKit, TLS di Network.framework). Per Apple questo rientra nelle esenzioni; nella domanda di conformità si risponde di conseguenza. Va confermato al momento dell'invio.
- **Rete locale e Bluetooth:** le descrizioni d'uso esistono già nel progetto.
- **Funzioni nascoste:** controllo remoto e registro batteria per ora sono solo nelle build Debug (o con `EMMAFORO_REMOTE` / `EMMAFORO_BATTERY_LOG`). Quando il server sarà attivo, il controllo remoto potrà entrare nella build pubblica come funzione visibile e documentata.

## Cosa resta da fare

- Mettere online il broker con la configurazione in `server/`.
- Provare su una scheda vera: abbinamento, comando da fuori casa, TWT e controllo a intervalli, con il registro batteria per misurarne il costo.
