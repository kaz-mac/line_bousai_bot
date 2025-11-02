/*
  line_bousai_bot.ino
  火災報知器の警報をLINEに通知する for M5Stack Atom Lite
  （ESP32 Arduino Core 3.0想定）

  Copyright (c) 2025 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT

  接続方法（GPIO 32は内部プルアップ＋外部プルアップ）
    接点1 --+-- GPIO 32
            +-- R 10KΩ --- 3.3V
    接点2 --- R 330Ω --- GND
*/
#include <M5Unified.h>
#include "esp32-hal-timer.h"

// WiFiとLINEの設定
const char* WIFI_SSID = "****";   // WiFiのSSID
const char* WIFI_PASS = "****";  // WiFiのパスワード

// LINEチャネルアクセストークン --> https://developers.line.biz/ja/
const char* ACCESS_TOKEN = "****";

// 基本動作設定
const uint32_t REPEAT_TEST_TIME = 86400*7;  // テストメッセージを送信する間隔(s)
const uint32_t REPEAT_ALARM_TIME = 60*3; // 警報メッセージを送信する間隔(s)

// ウォッチドックタイマー
#define WDT_TIMER_SETUP 5*60  // setup()でのタイムアウト [sec] 5分
#define WDT_TIMER_LOOP  5*60  // loop()でのタイムアウト  [sec] 5分
hw_timer_t *wdtimer0 = NULL;

// LINE Messaging APIライブラリ
#include "ESP32LineMessenger.h"   // https://github.com/tomorrow56/ESP32_LINE_Messaging_API
ESP32LineMessenger line;
#define LINE_DEBUG false

// GPIO設定
#define GPIO_ALERM 32 // GPIOポート Groveポート 火災報知器接点へ
#define GPIO_LED 27   // GPIOポート 内蔵LED

// LED関係
#include <FastLED.h>
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

// グローバル変数
uint32_t last_alarm_ms = 0;   // 最後にテストメッセージを送信した時間
uint32_t last_test_ms = 0;    // 最後に警報メッセージを送信した時間

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)


// ---------------------------------------------------------------------------

// システムを再起動する
void IRAM_ATTR wdt_reboot() {
  Serial.println("\n*** REBOOOOOOT!!");
  // M5.Power.reset();
  esp_restart();
}

// WDTのカウンターをクリアする
void wdt_clear() {
  timerRestart(wdtimer0);
}

// LEDの色を変える
void led_color(uint8_t r, uint8_t g, uint8_t b) {
  leds[0] = CRGB(r, g, b);
  FastLED.show();
} 

// Wi-Fi接続する
bool wifiConnect() {
  bool stat = false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Wifi connecting.");
    for (int j=0; j<10; j++) {
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASS);  //  Wi-Fi APに接続
      for (int i=0; i<10; i++) {
        if (WiFi.status() == WL_CONNECTED) break;
        Serial.print(".");
        delay(500);
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("connected!");
        Serial.println(WiFi.localIP());
        stat = true;
        break;
      } else {
        Serial.println("failed");
        WiFi.disconnect();
      }
    }
  }
  return stat;
}

// LINEメッセージ送信
bool send_message(String message) {
  bool res = false;
  sp("LINEメッセージ: "+message);
  if (WiFi.status() == WL_CONNECTED) {
    res = line.sendMessage(message.c_str(), LINE_DEBUG);
  }
  sp("結果: "+String(res?"成功":"失敗"));
  return res;
}

// ---------------------------------------------------------------------------

// セットアップ
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
	Serial.begin(115200);
	sp("system start!");

  // GPIO設定
  pinMode(GPIO_ALERM, INPUT_PULLUP);
  pinMode(GPIO_LED, OUTPUT);

  // LEDの設定
  FastLED.addLeds<WS2811, GPIO_LED, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(10);
  led_color(255, 255, 0); // LED 黄
  
  // ウォッチドックタイマー setup()用
  wdtimer0 = timerBegin(1000000);  // 80MHz / 80 = 1MHz = 1000000 Hz
  timerAttachInterrupt(wdtimer0, &wdt_reboot);
  timerAlarm(wdtimer0, WDT_TIMER_SETUP*1000000, false, 0);
  
  // LINE設定
  line.setAccessToken(ACCESS_TOKEN);
  line.setDebug(LINE_DEBUG);
  
  // WiFi接続
  while(!wifiConnect()) {
    delay(10000);
  }

  // ウォッチドックタイマー設定 loop()用
  wdt_clear();
  timerAlarm(wdtimer0, WDT_TIMER_LOOP*1000000, false, 0); //set time in us

  // セットアップ完了
  led_color(0, 255, 0); // LED 緑
  sp("Setup ok!");
}

// ---------------------------------------------------------------------------

// メインループ
void loop() {
  bool fire = false;
  static bool last_fire = true;
	M5.update();

  // WiFiが切れてたらLED黄点滅
  if (WiFi.status() == WL_CONNECTED) {
    wdt_clear(); // WDTタイマークリア
  } else {
    for (int i=0; i<5; i++) {
      led_color(255, 255, 0); // LED 黄
      delay(100);
      led_color(0, 0, 0); // LED 消灯
      delay(100);
    }
    led_color(0, 255, 0); // LED 緑
  }

  // 火災報知器の状態をチェックし、警報をメッセージを送信する
  fire = digitalRead(GPIO_ALERM);
  if (fire == LOW) {
    if (last_alarm_ms == 0 || (uint32_t)(millis() - last_alarm_ms) > REPEAT_ALARM_TIME*1000) {
      send_message("火事です! 火災報知器が反応しました");
      last_alarm_ms = millis();
    }
    led_color(255, 0, 0); // LED 赤
  } else {
    if (last_fire != fire) {
      led_color(0, 255, 0); // LED 緑
    }
  }
  last_fire = fire;

  // 1週間に1回テストメッセージを送信する
  if ((uint32_t)(millis() - last_test_ms) > REPEAT_TEST_TIME*1000) {
    send_message("[テスト] これは火災報知器のテストメッセージです（週1回）");
    last_test_ms = millis();
  }

  // Aボタン1秒長押しでテスト送信
  if (M5.BtnA.pressedFor(1000)) {
    send_message("[テスト] 手動テストメッセージです");
  }

  delay(50);
}
