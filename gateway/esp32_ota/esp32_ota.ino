/*
 * ESP32-S3 UNO  —  OTA gateway for the STM32 bootloader
 *
 * What this is
 * ------------
 * The board joins two networks that do not speak to each other: WiFi
 * on one side, the STM32's update protocol on the other. A firmware
 * image is uploaded from a browser, held in RAM, then transferred to
 * the STM32 frame by frame exactly as flash.py would do it over a
 * cable.
 *
 * The STM32 bootloader is unchanged. It sees framed, sequenced,
 * CRC-checked messages arriving on USART1 and has no way to tell
 * whether they came from a PC or from here — which is the point.
 * The protocol layer was written to be transport-agnostic; this is
 * what proves it rather than asserting it.
 *
 * Gateway, not bridge
 * -------------------
 * An earlier and simpler design forwarded bytes between a TCP socket
 * and the UART, leaving flash.py on the PC to run the transfer. It
 * works, and it puts a wireless link in the middle of an
 * acknowledged, timed protocol: every ACK makes a round trip over
 * WiFi, so one lost packet fires a timeout in the middle of a
 * transfer already writing to flash.
 *
 * Receiving the whole image first decouples the two sides. A dropped
 * connection during the upload costs nothing — the STM32 transfer has
 * not started. Once it does, it runs locally at a fixed rate with no
 * network in the loop.
 *
 * Protocol implementation
 * -----------------------
 * The framing below is a second implementation of PROTOCOL.md, from
 * the specification rather than from the Python source. That was the
 * useful part of the exercise: a spec that only its author can
 * implement is not a spec.
 *
 * Wiring
 * ------
 *   STM32 PA9  (USART1 TX, CN10 pin 21)  ->  ESP32 IO18
 *   STM32 PA10 (USART1 RX, CN10 pin 33)  <-  ESP32 IO17
 *   STM32 GND         (CN10 pin 9)       <-> ESP32 GND
 *
 * No supply rail between the boards; each has its own USB. The common
 * ground is mandatory — without a shared reference the link works
 * intermittently, which is harder to diagnose than not working at all.
 *
 * GPIO 33..37 are taken by the octal PSRAM on the N16R8 module. They
 * appear on the header, and using them stops the board from booting.
 */

#include <WiFi.h>
#include <WebServer.h>

/* ---------------------------------------------------------------- */
/* Configuration                                                     */
/* ---------------------------------------------------------------- */

#define AP_SSID             "STM32-OTA"
#define AP_PASSWORD         "bootloader"    /* 8 characters minimum */

/*
 * Security posture, stated rather than assumed.
 *
 * The WPA2 passphrase on the access point is the ONLY barrier. The
 * endpoints carry no authentication: anyone who joins the network can
 * upload an image and flash it. That is acceptable for a bench tool on
 * an isolated AP, and it is why the AP is not bridged to any other
 * network.
 *
 * It would NOT be acceptable on a deployed product. Getting there needs
 * a signed image verified by the bootloader before it is marked
 * bootable -- a CRC proves integrity, never provenance -- plus
 * credentials on the endpoints. Neither is implemented.
 */

#define PIN_UART_RX         18              /* to STM32 PA9  (its TX) */
#define PIN_UART_TX         17              /* to STM32 PA10 (its RX) */
#define UART_BAUD           115200

#define MAX_FIRMWARE_SIZE   (200 * 1024)    /* held in RAM */

/* ---------------------------------------------------------------- */
/* Protocol — mirrors shared/protocol.h                              */
/* ---------------------------------------------------------------- */

#define FRAME_MAGIC_0       0xAA
#define FRAME_MAGIC_1       0x55
#define FRAME_HEADER_SIZE   7
#define FRAME_CRC_SIZE      4
#define DATA_BLOCK_SIZE     256
#define MAX_PAYLOAD_SIZE    1024
#define PROTO_VERSION       1

#define CMD_GET_INFO        0x01
#define CMD_START_UPDATE    0x02
#define CMD_DATA            0x03
#define CMD_END_UPDATE      0x04
#define CMD_ABORT           0x05

#define RSP_INFO            0x81
#define RSP_ACK             0x82
#define RSP_NACK            0x83

/* Declared here, above the first function in the sketch, and not down
   with the framing code where it belongs logically. The Arduino
   preprocessor injects generated prototypes immediately before the
   first function definition; anything they mention has to already
   exist at that point. */
struct Frame {
  uint8_t  cmd;
  uint16_t seq;
  uint8_t  data[MAX_PAYLOAD_SIZE];
  uint16_t len;
};

static const char *errorName(uint8_t code)
{
  switch (code) {
    case 0x01: return "ERR_CRC";
    case 0x02: return "ERR_SEQ";
    case 0x03: return "ERR_LENGTH";
    case 0x04: return "ERR_FLASH";
    case 0x05: return "ERR_SIZE";
    case 0x06: return "ERR_SLOT";
    case 0x07: return "ERR_STATE";
    case 0x08: return "ERR_GLOBAL_CRC";
    case 0x09: return "ERR_PROTO_VER";
    default:   return "ERR_UNKNOWN";
  }
}

/* ---------------------------------------------------------------- */
/* CRC32 — the STM32 peripheral's variant                            */
/*                                                                   */
/* Not the same as zlib's: no output reflection, no final XOR. The    */
/* host matches the hardware rather than the reverse, because the     */
/* peripheral computes in four AHB cycles with no CPU cost while an   */
/* ESP32 can absorb any algorithm unnoticed.                         */
/*                                                                   */
/* Verified against silicon: "123456789" -> 0x9B63D02C               */
/* ---------------------------------------------------------------- */

static uint8_t reverseByte(uint8_t b)
{
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
  return b;
}

static uint32_t crc32_stm32(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < len; i++) {
    crc ^= (uint32_t)reverseByte(data[i]) << 24;
    for (int k = 0; k < 8; k++) {
      crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
  }
  return crc;
}

/* ---------------------------------------------------------------- */
/* Log ring buffer                                                   */
/*                                                                   */
/* Bounded. An unbounded String grows until the heap fragments and    */
/* the board resets — a failure that presents as a WiFi problem.      */
/* ---------------------------------------------------------------- */

#define LOG_LINES   80

String   logLines[LOG_LINES];
uint16_t logHead  = 0;
uint32_t logCount = 0;

void logAdd(const String &text)
{
  String line = "[" + String(millis() / 1000) + "s] " + text;
  logLines[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  logCount++;
  Serial.println(line);
}

String logAsText()
{
  String out;
  uint16_t n     = (logCount < LOG_LINES) ? logCount : LOG_LINES;
  uint16_t start = (logCount < LOG_LINES) ? 0 : logHead;

  for (uint16_t i = 0; i < n; i++) {
    out += logLines[(start + i) % LOG_LINES];
    out += "\n";
  }
  return out;
}

/* ---------------------------------------------------------------- */
/* Frame layer                                                       */
/* ---------------------------------------------------------------- */

/* struct Frame is defined up with the protocol constants — see the
   note there. */

static void sendFrame(uint8_t cmd, uint16_t seq,
                      const uint8_t *data, uint16_t len)
{
  uint8_t header[FRAME_HEADER_SIZE];

  header[0] = FRAME_MAGIC_0;
  header[1] = FRAME_MAGIC_1;
  header[2] = cmd;
  header[3] = (uint8_t)(len & 0xFF);
  header[4] = (uint8_t)(len >> 8);
  header[5] = (uint8_t)(seq & 0xFF);
  header[6] = (uint8_t)(seq >> 8);

  /* The CRC covers CMD, LENGTH, SEQ and DATA — not the MAGIC, which
     validates itself by being recognised at all. Covering LENGTH is
     the part that matters: an unprotected length field is a classic
     overflow vector. */
  uint8_t body[5 + MAX_PAYLOAD_SIZE];
  memcpy(body, &header[2], 5);
  if (len) memcpy(body + 5, data, len);

  uint32_t crc = crc32_stm32(body, 5 + len);

  uint8_t trailer[FRAME_CRC_SIZE] = {
    (uint8_t)(crc & 0xFF),
    (uint8_t)((crc >> 8) & 0xFF),
    (uint8_t)((crc >> 16) & 0xFF),
    (uint8_t)((crc >> 24) & 0xFF)
  };

  Serial1.write(header, FRAME_HEADER_SIZE);
  if (len) Serial1.write(data, len);
  Serial1.write(trailer, FRAME_CRC_SIZE);
  Serial1.flush();
}

/*
 * Reads one frame, or returns false on timeout.
 *
 * The header comes first because LENGTH is inside it: there is no way
 * to know how many bytes to wait for before reading it. Same reason
 * the bootloader's receiver is a state machine rather than a blocking
 * read.
 */
static bool readFrame(Frame &f, uint32_t timeoutMs)
{
  /* Elapsed-time form, not a precomputed deadline.
   *
   * 'millis() + timeoutMs' overflows after roughly 49.7 days of
   * uptime, and every comparison against it then fails immediately:
   * frame reads would time out instantly, forever, on a gateway that
   * had simply been left powered on. shared/systick.h documents this
   * exact trap and labels the additive form DO NOT WRITE THIS.
   *
   * Unsigned subtraction is modular, so (millis() - start) gives the
   * true elapsed time across a rollover. */
  const uint32_t start = millis();
  #define TIMED_OUT()  ((millis() - start) >= timeoutMs)
  uint8_t  window[2] = {0, 0};

  /* Hunt for the preamble. Stray bytes before a frame are ignored
     rather than treated as an error — a reset message, a leftover
     character, anything. */
  while (!TIMED_OUT()) {
    if (!Serial1.available()) { delay(1); continue; }
    window[0] = window[1];
    window[1] = (uint8_t)Serial1.read();
    if (window[0] == FRAME_MAGIC_0 && window[1] == FRAME_MAGIC_1) break;
  }
  if (TIMED_OUT()) return false;

  uint8_t head[5];
  for (int i = 0; i < 5; i++) {
    while (!Serial1.available()) {
      if (TIMED_OUT()) return false;
      delay(1);
    }
    head[i] = (uint8_t)Serial1.read();
  }

  f.cmd = head[0];
  f.len = (uint16_t)(head[1] | (head[2] << 8));
  f.seq = (uint16_t)(head[3] | (head[4] << 8));

  /* Plausibility check before any buffering, exactly as the
     bootloader does it. A corrupted LENGTH must not make us wait for
     sixty thousand bytes. */
  if (f.len > MAX_PAYLOAD_SIZE) return false;

  for (uint16_t i = 0; i < f.len; i++) {
    while (!Serial1.available()) {
      if (TIMED_OUT()) return false;
      delay(1);
    }
    f.data[i] = (uint8_t)Serial1.read();
  }

  uint8_t trailer[4];
  for (int i = 0; i < 4; i++) {
    while (!Serial1.available()) {
      if (TIMED_OUT()) return false;
      delay(1);
    }
    trailer[i] = (uint8_t)Serial1.read();
  }

  uint32_t received = (uint32_t)trailer[0]
                    | ((uint32_t)trailer[1] << 8)
                    | ((uint32_t)trailer[2] << 16)
                    | ((uint32_t)trailer[3] << 24);

  uint8_t body[5 + MAX_PAYLOAD_SIZE];
  memcpy(body, head, 5);
  if (f.len) memcpy(body + 5, f.data, f.len);

  return crc32_stm32(body, 5 + f.len) == received;
}
#undef TIMED_OUT

/*
 * Sends a frame and waits for the reply, retransmitting on failure.
 *
 * Retransmission is safe because the bootloader is idempotent:
 * reprocessing a frame it has already handled resends the
 * acknowledgement without rewriting flash. That property is what
 * makes this loop correct rather than merely hopeful.
 */
static bool exchange(uint8_t cmd, uint16_t seq,
                     const uint8_t *data, uint16_t len,
                     Frame &reply, int retries = 3,
                     uint32_t timeoutMs = 1500)
{
  for (int attempt = 0; attempt < retries; attempt++) {
    sendFrame(cmd, seq, data, len);
    if (readFrame(reply, timeoutMs)) return true;
    if (attempt < retries - 1) {
      while (Serial1.available()) Serial1.read();   /* resync */
      delay(50);
    }
  }
  return false;
}

/* ---------------------------------------------------------------- */
/* Firmware buffer                                                   */
/* ---------------------------------------------------------------- */

uint8_t *firmware      = nullptr;
size_t   firmwareLen   = 0;

/* Set when an upload exceeds the buffer; sticky until the next
   UPLOAD_FILE_START. */
static bool uploadOversize = false;
bool     transferBusy  = false;
uint8_t  targetSlot    = 0xFF;

/* ---------------------------------------------------------------- */
/* The transfer itself                                               */
/* ---------------------------------------------------------------- */

static bool runTransfer()
{
  Frame reply;

  if (targetSlot == 0xFF) {
    logAdd("no target slot prepared");
    return false;
  }

  uint8_t freeSlot = targetSlot;

  /* --- 2. pad to the flash write unit --- */
  size_t len = firmwareLen;
  if (len % 8) {
    size_t pad = 8 - (len % 8);
    /* 0xFF is the erased state of a flash cell, so padding with it is
       neutral: the bytes land where the image would have left them. */
    for (size_t i = 0; i < pad; i++) firmware[len + i] = 0xFF;
    len += pad;
    logAdd("padded by " + String(pad) + " bytes (64-bit write unit)");
  }

  uint32_t crc = crc32_stm32(firmware, len);
  logAdd("image " + String(len) + " bytes, CRC 0x" + String(crc, HEX));

  /* --- 3. announce --- */
  uint8_t su[16] = {0};
  su[0]  = (uint8_t)(len);        su[1] = (uint8_t)(len >> 8);
  su[2]  = (uint8_t)(len >> 16);  su[3] = (uint8_t)(len >> 24);
  su[4]  = (uint8_t)(crc);        su[5] = (uint8_t)(crc >> 8);
  su[6]  = (uint8_t)(crc >> 16);  su[7] = (uint8_t)(crc >> 24);
  su[8]  = 0x00; su[9] = 0x00; su[10] = 0x01; su[11] = 0x00;  /* v1.0.0 */
  su[12] = freeSlot;
  su[13] = PROTO_VERSION;

  if (!exchange(CMD_START_UPDATE, 0, su, 16, reply)) {
    logAdd("START_UPDATE got no reply");
    return false;
  }
  if (reply.cmd != RSP_ACK) {
    logAdd("START_UPDATE refused: " +
           String(errorName(reply.len ? reply.data[0] : 0)));
    return false;
  }
  logAdd("transfer accepted, target slot " +
         String((char)('A' + freeSlot)));

  /* --- 4. the blocks --- */
  uint16_t seq  = 1;
  size_t   sent = 0;
  uint32_t t0   = millis();

  while (sent < len) {
    uint16_t n = (uint16_t)((len - sent < DATA_BLOCK_SIZE)
                            ? (len - sent) : DATA_BLOCK_SIZE);

    if (!exchange(CMD_DATA, seq, firmware + sent, n, reply)) {
      logAdd("block " + String(seq) + " got no reply");
      return false;
    }
    if (reply.cmd != RSP_ACK) {
      logAdd("block " + String(seq) + " refused: " +
             String(errorName(reply.len ? reply.data[0] : 0)));
      return false;
    }

    sent += n;
    seq++;

    if ((seq % 8) == 0) {
      logAdd("  " + String(sent) + "/" + String(len) + " bytes");
    }
  }

  float secs = (millis() - t0) / 1000.0f;
  logAdd("transmitted in " + String(secs, 1) + " s (" +
         String((int)(len / (secs > 0 ? secs : 1))) + " B/s)");

  /* --- 5. close --- */
  /* A longer timeout here: the bootloader re-reads the whole image
     from flash and recomputes its CRC before answering. */
  logAdd("verifying (board re-reads flash)...");
  if (!exchange(CMD_END_UPDATE, seq, nullptr, 0, reply, 1, 15000)) {
    logAdd("END_UPDATE got no reply");
    return false;
  }
  if (reply.cmd != RSP_ACK) {
    logAdd("verification failed: " +
           String(errorName(reply.len ? reply.data[0] : 0)));
    return false;
  }

  logAdd("global CRC verified, image marked TESTING");
  logAdd("board will reboot and run it on trial");
  return true;
}

/* ---------------------------------------------------------------- */
/* Web interface                                                     */
/* ---------------------------------------------------------------- */

WebServer server(80);

void handlePrepare()
{
  if (transferBusy) {
    server.send(409, "text/plain", "transfer running");
    return;
  }
  
  targetSlot = 0xFF;
  Frame reply;

  logAdd("triggering OTA reset...");
  uint32_t tReset = millis();
  while (millis() - tReset < 3000) {  /* 3 s — guarantees 4+ polls at 75 ms/cycle */
    Serial1.write('U');
    delay(5);
  }
  delay(1500); /* wait for the STM32 to fully reboot into the bootloader */
  while (Serial1.available()) Serial1.read();

  logAdd("GET_INFO");
  if (!exchange(CMD_GET_INFO, 0, nullptr, 0, reply)) {
    logAdd("no answer — is the board in update mode?");
    server.send(500, "text/plain", "no answer");
    return;
  }
  if (reply.cmd != RSP_INFO || reply.len != 12) {
    logAdd("unexpected reply to GET_INFO");
    server.send(500, "text/plain", "unexpected reply");
    return;
  }

  uint8_t protoVer = reply.data[8];
  uint8_t active   = reply.data[9];
  uint8_t freeSlot = reply.data[10];
  uint8_t state    = reply.data[11];

  logAdd("board: proto v" + String(protoVer) +
         "  active slot " + String((char)('A' + active)) +
         "  free slot "   + String((char)('A' + freeSlot)) +
         "  state "       + String(state));

  if (protoVer != PROTO_VERSION) {
    logAdd("protocol mismatch");
    server.send(500, "text/plain", "protocol mismatch");
    return;
  }

  targetSlot = freeSlot;
  server.send(200, "text/plain", String((char)('A' + freeSlot)));
}
void handleLog()  { server.send(200, "text/plain", logAsText()); }

void handleUploadDone()
{
  if (uploadOversize) {
    server.send(413, "text/plain",
                "image too large: maximum " +
                String(MAX_FIRMWARE_SIZE - 8) + " bytes");
    return;
  }
  server.send(200, "text/plain",
              String(firmwareLen) + " bytes ready");
}

void handleUploadData()
{
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    firmwareLen = 0;
    uploadOversize = false;
    logAdd("upload started: " + up.filename);
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    /* Bounded, and REFUSED rather than truncated.
     *
     * Skipping the copy and carrying on, as this did, produces an
     * image that is silently short. Nothing downstream notices: the
     * CRC is computed over what was kept, so it is internally
     * consistent and the STM32 accepts it. A half-firmware then gets
     * installed and only the rollback catches it, three boots later,
     * with nothing reporting the real cause.
     *
     * The flag is sticky for the rest of the upload; handleUploadDone
     * turns it into an HTTP error. */
    if (uploadOversize ||
        firmwareLen + up.currentSize > MAX_FIRMWARE_SIZE - 8) {
      uploadOversize = true;
    } else {
      memcpy(firmware + firmwareLen, up.buf, up.currentSize);
      firmwareLen += up.currentSize;
    }
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (uploadOversize) {
      firmwareLen = 0;          /* refuse it outright */
      logAdd("upload REFUSED: larger than " +
             String(MAX_FIRMWARE_SIZE - 8) + " bytes");
    } else {
      logAdd("upload complete: " + String(firmwareLen) + " bytes");
    }
  }
}

bool flashRequested = false;

void handleFlash()
{
  if (firmwareLen == 0) {
    server.send(400, "text/plain", "no image uploaded");
    return;
  }
  if (transferBusy) {
    server.send(409, "text/plain", "transfer already running");
    return;
  }

  /* The transfer is started from the main loop, not from here. A
     handler that blocked for several seconds would stall the web
     server and the browser would time out mid-transfer. */
  flashRequested = true;
  server.send(200, "text/plain", "starting");
}

/* ---------------------------------------------------------------- */

void setup()
{
  Serial.begin(115200);
  delay(300);

  /* UART1 on free pins. The header also carries RXD/TXD, but those
     belong to UART0 and the USB bridge: using them puts two
     transmitters on one line and produces a silence that looks like
     a hardware fault. */
  Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  firmware = (uint8_t *)malloc(MAX_FIRMWARE_SIZE);
  if (!firmware) {
    Serial.println("FATAL: cannot allocate firmware buffer");
    while (1) delay(1000);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();

  server.on("/prepare", HTTP_GET,  handlePrepare);
  server.on("/log",    HTTP_GET,  handleLog);
  server.on("/flash",  HTTP_GET,  handleFlash);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.begin();

  Serial.println();
  Serial.println("=====================================");
  Serial.print  ("  SSID     : "); Serial.println(AP_SSID);
  Serial.print  ("  Password : "); Serial.println(AP_PASSWORD);
  Serial.print  ("  Open     : http://"); Serial.println(ip);
  Serial.println("=====================================");

  logAdd("gateway ready at " + ip.toString());
  logAdd("upload a .bin, then send it to the STM32");
}

void loop()
{
  server.handleClient();

  if (flashRequested && !transferBusy) {
    flashRequested = false;
    transferBusy   = true;

    logAdd("--- transfer starting ---");
    bool ok = runTransfer();
    logAdd(ok ? "--- transfer succeeded ---"
              : "--- transfer failed ---");

    transferBusy = false;
  }
}
