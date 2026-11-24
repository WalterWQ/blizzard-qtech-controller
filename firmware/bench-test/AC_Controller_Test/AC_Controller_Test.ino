/*
  AC_Controller_Test  -  bench test firmware for the AC Controller board (v3)
  ---------------------------------------------------------------------------
  Tests the controller on its own, powered from USB-C. No power board needed.
  No extra libraries needed - everything is talked to directly.

  ARDUINO IDE SETUP (once)
    1. File > Preferences > "Additional boards manager URLs", add:
         https://espressif.github.io/arduino-esp32/package_esp32_index.json
    2. Tools > Board > Boards Manager: install "esp32 by Espressif Systems".
    3. Tools menu settings:
         Board ............ ESP32S3 Dev Module
         USB CDC On Boot .. Enabled          <- needed to see the menu over USB
         Flash Size ....... 16MB (or 8MB for an N8R2 module)
         PSRAM ............ OPI PSRAM (N16R8)  /  QSPI PSRAM (N8R2)
         Upload Mode ...... UART0 / Hardware CDC
    4. Take the J10 jumper OFF before uploading.
    5. First upload only: hold BOOT (SW1), tap RESET (SW2), let go of BOOT,
       then click Upload. After that, normal Upload usually works.
    6. Tools > Serial Monitor, 115200 baud, "Newline". Type a menu number.

  Results are shown in the Serial Monitor and summarised on the LCD.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

// ------------------------------------------------------------ pin map (from the PCB)
#define PIN_SDA        1
#define PIN_SCL        2
#define PIN_SAFE_GPIO  3      // AND-ed with watchdog -> SAFE_EN (J30 pin 12)
#define PIN_BOOT       0      // SW1
#define PIN_LCD_RS     10
#define PIN_LCD_E      11
#define PIN_LCD_D4     12
#define PIN_LCD_D5     13
#define PIN_LCD_D6     14
#define PIN_LCD_D7     21
#define PIN_LCD_BL     38     // backlight MOSFET Q2, HIGH = on
#define PIN_PRN_TX     39
#define PIN_KEY_INT    40
#define PIN_BUZZER     41     // buzzer MOSFET Q1, HIGH = on
#define PIN_WDT        42     // TPS3823 WDI - must toggle at least every ~1 s
#define PIN_PRN_RX     47
#define PIN_RGB        48     // WS2812B

#define LCD_COLS 16           // change to 20 if your display is 20 characters wide

// ------------------------------------------------------------ I2C addresses
#define A_MCP_KEYS   0x20     // U3  keypad, heater, fan, E-stop, HP switch, printer busy
#define A_MCP_EXP    0x21     // U19 RLY11, RLY12, J43 expansion
#define A_NAU        0x2A     // U15 load cell
#define A_ADS1       0x48     // U9  tank NTC, P1, P2, P3
#define A_ADS2       0x49     // U13 CT pump, CT comp, 12V monitor, 5V monitor
#define A_ADS3       0x4B     // U17 leak, P4, P5, sensor 5V
#define A_FRAM       0x50     // U10
#define A_RTC        0x68     // U12

// ------------------------------------------------------------ relay outputs
struct Out { const char *name; int j30pin; int kind; int pin; };
// kind 0 = ESP GPIO, 1 = U3 port B bit, 2 = U19 port A bit
const Out OUTS[] = {
  {"RLY1  (valve 1)",   5, 0, 4},  {"RLY2  (valve 2)",   6, 0, 5},
  {"RLY3  (valve 3)",   7, 0, 6},  {"RLY4  (valve 4)",   8, 0, 7},
  {"RLY5  (valve 5)",   9, 0, 15}, {"RLY6  (valve 6)",  10, 0, 16},
  {"RLY7  (valve 7)",  13, 0, 17}, {"RLY8  (valve 8)",  14, 0, 18},
  {"RLY9  (compressor)",15, 0, 8}, {"RLY10 (vac pump)", 16, 0, 9},
  {"FAN_RLY",          17, 1, 5},  {"RLY11 (valve 9)",  21, 2, 0},
  {"RLY12 (valve 10)", 22, 2, 1},  {"HEATER_RLY",       25, 1, 0},
};
const int NOUTS = sizeof(OUTS) / sizeof(OUTS[0]);

// ------------------------------------------------------------ watchdog feeder
volatile bool wdtFeed = true;
void wdtTask(void *) {
  bool s = false;
  for (;;) {
    if (wdtFeed) { s = !s; digitalWrite(PIN_WDT, s); }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ------------------------------------------------------------ small I2C helpers
bool i2cPresent(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }
bool wr8(uint8_t a, uint8_t r, uint8_t v) {
  Wire.beginTransmission(a); Wire.write(r); Wire.write(v); return Wire.endTransmission() == 0;
}
int rd8(uint8_t a, uint8_t r) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(a, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
bool rdN(uint8_t a, uint8_t r, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(a, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// ------------------------------------------------------------ LCD (HD44780, 4-bit, write only)
void lcdNib(uint8_t n) {
  digitalWrite(PIN_LCD_D4, n & 1); digitalWrite(PIN_LCD_D5, (n >> 1) & 1);
  digitalWrite(PIN_LCD_D6, (n >> 2) & 1); digitalWrite(PIN_LCD_D7, (n >> 3) & 1);
  digitalWrite(PIN_LCD_E, HIGH); delayMicroseconds(2);
  digitalWrite(PIN_LCD_E, LOW);  delayMicroseconds(60);
}
void lcdByte(uint8_t b, bool data) {
  digitalWrite(PIN_LCD_RS, data); lcdNib(b >> 4); lcdNib(b & 0x0F);
}
void lcdCmd(uint8_t c) { lcdByte(c, false); if (c < 4) delay(3); }
void lcdInit() {
  int p[] = {PIN_LCD_RS, PIN_LCD_E, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7};
  for (int i : p) { pinMode(i, OUTPUT); digitalWrite(i, LOW); }
  delay(60);
  lcdNib(3); delay(5); lcdNib(3); delayMicroseconds(150); lcdNib(3); lcdNib(2);
  lcdCmd(0x28); lcdCmd(0x0C); lcdCmd(0x06); lcdCmd(0x01);
}
void lcdLine(int row, const char *s) {
  lcdCmd(0x80 | (row ? 0x40 : 0x00));
  int i = 0;
  for (; s[i] && i < LCD_COLS; i++) lcdByte(s[i], true);
  for (; i < LCD_COLS; i++) lcdByte(' ', true);
}

// ------------------------------------------------------------ RGB LED
void rgb(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_RGB, r, g, b);
#else
  neopixelWrite(PIN_RGB, r, g, b);
#endif
}

// ------------------------------------------------------------ MCP23017
// registers (BANK=0): IODIRA 00, IODIRB 01, GPPUA 0C, GPPUB 0D, GPIOA 12, GPIOB 13, OLATA 14, OLATB 15
uint8_t olatU3B = 0, olatU19A = 0;
void mcpSetup() {
  // U3: port A = keypad (inputs, pull-ups). Port B: GPB0 heater OUT, GPB5 fan OUT, rest inputs
  wr8(A_MCP_KEYS, 0x14, 0x00); wr8(A_MCP_KEYS, 0x15, 0x00);
  wr8(A_MCP_KEYS, 0x00, 0xFF); wr8(A_MCP_KEYS, 0x0C, 0xFF);
  wr8(A_MCP_KEYS, 0x01, 0xDE); wr8(A_MCP_KEYS, 0x0D, 0xDE);
  // U19: GPA0/GPA1 = RLY11/RLY12 OUT, everything else input with pull-up
  wr8(A_MCP_EXP, 0x14, 0x00);
  wr8(A_MCP_EXP, 0x00, 0xFC); wr8(A_MCP_EXP, 0x0C, 0xFC);
  wr8(A_MCP_EXP, 0x01, 0xFF); wr8(A_MCP_EXP, 0x0D, 0xFF);
}
void setOut(int i, bool on) {
  const Out &o = OUTS[i];
  if (o.kind == 0) digitalWrite(o.pin, on);
  else if (o.kind == 1) { olatU3B = on ? (olatU3B | (1 << o.pin)) : (olatU3B & ~(1 << o.pin)); wr8(A_MCP_KEYS, 0x15, olatU3B); }
  else { olatU19A = on ? (olatU19A | (1 << o.pin)) : (olatU19A & ~(1 << o.pin)); wr8(A_MCP_EXP, 0x14, olatU19A); }
}
void allOff() { for (int i = 0; i < NOUTS; i++) setOut(i, false); }

// Keypad: drive one line low at a time, see which other line follows it.
// Returns e.g. "KP1+KP5" or "" if nothing pressed.
String keyScan() {
  String res = "";
  for (int i = 0; i < 8 && res == ""; i++) {
    wr8(A_MCP_KEYS, 0x14, 0x00);
    wr8(A_MCP_KEYS, 0x00, (uint8_t)~(1 << i));      // line i = output low
    delayMicroseconds(200);
    int g = rd8(A_MCP_KEYS, 0x12);
    if (g < 0) break;
    for (int j = i + 1; j < 8; j++)
      if (!(g & (1 << j))) { res = "KP" + String(i + 1) + "+KP" + String(j + 1); break; }
  }
  wr8(A_MCP_KEYS, 0x00, 0xFF);
  return res;
}

// ------------------------------------------------------------ ADS1115
// returns volts at the chip pin, or NAN on error. Range +/-4.096 V.
float adsRead(uint8_t a, int ch) {
  uint16_t cfg = 0x8000 | ((4 + ch) << 12) | (1 << 9) | (1 << 8) | (4 << 5) | 0x0003;
  Wire.beginTransmission(a); Wire.write(0x01); Wire.write(cfg >> 8); Wire.write(cfg & 0xFF);
  if (Wire.endTransmission() != 0) return NAN;
  uint32_t t0 = millis();
  uint8_t b[2];
  do { delay(2); if (!rdN(a, 0x01, b, 2)) return NAN; } while (!(b[0] & 0x80) && millis() - t0 < 50);
  if (!rdN(a, 0x00, b, 2)) return NAN;
  int16_t raw = (int16_t)((b[0] << 8) | b[1]);
  return raw * 4.096f / 32768.0f;
}
const float DIV_SENS = 42.0f / 27.0f;   // 15k / 27k divider on pressure, leak, sensor-5V inputs

float ntcTempC(float v) {
  if (isnan(v) || v > 3.25f) return NAN;        // open circuit
  if (v < 0.02f) return NAN;                    // short
  float r = 10000.0f * v / (3.3f - v);
  float t = 1.0f / (1.0f / 298.15f + log(r / 10000.0f) / 3950.0f);
  return t - 273.15f;
}

// ------------------------------------------------------------ NAU7802
bool nauReady = false;
bool nauInit() {
  if (!i2cPresent(A_NAU)) return false;
  wr8(A_NAU, 0x00, 0x01); delay(1);             // register reset
  wr8(A_NAU, 0x00, 0x02);                       // power up digital
  uint32_t t0 = millis();
  while (!(rd8(A_NAU, 0x00) & 0x08)) { if (millis() - t0 > 200) return false; delay(1); }
  wr8(A_NAU, 0x01, (0b101 << 3) | 0b111);       // LDO 3.0 V, gain 128
  wr8(A_NAU, 0x00, 0x80 | 0x04 | 0x02);         // internal LDO, power up analog + digital
  int adc = rd8(A_NAU, 0x15); wr8(A_NAU, 0x15, (adc & ~0x30) | 0x30);  // chopper clock off (datasheet rec.)
  wr8(A_NAU, 0x02, 0x00);                       // 10 samples/s
  delay(600);
  wr8(A_NAU, 0x02, 0x04);                       // internal offset calibration
  t0 = millis();
  while (rd8(A_NAU, 0x02) & 0x04) { if (millis() - t0 > 1000) break; delay(5); }
  wr8(A_NAU, 0x00, rd8(A_NAU, 0x00) | 0x10);    // start conversions
  nauReady = true;
  return true;
}
bool nauRead(int32_t &val) {
  uint32_t t0 = millis();
  while (!(rd8(A_NAU, 0x00) & 0x20)) { if (millis() - t0 > 300) return false; delay(5); }
  uint8_t b[3];
  if (!rdN(A_NAU, 0x12, b, 3)) return false;
  val = ((int32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8))) >> 8;
  return true;
}

// ------------------------------------------------------------ FRAM (MB85RC256V, 32 KB)
bool framWrite(uint16_t addr, const uint8_t *d, int n) {
  Wire.beginTransmission(A_FRAM); Wire.write(addr >> 8); Wire.write(addr & 0xFF);
  for (int i = 0; i < n; i++) Wire.write(d[i]);
  return Wire.endTransmission() == 0;
}
bool framRead(uint16_t addr, uint8_t *d, int n) {
  Wire.beginTransmission(A_FRAM); Wire.write(addr >> 8); Wire.write(addr & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)A_FRAM, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}
const uint16_t F_BOOTCNT = 0x7FF0;   // 4 bytes
const uint16_t F_WDTFLAG = 0x7FF4;   // 1 byte
const uint16_t F_SCRATCH = 0x7F00;   // 16 bytes used for the read/write test

// ------------------------------------------------------------ RTC (DS3231M)
uint8_t bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
uint8_t tobcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
bool rtcRead(char *out) {
  uint8_t b[7];
  if (!rdN(A_RTC, 0x00, b, 7)) return false;
  sprintf(out, "20%02d-%02d-%02d %02d:%02d:%02d", bcd2(b[6]), bcd2(b[5] & 0x1F), bcd2(b[4]),
          bcd2(b[2] & 0x3F), bcd2(b[1]), bcd2(b[0] & 0x7F));
  return true;
}
bool rtcSet(int y, int mo, int d, int h, int mi, int s) {
  Wire.beginTransmission(A_RTC); Wire.write(0x00);
  Wire.write(tobcd(s)); Wire.write(tobcd(mi)); Wire.write(tobcd(h)); Wire.write(1);
  Wire.write(tobcd(d)); Wire.write(tobcd(mo)); Wire.write(tobcd(y % 100));
  bool ok = Wire.endTransmission() == 0;
  int st = rd8(A_RTC, 0x0F); if (st >= 0) wr8(A_RTC, 0x0F, st & ~0x80);   // clear "lost power" flag
  return ok;
}
void rtcSetFromBuild() {
  const char *m = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4]; int d, y, h, mi, s;
  sscanf(__DATE__, "%3s %d %d", mon, &d, &y); sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  rtcSet(y, (strstr(m, mon) - m) / 3 + 1, d, h, mi, s);
}

// ------------------------------------------------------------ result tracking
int nPass = 0, nFail = 0;
String firstFail = "";
void result(const char *name, bool ok, const String &detail) {
  Serial.printf("  [%s] %-18s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (ok) nPass++; else { nFail++; if (firstFail == "") firstFail = name; }
}
void info(const char *name, const String &detail) {
  Serial.printf("  [info] %-18s %s\n", name, detail.c_str());
}

// ------------------------------------------------------------ automatic tests
void testI2C() {
  struct { uint8_t a; const char *n; } dev[] = {
    {A_MCP_KEYS, "U3 MCP23017 0x20"}, {A_MCP_EXP, "U19 MCP23017 0x21"}, {A_NAU, "U15 NAU7802 0x2A"},
    {A_ADS1, "U9 ADS1115 0x48"}, {A_ADS2, "U13 ADS1115 0x49"}, {A_ADS3, "U17 ADS1115 0x4B"},
    {A_FRAM, "U10 FRAM 0x50"}, {A_RTC, "U12 RTC 0x68"}};
  for (auto &d : dev) result(d.n, i2cPresent(d.a), i2cPresent(d.a) ? "found" : "NOT FOUND - check soldering / address pins");
  String extra = "";
  for (uint8_t a = 8; a < 0x78; a++) {
    bool known = false; for (auto &d : dev) if (d.a == a) known = true;
    if (!known && a != 0x7C && i2cPresent(a)) extra += String(" 0x") + String(a, HEX);
  }
  if (extra != "") info("Other I2C devices", extra);
}

void testModule() {
  info("Chip", String(ESP.getChipModel()) + " rev " + ESP.getChipRevision());
  info("Flash", String(ESP.getFlashChipSize() / 1048576) + " MB");
  info("PSRAM", String(ESP.getPsramSize() / 1048576) + " MB");
}

void testFram() {
  if (!i2cPresent(A_FRAM)) { result("FRAM read/write", false, "chip not found"); return; }
  uint8_t keep[16], pat[16], back[16];
  framRead(F_SCRATCH, keep, 16);
  for (int i = 0; i < 16; i++) pat[i] = (uint8_t)(0xA5 ^ (i * 37) ^ (millis() & 0xFF));
  framWrite(F_SCRATCH, pat, 16); framRead(F_SCRATCH, back, 16);
  bool ok = memcmp(pat, back, 16) == 0;
  framWrite(F_SCRATCH, keep, 16);
  result("FRAM read/write", ok, ok ? "16 bytes written and read back" : "data came back wrong");
  uint32_t c = 0; framRead(F_BOOTCNT, (uint8_t *)&c, 4);
  info("FRAM boot count", String(c) + "  (should go up by 1 every time you power up)");
}

void testRtc() {
  if (!i2cPresent(A_RTC)) { result("RTC", false, "chip not found"); return; }
  int st = rd8(A_RTC, 0x0F);
  char t1[24], t2[24];
  if (st & 0x80) {
    info("RTC", "clock had lost power (no battery yet?) - setting it to the build time");
    rtcSetFromBuild();
  }
  rtcRead(t1); delay(1100); rtcRead(t2);
  bool ticking = strcmp(t1, t2) != 0;
  result("RTC ticking", ticking, String(t2));
  uint8_t b[2]; rdN(A_RTC, 0x11, b, 2);
  float tc = (int8_t)b[0] + (b[1] >> 6) * 0.25f;
  result("RTC temperature", tc > 0 && tc < 50, String(tc, 2) + " C (should be about room temp)");
}

void testAnalog(bool verdicts) {
  float v5 = adsRead(A_ADS2, 3) * 2.0f;
  float v12 = adsRead(A_ADS2, 2) * 5.7f;
  float vs5 = adsRead(A_ADS3, 3) * DIV_SENS;
  float bias = adsRead(A_ADS2, 0);
  if (verdicts) {
    result("5V rail", v5 > 4.5f && v5 < 5.5f, String(v5, 2) + " V");
    if (v12 < 1.0f) info("12V rail", String(v12, 2) + " V (none - normal when on USB only)");
    else result("12V rail", v12 > 11.0f && v12 < 13.8f, String(v12, 2) + " V");
    result("Sensor 5V (F6)", vs5 > 4.3f && vs5 < 5.5f, String(vs5, 2) + " V");
    info("CT bias", "check with a meter on C25: should be about 1.65 V (the CT inputs only see it with a clamp plugged in)");
  }
  float ntcV = adsRead(A_ADS1, 0);
  float t = ntcTempC(ntcV);
  info("Tank temp (J12)", isnan(t) ? String("not connected (") + String(ntcV, 2) + " V)" : String(t, 1) + " C");
  float pv[5] = {adsRead(A_ADS1, 1), adsRead(A_ADS1, 2), adsRead(A_ADS1, 3), adsRead(A_ADS3, 1), adsRead(A_ADS3, 2)};
  const char *pn[5] = {"P1 HP  (J37)", "P2 LP  (J38)", "P3 MAN1(J39)", "P4 MAN2(J40)", "P5 SPR (J41)"};
  for (int i = 0; i < 5; i++)
    info(pn[i], String(pv[i], 3) + " V at ADC  = " + String(pv[i] * DIV_SENS, 2) + " V from sensor (0.5-4.5 V type)");
  info("Leak (J34)", String(adsRead(A_ADS3, 0) * DIV_SENS, 2) + " V from sensor");
  info("CT comp (J13)", String(adsRead(A_ADS2, 1), 3) + " V (about 1.65 V with a clamp plugged in, random without)");
  info("CT pump (J15)", String(bias, 3) + " V (about 1.65 V with a clamp plugged in, random without)");
}

void testNau() {
  if (!nauReady && !nauInit()) { result("Load cell ADC", false, "NAU7802 did not start"); return; }
  int rev = rd8(A_NAU, 0x1F);
  int32_t v;
  bool ok = nauRead(v);
  result("Load cell ADC", ok && (rev & 0x0F) == 0x0F, ok ? String("reading ") + v + " (rev 0x" + String(rev, HEX) + ")" : "no data");
}

void testInputs() {
  int b = rd8(A_MCP_KEYS, 0x13);
  if (b < 0) { result("Inputs", false, "U3 not answering"); return; }
  info("ESTOP_OK (J30-19)", (b & 0x40) ? "HIGH" : "LOW");
  info("HP_OK   (J30-20)", (b & 0x80) ? "HIGH" : "LOW");
  info("Printer BUSY", (b & 0x10) ? "HIGH" : "LOW");
  info("BOOT button", digitalRead(PIN_BOOT) ? "not pressed" : "PRESSED");
}

void testWifi() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  int n = WiFi.scanNetworks();
  String s = String(n) + " networks";
  for (int i = 0; i < n && i < 3; i++) s += "  " + WiFi.SSID(i) + "(" + WiFi.RSSI(i) + ")";
  result("Wi-Fi scan", n > 0, s);
  WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
}

void runAuto() {
  nPass = nFail = 0; firstFail = "";
  lcdLine(0, "Testing..."); lcdLine(1, "");
  rgb(0, 0, 40);
  Serial.println("\n===== AUTOMATIC TESTS =====");
  testModule();
  testI2C();
  testFram();
  testRtc();
  testAnalog(true);
  testNau();
  testInputs();
  testWifi();
  Serial.printf("===== %d passed, %d failed =====\n", nPass, nFail);
  char l[24];
  snprintf(l, sizeof l, "Pass %d Fail %d", nPass, nFail);
  lcdLine(0, l);
  lcdLine(1, nFail ? firstFail.c_str() : "ALL OK");
  rgb(nFail ? 60 : 0, nFail ? 0 : 60, 0);
}

// ------------------------------------------------------------ interactive tests
bool keyHit() { if (Serial.available()) { while (Serial.available()) Serial.read(); return true; } return false; }

void liveAnalog() {
  Serial.println("Live readings every second. Press Enter to stop.");
  while (!keyHit()) { Serial.println("-----"); testAnalog(false); testInputs(); delay(1000); }
}

void keypadTest() {
  Serial.println("Press keys on the keypad. Each press shows which two KP lines it joins.");
  Serial.println("Write these down - the real firmware needs this map. Press Enter to stop.");
  lcdLine(0, "Keypad test"); lcdLine(1, "press a key");
  String last = "";
  while (!keyHit()) {
    String k = keyScan();
    if (k != last && k != "") {
      Serial.println("  key: " + k); lcdLine(1, k.c_str());
      digitalWrite(PIN_BUZZER, HIGH); delay(30); digitalWrite(PIN_BUZZER, LOW);
    }
    last = k; delay(30);
  }
}

void relayWalk() {
  Serial.println("Each output turns on for 1.5 s in turn.");
  Serial.println("Put an LED + 1k resistor (or meter) between the J30 pin and GND (pin 3) to see it.");
  for (int i = 0; i < NOUTS; i++) {
    char l[24];
    Serial.printf("  ON : %-20s J30 pin %d\n", OUTS[i].name, OUTS[i].j30pin);
    snprintf(l, sizeof l, "J30 pin %d ON", OUTS[i].j30pin);
    lcdLine(0, OUTS[i].name); lcdLine(1, l);
    setOut(i, true); delay(1500); setOut(i, false); delay(200);
    if (keyHit()) break;
  }
  allOff();
  Serial.println("All outputs off.");
}

void outputsTest() {
  Serial.println("Backlight off/on, buzzer beep, RGB LED red/green/blue.");
  digitalWrite(PIN_LCD_BL, LOW); delay(700); digitalWrite(PIN_LCD_BL, HIGH);
  digitalWrite(PIN_BUZZER, HIGH); delay(300); digitalWrite(PIN_BUZZER, LOW);
  rgb(80, 0, 0); delay(600); rgb(0, 80, 0); delay(600); rgb(0, 0, 80); delay(600); rgb(0, 0, 0);
  Serial.println("Did you see/hear all of them? (backlight blink, beep, red, green, blue)");
}

void watchdogTest() {
  Serial.println("Measure J30 pin 12 (SAFE_EN) to GND, or put an LED + 1k on it.");
  Serial.println("Step 1: SAFE_EN ON for 5 s  -> should read about 3.3 V");
  digitalWrite(PIN_SAFE_GPIO, HIGH); delay(5000);
  Serial.println("Step 2: SAFE_EN OFF for 3 s -> should read 0 V");
  digitalWrite(PIN_SAFE_GPIO, LOW); delay(3000);
  Serial.println("Step 3: SAFE_EN ON, then the firmware stops feeding the watchdog.");
  Serial.println("        SAFE_EN must drop out. If J10 is fitted the board will also reset.");
  uint8_t f = 1; framWrite(F_WDTFLAG, &f, 1);
  digitalWrite(PIN_SAFE_GPIO, HIGH); delay(1000);
  wdtFeed = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 6000) { Serial.print('.'); delay(500); }
  wdtFeed = true;
  digitalWrite(PIN_SAFE_GPIO, LOW);
  f = 0; framWrite(F_WDTFLAG, &f, 1);
  Serial.println("\nStill running, so J10 is OFF (or the watchdog is not resetting the ESP32).");
  Serial.println("With J10 off: did SAFE_EN flick off during the dots? If yes -> PASS.");
  Serial.println("Fit J10 and run this again to check the reset as well.");
}

void printerTest() {
  Serial.println("Printer loopback:");
  Serial.println("  RS-232: J5 jumper on pins 1-2, bridge J7 pin 3 to J7 pin 5");
  Serial.println("  TTL   : J5 jumper on pins 2-3, bridge J4 pin 2 to J4 pin 3");
  Serial1.begin(9600, SERIAL_8N1, PIN_PRN_RX, PIN_PRN_TX);
  delay(50); while (Serial1.available()) Serial1.read();
  const char *msg = "LOOPBACK-123";
  Serial1.print(msg); Serial1.flush(); delay(100);
  String got = ""; while (Serial1.available()) got += (char)Serial1.read();
  Serial1.end();
  nPass = nFail = 0;
  result("Printer loopback", got == msg, String("sent ") + msg + "  got '" + got + "'");
}

void loadCellLive() {
  if (!nauReady && !nauInit()) { Serial.println("NAU7802 not responding."); return; }
  Serial.println("Load cell raw reading. Press on the cell - the number should change smoothly.");
  Serial.println("Press Enter to stop.");
  while (!keyHit()) { int32_t v; if (nauRead(v)) Serial.printf("  %ld\n", (long)v); delay(200); }
}

void setTime() {
  Serial.println("Type the time as  YYYY-MM-DD HH:MM:SS  and press Enter:");
  while (!Serial.available()) delay(10);
  String s = Serial.readStringUntil('\n');
  int y, mo, d, h, mi, se;
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6) {
    rtcSet(y, mo, d, h, mi, se); char t[24]; rtcRead(t); Serial.println(String("RTC set: ") + t);
  } else Serial.println("Didn't understand that.");
}

void menu() {
  Serial.println("\n----- MENU (type a number, press Enter) -----");
  Serial.println(" 1  Run automatic tests again");
  Serial.println(" 2  Live sensor readings");
  Serial.println(" 3  Keypad test");
  Serial.println(" 4  Relay outputs walk (J30)");
  Serial.println(" 5  Backlight / buzzer / RGB LED");
  Serial.println(" 6  Watchdog + SAFE_EN test");
  Serial.println(" 7  Printer port loopback");
  Serial.println(" 8  Load cell live reading");
  Serial.println(" 9  Set the clock");
}

// ------------------------------------------------------------ setup / loop
void setup() {
  pinMode(PIN_WDT, OUTPUT);
  xTaskCreate(wdtTask, "wdt", 2048, nullptr, 5, nullptr);   // feed the watchdog straight away

  pinMode(PIN_SAFE_GPIO, OUTPUT); digitalWrite(PIN_SAFE_GPIO, LOW);
  for (int i = 0; i < NOUTS; i++) if (OUTS[i].kind == 0) { pinMode(OUTS[i].pin, OUTPUT); digitalWrite(OUTS[i].pin, LOW); }
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LCD_BL, OUTPUT); digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY_INT, INPUT_PULLUP);
  rgb(0, 0, 0);

  Serial.begin(115200);
  uint32_t t0 = millis(); while (!Serial && millis() - t0 < 4000) delay(10);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  lcdInit();
  lcdLine(0, "AC Controller"); lcdLine(1, "bench test");
  mcpSetup();

  Serial.println("\n\nAC Controller bench test");
  if (i2cPresent(A_FRAM)) {
    uint32_t c = 0; framRead(F_BOOTCNT, (uint8_t *)&c, 4);
    if (c == 0xFFFFFFFF) c = 0;
    c++; framWrite(F_BOOTCNT, (uint8_t *)&c, 4);
    uint8_t f = 0; framRead(F_WDTFLAG, &f, 1);
    if (f == 1) {
      Serial.println(">>> Watchdog test: the board was reset by the watchdog. PASS <<<");
      f = 0; framWrite(F_WDTFLAG, &f, 1);
    }
  }
  digitalWrite(PIN_BUZZER, HIGH); delay(80); digitalWrite(PIN_BUZZER, LOW);
  delay(800);
  runAuto();
  menu();
}

void loop() {
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim();
    int c = s.toInt();
    switch (c) {
      case 1: runAuto(); break;
      case 2: liveAnalog(); break;
      case 3: keypadTest(); break;
      case 4: relayWalk(); break;
      case 5: outputsTest(); break;
      case 6: watchdogTest(); break;
      case 7: printerTest(); break;
      case 8: loadCellLive(); break;
      case 9: setTime(); break;
      default: break;
    }
    if (s.length()) menu();
  }
  delay(10);
}
