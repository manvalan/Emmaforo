# Server per il controllo remoto

Un solo servizio: un broker MQTT (Mosquitto, licenza EPL/EDL) su `emmaforo.michelebigi.it:8883`. Non c'è database, non ci sono account e non c'è codice nostro sul server.

## Installazione (Debian/Ubuntu)

```bash
sudo apt install mosquitto certbot
```

```bash
sudo certbot certonly --standalone -d emmaforo.michelebigi.it
```

```bash
sudo cp mosquitto/emmaforo.conf /etc/mosquitto/conf.d/emmaforo.conf
```

```bash
sudo cp mosquitto/emmaforo.acl /etc/mosquitto/emmaforo.acl
```

```bash
sudo systemctl restart mosquitto
```

Mosquitto deve poter leggere i certificati (per esempio con un hook di certbot che li copia in `/etc/mosquitto/certs` con il proprietario `mosquitto`, aggiornando `certfile` e `keyfile`). La porta 8883 va aperta nel firewall.

## Cosa vede il server

- Nomi di topic casuali (`emmaforo/<32 cifre esadecimali>/cmd` e `/state`), derivati da una chiave che esiste solo sul telefono e sulla lampada.
- Payload cifrati e autenticati con AES-256-GCM. Senza la chiave non si leggono e non si falsificano.
- Gli indirizzi IP durante la connessione TLS, come qualsiasi server; la configurazione non li scrive nei log.

Non vede: nome, email, posizione, numero di serie, indirizzo MAC, rete Wi‑Fi, colori o orari d'uso in chiaro.

## Limiti noti

- Chiunque può collegarsi in modo anonimo e iscriversi con i caratteri jolly ai topic `emmaforo/+/…`: vedrebbe solo identificativi casuali e dati cifrati. Potrebbe però riempire le code con messaggi falsi, che la lampada scarta. Se servisse, il passo successivo è registrare gli identificativi (sempre casuali, nessun dato personale) con il plugin Dynamic Security di Mosquitto.
- Alexa e Google, quando li aggiungeremo, avranno bisogno di una chiave propria per lampada, creata solo se l'utente collega l'account. Andrà indicato nell'informativa sulla privacy.
