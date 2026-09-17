/*
 * WiFi bridge for the STM32 bootloader protocol.
 *
 * What this is
 * ------------
 * A transparent pipe. Bytes arriving on the TCP socket go out of
 * UART1 to the STM32; bytes arriving on UART1 go back to the socket.
 * Nothing in between is parsed, buffered as a message, or understood.
 *
 * Why it stopped being a web server
 * ---------------------------------
 * The previous version served an HTML page, held the whole image in a
 * 200 KB RAM buffer, and reimplemented the protocol: frame building,
 * CRC32, sequence numbers, the transfer state machine, retries. That
 * made it a THIRD implementation of a protocol that already had two
 * (the bootloader in C, the host in Python), with no test covering it
 * and no way to notice when it drifted.
 *
 * It also could not be driven from a command line, which is what this
 * link is actually for.
 *
 * As a pipe, the gateway has no opinion about the protocol. The host
 * tool talks to the bootloader exactly as it does over a wire, so
 * there is one implementation of framing on each end and none in the
 * middle. tools/flash.py --host <ip> is the whole user interface.
 *
 * What is deliberately NOT here
 * -----------------------------
 * No image buffer: bytes are forwarded as they arrive, so image size
 * is bounded by the slot, not by ESP32 RAM.
 *
 * No CRC and no sequence handling: TCP already guarantees delivery and
 * ordering over the WiFi hop, and the protocol's own CRC still covers
 * the UART hop end to end -- which is the hop where corruption
 * actually happens.
 *
 * No authentication. The WPA2 passphrase on the access point is the
 * only barrier: anyone who joins can flash the board. Acceptable for a
 * bench tool on an isolated AP, which is why the AP is not bridged to
 * any other network. A product would need a signed image verified by
 * the bootloader before it is marked bootable -- a CRC proves
 * integrity, never provenance.
 *
 * Wiring
 * ------
 *   ESP32 GPIO18 (RX)  <-  STM32 PA9  (USART1 TX, CN10 pin 21)
 *   ESP32 GPIO17 (TX)  ->  STM32 PA10 (USART1 RX, CN10 pin 33)
 *   ESP32 GND          <-> STM32 GND  (CN10 pin 9)
 *
 * Usage
 * -----
 *   join WiFi "STM32-OTA", then:
 *     python3 tools/flash.py --host 192.168.4.1 --dir app/
 *     python3 tools/flash.py --host 192.168.4.1 --info
 */

#include <WiFi.h>

#define AP_SSID             "STM32-OTA"
#define AP_PASSWORD         "bootloader"    /* 8 characters minimum */

#define TCP_PORT            3333

#define PIN_UART_RX         18              /* to STM32 PA9  (its TX) */
#define PIN_UART_TX         17              /* to STM32 PA10 (its RX) */
#define UART_BAUD           115200

/* One client at a time. Two hosts flashing the same board at once
   would interleave their frames into nonsense; refusing the second is
   clearer than letting both fail in a way neither can diagnose. */
WiFiServer server(TCP_PORT);
WiFiClient client;

/* Sized to a comfortable multiple of the protocol's 256-byte block.
   Larger buys nothing: the UART is the bottleneck at 115200 baud. */
static uint8_t buf[512];


void setup()
{
  Serial.begin(115200);

  /* UART1 on free pins. The header also carries RXD/TXD, but those
     belong to UART0 and the USB bridge: using them puts two
     transmitters on one line and produces a silence that looks like a
     hardware fault. */
  Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.begin();
  server.setNoDelay(true);      /* a 40 ms Nagle delay per frame would
                                   dominate a request-response protocol */

  Serial.println();
  Serial.println("STM32 OTA bridge");
  Serial.print  ("  SSID     : "); Serial.println(AP_SSID);
  Serial.print  ("  password : "); Serial.println(AP_PASSWORD);
  Serial.print  ("  address  : "); Serial.println(WiFi.softAPIP());
  Serial.print  ("  TCP port : "); Serial.println(TCP_PORT);
  Serial.println();
  Serial.println("  python3 tools/flash.py --host 192.168.4.1 --dir app/");
}


void loop()
{
  /* Accept one client; refuse the rest. */
  if (!client || !client.connected()) {
    WiFiClient incoming = server.available();
    if (incoming) {
      if (client) {
        client.stop();
      }
      client = incoming;
      client.setNoDelay(true);

      /* Drop anything the UART was mid-way through saying. A new
         session must not inherit half a frame from the previous one:
         the host would resynchronise on the magic eventually, but the
         first exchange would fail for a reason that looks random. */
      while (Serial1.available()) {
        Serial1.read();
      }
      Serial.print("client connected: ");
      Serial.println(client.remoteIP());
    }
  }

  if (!client || !client.connected()) {
    return;
  }

  /* socket -> UART */
  int n = client.available();
  if (n > 0) {
    if (n > (int)sizeof(buf)) {
      n = sizeof(buf);
    }
    int got = client.read(buf, n);
    if (got > 0) {
      Serial1.write(buf, got);
    }
  }

  /* UART -> socket */
  n = Serial1.available();
  if (n > 0) {
    if (n > (int)sizeof(buf)) {
      n = sizeof(buf);
    }
    int got = Serial1.readBytes(buf, n);
    if (got > 0) {
      client.write(buf, got);
    }
  }
}
