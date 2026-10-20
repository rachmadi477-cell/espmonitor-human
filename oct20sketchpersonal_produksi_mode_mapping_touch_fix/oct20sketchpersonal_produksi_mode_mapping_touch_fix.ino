#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

// =====================================================
// WIFI & SERVER
// =====================================================

const char* ssid = "SEED-5";
const char* password = "transformer7";

const char* SERVER_IP = "192.168.11.177";

const char* API_KARTU = "/personal_produksi/api_kartu.php";
const char* API_TRANSAKSI = "/personal_produksi/api_transaksi.php";
const char* REG_PATH = "/personal_produksi/index.php";

#define NAMA_MESIN "LAS P STAINLESS"

// =====================================================
// PIN
// =====================================================

#define SS_PIN         2
#define RST_PIN        22

#define SCK_PIN        18
#define MISO_PIN       19
#define MOSI_PIN       23

#define LCD_SDA        21
#define LCD_SCL        25

#define BUZZER_PIN     26
#define TOUCH_MODE_PIN 4

// =====================================================
// OBJECT
// =====================================================

MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================================================
// STATUS SISTEM
// =====================================================

bool modeRegistrasi = false;
bool menungguValidasi = false;
bool menungguBreak = false;
bool menungguStartMode5 = false;
String uidMasterBreakPending = "";

String uidPekerjaPending = "";
String namaPekerjaPending = "";
String aksiPending = "";   // START atau FINISH

unsigned long lastRegCheck = 0;

// =====================================================
// IDENTITAS ESP + MODE JEDA
// =====================================================
#define IDENTITAS_BARIS1 "P STAINLESS II"
#define IDENTITAS_BARIS2 "LAS STIK II"

int modeJeda = 1;
bool touchLock = false;
unsigned long lastTouch = 0;
unsigned long modeDipilihPada = 0;
const unsigned long MODE_NONDEFAULT_TIMEOUT = 2500; // 2,5 detik tanpa tap kartu
unsigned long lastLcdScroll = 0;
int halamanStandby = 0;

// Deklarasi fungsi lebih awal
void beepBeb();

// =====================================================
// DEFAULT MODE JEDA: MODE 5 = TOILET
// =====================================================
void resetModeDefault() {
  modeJeda = 5;
  modeDipilihPada = 0;
  touchLock = false;
  Serial.println(">>> MODE DEFAULT RESET = 5 (TOILET)");
}

const char* namaModeJeda(int mode) {
  if (mode == 1) return "START";
  if (mode == 2) return "FINISH";
  if (mode == 3) return "URUSAN ADMIN";
  if (mode == 4) return "DIPANGGIL ATASAN";
  return "TOILET";
}

int batasModeJeda(int mode) {
  if (mode == 3) return 15;
  if (mode == 4) return 20;
  return 0;
}

void tampilIdentitas() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(IDENTITAS_BARIS1);
  lcd.setCursor(0, 1);
  lcd.print(IDENTITAS_BARIS2);
}

void tampilModeJeda() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MODE JEDA: ");
  lcd.print(modeJeda);
  lcd.setCursor(0, 1);
  String teks = String(namaModeJeda(modeJeda)) + " " + String(batasModeJeda(modeJeda)) + "M";
  lcd.print(teks.substring(0, 16));
}

void beepMode() {
  beepBeb();
  delay(70);
  beepBeb();
}

void cekTouchMode() {
  if (modeRegistrasi || menungguValidasi || menungguBreak) return;

  // Debounce sensor sentuh: sinyal kudu stabil HIGH, ora cukup spike cekak.
  static unsigned long highMulai = 0;
  static bool highStabilDiproses = false;
  bool touched = (digitalRead(TOUCH_MODE_PIN) == HIGH);

  if (touched) {
    if (highMulai == 0) highMulai = millis();
    if (!highStabilDiproses && (millis() - highMulai >= 80) &&
        !touchLock && (millis() - lastTouch > 700)) {
      highStabilDiproses = true;
      touchLock = true;
      lastTouch = millis();

      modeJeda++;
      if (modeJeda > 5) modeJeda = 1;
      modeDipilihPada = (modeJeda == 5) ? 0 : millis();

      Serial.print("MODE JEDA DIPILIH: ");
      Serial.print(modeJeda);
      Serial.print(" - ");
      Serial.println(namaModeJeda(modeJeda));

      tampilModeJeda();
      beepMode();
      lastLcdScroll = millis();
    }
  } else {
    highMulai = 0;
    highStabilDiproses = false;
    touchLock = false;
  }
}

void cekTimeoutModeDefault() {
  // Mode pilihan bali menyang default TOILET (5) yen ora ana transaksi.
  // Mode 1 START diwenehi wektu luwih dawa kanggo urutan KARU -> pekerja.
  unsigned long timeoutMode = (modeJeda == 1) ? 15000UL : MODE_NONDEFAULT_TIMEOUT;
  if (modeJeda != 5 && modeDipilihPada > 0 &&
      !menungguBreak && !menungguValidasi &&
      !modeRegistrasi && (millis() - modeDipilihPada >= timeoutMode)) {
    Serial.println(">>> TIMEOUT TANPA TAP KARTU: BALIK MODE 5 (TOILET)");
    resetModeDefault();
    tampilStandby();
  }
}

void scrollStandby() {
  if (modeRegistrasi || menungguValidasi || menungguBreak) return;
  if (millis() - lastLcdScroll < 3000) return;

  lastLcdScroll = millis();

  halamanStandby++;
  if (halamanStandby > 1) halamanStandby = 0;

  lcd.clear();

  if (halamanStandby == 0) {
    lcd.setCursor(0, 0);
    lcd.print("    SANIHARTO");
    lcd.setCursor(0, 1);
    lcd.print("TAP PIN PRODUKSI");
  } else {
    lcd.setCursor(0, 0);
    lcd.print(IDENTITAS_BARIS1);
    lcd.setCursor(0, 1);
    lcd.print(IDENTITAS_BARIS2);
  }
}

// =====================================================
// MASTER UID & BUZZER
// =====================================================

const String UID_KARU  = "99 43 D6 06";
const String UID_KK    = "75 6D D7 06";
const String UID_ADMIN = "94 1D DB 06";

void beepBeb() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(120);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepOK() {
  beepBeb();
  delay(100);
  beepBeb();
  delay(100);
  beepBeb();
}

void beepSalah() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1200);
  digitalWrite(BUZZER_PIN, LOW);
}

// =====================================================
// LCD TAMPILAN
// =====================================================

void tampilStandby() {
  halamanStandby = 0;
  lastLcdScroll = millis();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("    SANIHARTO");
  lcd.setCursor(0, 1);
  lcd.print("TAP PIN PRODUKSI");
}

void tampilReg() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MODE DAFTAR UID");
  lcd.setCursor(0, 1);
  lcd.print("TAP KARTU BARU");
}

void tampilValidasi() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("TAP VALIDASI");
  lcd.setCursor(0, 1);
  lcd.print("CARD");
}

// =====================================================
// UTILITY & WIFI
// =====================================================

String enc(String s) {
  s.replace(" ", "%20");
  return s;
}

String bacaUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) {
      uid += " ";
    }
  }
  uid.toUpperCase();
  return uid;
}

void koneksiWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("WIFI ");
  int c = 0;
  while (WiFi.status() != WL_CONNECTED && c < 20) {
    delay(500);
    Serial.print(".");
    c++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP ESP32 : ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WIFI OFFLINE");
  }
}

// =====================================================
// CEK REGISTRASI & KARTU
// =====================================================

void cekModeRegistrasi() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastRegCheck < 1000) return;
  lastRegCheck = millis();

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(REG_PATH) + "?api=register_status&_=" + String(millis());

  http.setConnectTimeout(1500);
  http.setTimeout(2000);

  if (http.begin(url)) {
    int code = http.GET();
    if (code == 200) {
      String s = http.getString();
      bool aktif = s.indexOf("\"aktif\":1") >= 0;

      if (aktif && !modeRegistrasi) {
        modeRegistrasi = true;
        if (!menungguValidasi) tampilReg();
      }
      if (!aktif && modeRegistrasi) {
        modeRegistrasi = false;
        if (!menungguValidasi) tampilStandby();
      }
    }
    http.end();
  }
}

void kirimRegistrasi(String uid) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(REG_PATH) + "?api=register_scan&uid=" + enc(uid);
  http.setConnectTimeout(2000);
  http.setTimeout(3000);

  if (http.begin(url)) {
    int code = http.GET();
    if (code == 200) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("UID DITERIMA");
      lcd.setCursor(0, 1);
      lcd.print(uid.substring(0, 16));
      beepOK();
      delay(2500);
      tampilReg();
    }
    http.end();
  }
}

bool cekKartu(String uid, String &tipe, String &nama) {
  tipe = "";
  nama = "";

  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_KARTU) + "?uid=" + enc(uid);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);

  if (!http.begin(url)) return false;

  int code = http.GET();
  String response = http.getString();
  http.end();

  if (code != 200) return false;

  String data = response;
  data.replace(" ", "");
  data.replace("\r", "");
  data.replace("\n", "");
  data.replace("\t", "");

  if (data.indexOf("\"status\":\"OK\"") < 0) return false;

  if (data.indexOf("\"tipe\":\"MASTER\"") >= 0) {
    tipe = "MASTER";
  } else if (data.indexOf("\"tipe\":\"PEKERJA\"") >= 0) {
    tipe = "PEKERJA";
  } else {
    return false;
  }

  int posisiNama = data.indexOf("\"nama\":\"");
  if (posisiNama >= 0) {
    posisiNama += 8;
    int akhirNama = data.indexOf("\"", posisiNama);
    if (akhirNama > posisiNama) {
      nama = data.substring(posisiNama, akhirNama);
    }
  }

  return true;
}

// =====================================================
// CEK STATUS PEKERJA
// =====================================================
String cekStatusPekerja(String uidPekerja) {
  if (WiFi.status() != WL_CONNECTED) return "ERROR";

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) +
               "?aksi=STATUS&uid1=" + enc(uidPekerja);
  http.setConnectTimeout(1500);
  http.setTimeout(3000);

  if (!http.begin(url)) return "ERROR";

  int code = http.GET();
  String response = http.getString();
  http.end();

  String data = response;
  data.replace(" ", "");

  if (code != 200) return "ERROR";
  if (data.indexOf("\"status\":\"WORK\"") >= 0) return "WORK";
  if (data.indexOf("\"status\":\"READY\"") >= 0) return "READY";
  if (data.indexOf("\"status\":\"BREAK\"") >= 0) return "BREAK";
  if (data.indexOf("\"status\":\"FINISH\"") >= 0) return "FINISH";
  return "NONE";
}

// =====================================================
// TRANSAKSI START & BREAK & RETURN
// =====================================================

void kirimSTART(String uidPekerja, String uidMaster) {
  if (WiFi.status() != WL_CONNECTED) {
    tampilStandby();
    return;
  }

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) + "?aksi=START&uid1=" + enc(uidPekerja) + "&uid2=" + enc(uidMaster);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);

  if (!http.begin(url)) {
    beepSalah();
    delay(600);
    menungguValidasi = false;
    uidPekerjaPending = "";
    namaPekerjaPending = "";
    aksiPending = "";
    tampilStandby();
    return;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  String data = response;
  data.replace(" ", "");

  if (code == 200 && data.indexOf("\"status\":\"OK\"") >= 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ACC DITERIMA");
    lcd.setCursor(0, 1);
    lcd.print(namaPekerjaPending.length() > 0 ? namaPekerjaPending.substring(0, 16) : "START WORK");
    beepOK();
    delay(3000);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("START DITOLAK");
    beepSalah();
    delay(600);
  }

  menungguValidasi = false;
  uidPekerjaPending = "";
  namaPekerjaPending = "";
  aksiPending = "";
  tampilStandby();
}

void kirimBREAK(String uidMaster, String uidPekerja) {
  if (WiFi.status() != WL_CONNECTED) {
    tampilStandby();
    return;
  }

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) +
               "?aksi=BREAK&uid1=" + enc(uidMaster) +
               "&uid2=" + enc(uidPekerja) +
               "&kode_jeda=" + String(modeJeda);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);

  if (!http.begin(url)) {
    beepSalah();
    delay(600);
    menungguBreak = false;
    menungguStartMode5 = false;
    uidMasterBreakPending = "";
    tampilStandby();
    return;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  // =====================================================
  // DEBUG BREAK - V42
  // =====================================================
  Serial.println();
  Serial.println("===== BREAK REQUEST V42 =====");
  Serial.print("MASTER : ");
  Serial.println(uidMaster);
  Serial.print("PEKERJA: ");
  Serial.println(uidPekerja);
  Serial.print("MODE   : ");
  Serial.print(modeJeda);
  Serial.print(" - ");
  Serial.println(namaModeJeda(modeJeda));
  Serial.print("HTTP   : ");
  Serial.println(code);
  Serial.print("RESP   : ");
  Serial.println(response);
  Serial.println("============================");
  Serial.println();

  String data = response;
  data.replace(" ", "");

  if (code == 200 && data.indexOf("\"status\":\"OK\"") >= 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BREAK BERHASIL");
    lcd.setCursor(0, 1);
    String infoJeda = String(namaModeJeda(modeJeda)) + " " + String(batasModeJeda(modeJeda)) + "M";
    lcd.print(infoJeda.substring(0, 16));
    beepOK();
    delay(2000);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BREAK DITOLAK");
    beepSalah();
    delay(600);
  }

  // Protap: sawise UID pekerja ditap kanggo ngrampungake BREAK mode 1/2/3,
  // mode bali menyang default TOILET, kalebu yen API nolak transaksi.
  // Reset ditindakake sawise respons lan tampilan wis nggunakake mode sing dipilih.
  resetModeDefault();

  menungguBreak = false;
  uidMasterBreakPending = "";
  tampilStandby();
}

void kirimFINISH(String uidPekerja, String uidMaster) {
  if (WiFi.status() != WL_CONNECTED) {
    tampilStandby();
    return;
  }

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) +
               "?aksi=FINISH&uid1=" + enc(uidPekerja) +
               "&uid2=" + enc(uidMaster);
  http.setConnectTimeout(1500);
  http.setTimeout(3000);

  if (!http.begin(url)) {
    beepSalah();
    delay(600);
    menungguValidasi = false;
    uidPekerjaPending = "";
    namaPekerjaPending = "";
    aksiPending = "";
    tampilStandby();
    return;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  String data = response;
  data.replace(" ", "");

  if (code == 200 && data.indexOf("\"status\":\"OK\"") >= 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("FINISH BERHASIL");
    lcd.setCursor(0, 1);
    lcd.print("PEKERJA SELESAI");
    beepOK();
    delay(2000);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("FINISH DITOLAK");
    lcd.setCursor(0, 1);
    lcd.print("CEK STATUS");
    beepSalah();
    delay(700);
  }

  menungguValidasi = false;
  uidPekerjaPending = "";
  namaPekerjaPending = "";
  aksiPending = "";
  resetModeDefault();
  tampilStandby();
}

bool kirimRETURN(String uidPekerja) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) + "?aksi=RETURN&uid1=" + enc(uidPekerja);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);

  if (!http.begin(url)) return false;

  int code = http.GET();
  String response = http.getString();
  http.end();

  String data = response;
  data.replace(" ", "");

  if (code == 200 && data.indexOf("\"status\":\"OK\"") >= 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("RETURN BERHASIL");
    lcd.setCursor(0, 1);
    lcd.print("KEMBALI WORK");
    beepOK();
    delay(2000);

    resetModeDefault();
    tampilStandby();
    return true;
  }

  return false;
}


// Tap lama = kartu UID yang sama tetep ana ing reader nganti 5 detik.
// Wacan gagal sakpisan ora langsung dianggep tap cekak; diwenehi toleransi
// sawetara kegagalan polling supaya timing RC522 ora gampang salah deteksi.
bool tahanKartuLimaDetik(String uidExpected) {
  const unsigned long DURASI_TAP_LAMA = 5000;
  const byte MAKS_GAGAL_BERTURUT = 4;
  unsigned long mulai = millis();
  byte gagalBerturut = 0;

  Serial.println("[TAP] Mulai deteksi tahan 5 detik");

  // Kartu saka proses awal isih ing status ACTIVE; HALT dhisik supaya
  // polling WUPA sabanjure bisa nemokake UID sing padha maneh.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  while (millis() - mulai < DURASI_TAP_LAMA) {
    byte atqa[2];
    byte atqaSize = sizeof(atqa);
    MFRC522::StatusCode status = rfid.PICC_WakeupA(atqa, &atqaSize);

    if (status != MFRC522::STATUS_OK || !rfid.PICC_ReadCardSerial()) {
      gagalBerturut++;
      if (gagalBerturut >= MAKS_GAGAL_BERTURUT) {
        Serial.println("[TAP] Kartu ora ketemu maneh: TAP CEKAK");
        rfid.PCD_StopCrypto1();
        return false;
      }
      delay(70);
      continue;
    }

    String uidSekarang = bacaUID();
    if (uidSekarang != uidExpected) {
      Serial.println("[TAP] UID berubah: TAP CEKAK");
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      return false;
    }

    gagalBerturut = 0;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    delay(70);
  }

  Serial.println("[TAP] UID padha ketahan 5 detik: TAP LAMA");
  return true;
}

// BREAK TOILET worker-only path, used only inside the official PKB toilet windows.
void kirimBreakToiletPekerja(String uidPekerja) {
  if (WiFi.status() != WL_CONNECTED) { tampilStandby(); return; }
  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) +
               "?aksi=BREAK&uid1=" + enc(uidPekerja) + "&kode_jeda=1";
  http.setConnectTimeout(3000); http.setTimeout(5000);
  if (!http.begin(url)) { beepSalah(); tampilStandby(); return; }
  int code=http.GET(); String response=http.getString(); http.end();
  String data=response; data.replace(" ", "");
  Serial.println("===== BREAK TOILET WORKER ONLY ====="); Serial.println(response);
  if (code==200 && data.indexOf("\"status\":\"OK\"")>=0) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("BREAK TOILET");
    lcd.setCursor(0,1); lcd.print("JEDA TERCATAT"); beepOK(); delay(1500);
  } else {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("BREAK DITOLAK");
    lcd.setCursor(0,1); lcd.print("TAP MASTER DULU"); beepSalah(); delay(1000);
  }
  tampilStandby();
}

bool breakToiletWorkerOnly(String uidPekerja) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BREAK_POLICY] WiFi offline");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + String(API_TRANSAKSI) +
               "?aksi=BREAK_POLICY&uid1=" + enc(uidPekerja) + "&kode_jeda=1";
  http.setConnectTimeout(2500);
  http.setTimeout(3500);

  if (!http.begin(url)) {
    Serial.println("[BREAK_POLICY] HTTP begin gagal");
    return false;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  String data = response;
  data.replace(" ", "");
  data.replace("\\r", "");
  data.replace("\\n", "");
  Serial.print("[BREAK_POLICY] HTTP=");
  Serial.println(code);
  Serial.print("[BREAK_POLICY] RESP=");
  Serial.println(response);

  // API ngirim worker_only ing data policy (nested JSON); substring iki
  // sengaja digoleki ing kabeh response.
  bool diijini = (code == 200 && data.indexOf("\"worker_only\":true") >= 0);
  Serial.print("[BREAK_POLICY] PEKERJA TANPA MASTER=");
  Serial.println(diijini ? "YA" : "ORA");
  return diijini;
}

// =====================================================
// PROSES KARTU
// =====================================================

void prosesStartMode5(String uidPekerja) {
  String tipe, nama;
  bool valid = cekKartu(uidPekerja, tipe, nama);
  if (!valid || tipe != "PEKERJA") {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("START DITOLAK");
    lcd.setCursor(0, 1); lcd.print("TAP PEKERJA");
    beepSalah(); delay(700);
    menungguBreak = false;
    menungguStartMode5 = false;
    uidMasterBreakPending = "";
    resetModeDefault();
    tampilStandby();
    return;
  }

  String status = cekStatusPekerja(uidPekerja);
  Serial.print("[MODE5 START] STATUS="); Serial.println(status);
  if (status != "READY") {
  lcd.clear();
  lcd.setCursor(0, 0);

  if (status == "FINISH") {
    lcd.print("CEK DASBOARD");
    lcd.setCursor(0, 1);
    lcd.print("PILIH ORDER");
  } else {
    lcd.print("START DITOLAK");
    lcd.setCursor(0, 1);
    lcd.print(status.substring(0, 16));
  }
    beepSalah(); delay(900);
    menungguBreak = false;
    menungguStartMode5 = false;
    uidMasterBreakPending = "";
    resetModeDefault();
    tampilStandby();
    return;
  }

  uidPekerjaPending = uidPekerja;
  namaPekerjaPending = nama;
  aksiPending = "START";
  String master = uidMasterBreakPending;
  menungguBreak = false;
  menungguStartMode5 = false;
  uidMasterBreakPending = "";
  Serial.println("[MODE5 START] MASTER->PEKERJA, kirim START");
  kirimSTART(uidPekerjaPending, master);
  resetModeDefault();
  tampilStandby();
}

void prosesBreakPekerja(String uidPekerja) {
  String tipe, nama;
  bool valid = cekKartu(uidPekerja, tipe, nama);

  if (!valid || tipe != "PEKERJA") {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BREAK DITOLAK");
    lcd.setCursor(0, 1);
    lcd.print("TAP PEKERJA");
    beepSalah();
    delay(600);
    resetModeDefault();
    menungguBreak = false;
    menungguStartMode5 = false;
    uidMasterBreakPending = "";
    tampilStandby();
    return;
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BREAK PEKERJA");
  lcd.setCursor(0, 1);
  lcd.print(nama.substring(0, 16));
  beepBeb();
  delay(700);

  kirimBREAK(uidMasterBreakPending, uidPekerja);
}

void prosesValidasi(String uidMaster) {
  String tipe, nama;
  bool valid = cekKartu(uidMaster, tipe, nama);

  if (!valid || tipe != "MASTER") {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NOT VALIDASI");
    lcd.setCursor(0, 1);
    lcd.print("CARD SALAH");
    beepSalah();
    delay(600);

    menungguValidasi = false;
    uidPekerjaPending = "";
    namaPekerjaPending = "";
    aksiPending = "";
    tampilStandby();
    return;
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(aksiPending == "FINISH" ? "ACC FINISH" : "ACC START");
  lcd.setCursor(0, 1);
  lcd.print(nama.length() > 0 ? nama.substring(0, 16) : "MASTER");
  beepOK();
  delay(700);

  if (aksiPending == "FINISH") {
    kirimFINISH(uidPekerjaPending, uidMaster);
  } else {
    kirimSTART(uidPekerjaPending, uidMaster);
  }
}

void prosesKartuStandby(String uid) {
  String tipe, nama;
  bool valid = cekKartu(uid, tipe, nama);

  if (!valid) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("KARTU TIDAK ADA");
    lcd.setCursor(0, 1);
    lcd.print("CEK REGISTRASI");
    beepSalah();
    delay(600);
    tampilStandby();
    return;
  }

  if (tipe == "MASTER") {
    if (uid == UID_KARU) {
      uidMasterBreakPending = uid;
      menungguBreak = true;
      menungguStartMode5 = (modeJeda == 1);

      // Mode 1: START master dhisik, banjur UID tenaga.
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(modeJeda == 1 ? "START OLEH KARU" : "BREAK OLEH KARU");
      lcd.setCursor(0, 1);
      lcd.print("TAP PIN PEKERJA");
      beepBeb();
      delay(700);
      return;
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("MASTER CARD");
    lcd.setCursor(0, 1);
    lcd.print("TAP PIN PEKERJA");
    beepSalah();
    delay(600);
    tampilStandby();
    return;
  }

  if (tipe == "PEKERJA") {
    // Durasi tap ditentukan dari keberadaan UID yang sama di reader.
    // Tap pendek dipakai untuk RETURN/BREAK/START; tahan >=5 detik untuk FINISH.
    bool tapLama = false; // FINISH dipilih lewat Mode 2; ora perlu nahan kartu 5 detik
    String statusPekerja = cekStatusPekerja(uid);
    Serial.print("STATUS PEKERJA = ["); Serial.print(statusPekerja);
    Serial.print("] TAP_LAMA="); Serial.println(tapLama ? "YA" : "TIDAK");

    // RETURN selalu diprioritaskan untuk pekerja yang sedang jeda umum/pekerjaan.
    if (!tapLama && kirimRETURN(uid)) return;

    // MODE 4: FINISH khusus. Tap pekerja langsung njaluk ACC master,
    // ora perlu tahan 5 detik. Mung status WORK sing bisa FINISH.
    if (modeJeda == 2) {
      if (statusPekerja != "WORK") {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("FINISH DITOLAK");
        lcd.setCursor(0,1); lcd.print(statusPekerja.substring(0,16));
        beepSalah(); delay(800); resetModeDefault(); tampilStandby(); return;
      }
      aksiPending = "FINISH";
      uidPekerjaPending = uid;
      namaPekerjaPending = nama;
      menungguValidasi = true;
      lcd.clear(); lcd.setCursor(0,0); lcd.print("PEKERJA FINISH");
      lcd.setCursor(0,1); lcd.print(nama.length() > 0 ? nama.substring(0,16) : "PEKERJA");
      beepBeb(); delay(500); tampilValidasi();
      return;
    }

    // BREAK TOILET Mode 1 tanpa MASTER mung yen Engine Time/PKB ngidini.
    // Ora gumantung status WORK utawa FINISH: kabeh pekerja oleh dicoba BREAK.
    if (!tapLama && modeJeda == 5 && breakToiletWorkerOnly(uid)) {
      kirimBreakToiletPekerja(uid);
      return;
    }

    if (statusPekerja == "WORK") {
      if (!tapLama) {
        lcd.clear();
        lcd.setCursor(0, 0);
        if (modeJeda == 5) lcd.print("LUAR JAM TOILET");
        else lcd.print("BREAK BUTUH ACC");
        lcd.setCursor(0, 1);
        lcd.print("TAP MASTER DULU");
        beepSalah();
        delay(900);
        tampilStandby();
        return;
      }
      aksiPending = "FINISH";
    } else if (statusPekerja == "READY") {
      if (tapLama) {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("FINISH DITOLAK");
        lcd.setCursor(0,1); lcd.print("STATUS READY");
        beepSalah(); delay(800); tampilStandby(); return;
      }
      aksiPending = "START";
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      if (statusPekerja == "FINISH" && modeJeda == 5) lcd.print("LUAR JAM TOILET");
      else lcd.print("BREAK BUTUH ACC");
      lcd.setCursor(0, 1);
      lcd.print("TAP MASTER DULU");
      beepSalah();
      delay(900);
      tampilStandby();
      return;
    }

    uidPekerjaPending = uid;
    namaPekerjaPending = nama;
    menungguValidasi = true;
    lcd.clear(); lcd.setCursor(0,0);
    lcd.print(aksiPending == "FINISH" ? "PEKERJA FINISH" : "PEKERJA START");
    lcd.setCursor(0,1); lcd.print(nama.length() > 0 ? nama.substring(0,16) : "PEKERJA");
    beepBeb(); delay(500); tampilValidasi();
    return;
  }
}

// =====================================================
// SETUP & LOOP
// =====================================================

void setup() {
  Serial.begin(115200);

  resetModeDefault();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

pinMode(TOUCH_MODE_PIN, INPUT_PULLDOWN);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  tampilStandby();

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  Serial.println("=== TES RC522 ===");
byte versi = rfid.PCD_ReadRegister(MFRC522::VersionReg);
Serial.print("RC522 VersionReg: 0x");
Serial.println(versi, HEX);
  delay(50);
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);

  koneksiWiFi();

  resetModeDefault();
  tampilStandby();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long rec = 0;
    if (millis() - rec > 20000) {
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      rec = millis();
    }
  }

  cekModeRegistrasi();
  cekTouchMode();
  cekTimeoutModeDefault();
  scrollStandby();

  static unsigned long resetRFID = 0;
  if (millis() - resetRFID > 30000) {
    rfid.PCD_Init();
    rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    resetRFID = millis();
  }

 if (!rfid.PICC_IsNewCardPresent()) {
  delay(20);
  return;
}

Serial.println("KARTU KETDETEKSI");

if (!rfid.PICC_ReadCardSerial()) {
  Serial.println("KARTU KETDETEKSI, NANGING UID GAGAL");
  delay(20);
  return;
}

Serial.print("UID: ");
for (byte i = 0; i < rfid.uid.size; i++) {
  if (rfid.uid.uidByte[i] < 0x10) Serial.print("0");
  Serial.print(rfid.uid.uidByte[i], HEX);
  if (i < rfid.uid.size - 1) Serial.print(" ");
}
Serial.println();

  if (rfid.uid.size < 4) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  String uid = bacaUID();

  if (modeRegistrasi) {
    if (!menungguValidasi && !menungguBreak) {
      kirimRegistrasi(uid);
      delay(1500);
    }
  } else if (menungguBreak) {
    if (menungguStartMode5) prosesStartMode5(uid);
    else prosesBreakPekerja(uid);
  } else if (menungguValidasi) {
    prosesValidasi(uid);
  } else {
    prosesKartuStandby(uid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(500);
}