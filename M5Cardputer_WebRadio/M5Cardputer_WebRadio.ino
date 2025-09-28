/**
 * @file M5Cardputer_WebRadio.ino
 * @author Aurélio Avanzi
 * @brief https://github.com/cyberwisk/M5Cardputer_WebRadio
 * @version Beta 1.1 custom DP
 * @date 2025-09-28
 *
 * @Hardwares: M5Cardputer ADV
 * @Platform Version: Arduino M5Stack Board Manager v2.3.6
 * @Dependent Library:
 * M5GFX: https://github.com/m5stack/M5GFX
 * M5Unified: https://github.com/m5stack/M5Unified
 * M5Cardputer: https://github.com/m5stack/M5Cardputer
 * Preferences: https://github.com/espressif/arduino-esp32/tree/master/libraries/Preferences
 * ESP32-audioI2S version 3.2.1: https://github.com/schreibfaul1/ESP32-audioI2S
 **/

//Display: Tela TFT de 1.14 polegadas com resolução de 135x240 pixels.
#include "M5Cardputer.h"
#include "CardWifiSetup.h"
#include <Audio.h> //ESP32-audioI2S vesão 

#define MAX_STATIONS 20
#define MAX_NAME_LENGTH 30
#define MAX_URL_LENGTH 100
#define I2S_BCK 41
#define I2S_WS 43
#define I2S_DOUT 42

#define BAT_ADC_PIN 10

// FFT Constants
#define FFT_SIZE 256
#define WAVE_SIZE 320
static uint16_t prev_y[(FFT_SIZE / 2)+1];
static uint16_t peak_y[(FFT_SIZE / 2)+1];
static int header_height = 51; // Altura do FFT
static bool fft_enabled = false;  // Flag para habilitar/desabilitar FFT
static bool fftSimON = true; //Liga a simulação do FFT

bool requestStop = false;
bool requestReconnect = false;
bool requestChangeVolume = false;

String g_stationName;
String g_streamTitle;
String g_id3Title;
SemaphoreHandle_t g_mutex;
bool infoChanged = false;

Audio audio;
// Task handle for audio task
TaskHandle_t handleAudioTask = NULL;

//const long interval = 100;
unsigned long lastUpdate = 0;

// Classe FFT simplificada
class fft_t {
public:
  fft_t() {
    for (int i = 0; i < FFT_SIZE; i++) {
      _data[i] = 0;
    }
  }

  void exec(const int16_t* in) {
    // Simula FFT para demonstração
    if (fftSimON) {
      for (int i = 0; i < FFT_SIZE; i++) {
        _data[i] = abs(in[i]);
      }
    }
  }

  uint32_t get(size_t index) {
    if (index < FFT_SIZE) {
      return _data[index];
    }
    return 0;
  }

private:
  uint32_t _data[FFT_SIZE];
};

static fft_t fft;
static int16_t raw_data[WAVE_SIZE * 2];

// Função para obter cor de fundo baseada na posição Y
static uint32_t bgcolor(int y) {
  auto h = M5Cardputer.Display.height();
  auto dh = h - header_height;
  int v = ((h - y) << 5) / dh;
  if (dh > header_height) {
    int v2 = ((h - y - 1) << 5) / dh;
    if ((v >> 2) != (v2 >> 2)) {
      return 0x666666u;
    }
  }
  return M5Cardputer.Display.color888(v + 2, v, v + 6);
}

struct RadioStation {
  char name[MAX_NAME_LENGTH];
  char url[MAX_URL_LENGTH];
};

const PROGMEM RadioStation defaultStations[] = {
  {"Radio Porao", "https://server03.stmsg.com.br:6678/stream"},
  {"Morcegao FM", "https://radio.morcegaofm.com.br/morcegao32"},
  {"Mundo Livre", "https://up-rcr.webnow.com.br/mundolivre.mp3"},
  {"Radio Mundo do Rock","https://servidor34.brlogic.com:8014/live"},
};

RadioStation stations[MAX_STATIONS];
size_t numStations = 0;
size_t curStation = 0; //qual radio iniciar
uint16_t curVolume = 40;

// Controle de debounce
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

bool isAudioPlay = false;


// get Battery voltage from ADC
// @return {float} voltage(Volt)
float getBattVoltage() {
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  uint32_t mv = analogReadMilliVolts(BAT_ADC_PIN);
  return (mv * 2) / 1000.0f;
}

// change Battery Level
uint8_t getBattLevel(float v) {
  if (v >= 4.12f) return 100;
  if (v >= 3.88f) return 75;
  if (v >= 3.61f) return 50;
  if (v >= 3.40f) return 25;
  return 0;
}

// Battery Level wrapper
uint8_t getBatteryLevel() {
  if(M5.getBoard() == m5::board_t::board_M5CardputerADV){
    return getBattLevel(getBattVoltage());
  } else {
    return M5.Power.getBatteryLevel();
  }
}

// Funções FFT
void setupFFT() {
  if (!fft_enabled) return;
  
  // Inicializa os arrays de FFT
  for (int x = 0; x < (FFT_SIZE / 2) + 1; ++x) {
    prev_y[x] = INT16_MAX;
    peak_y[x] = INT16_MAX;
  }

  // Desenha o fundo gradiente
  int display_height = M5Cardputer.Display.height();
  for (int y = header_height; y < display_height; ++y) {
    M5Cardputer.Display.drawFastHLine(0, y, M5Cardputer.Display.width(), bgcolor(y));
  }
}

void updateFFT() {
  if (!fft_enabled) return;

  static unsigned long lastFFTUpdate = 0;
  if (millis() - lastFFTUpdate < 50) return; // Atualiza a cada 50ms
  lastFFTUpdate = millis();

  // Preencha raw_data com amostras de áudio
  for (int i = 0; i < WAVE_SIZE * 2; i++) {
    raw_data[i] = random(-32000, 32000);
  }

  // Executa FFT
  fft.exec(raw_data);

  // Parâmetros para desenho
  size_t bw = M5Cardputer.Display.width() / 30;
  if (bw < 3) bw = 3;
  int32_t dsp_height = M5Cardputer.Display.height();
  int32_t fft_height = dsp_height - header_height - 1;
  size_t xe = M5Cardputer.Display.width() / bw;
  if (xe > (FFT_SIZE / 2)) xe = (FFT_SIZE / 2);

  uint32_t bar_color[2] = {0x000033u, 0x99AAFFu};

  M5Cardputer.Display.startWrite();
  
  for (size_t bx = 0; bx <= xe; ++bx) {
    size_t x = bx * bw;
    int32_t f = fft.get(bx) * 3; //intencidade 
    int32_t y = (f * fft_height) >> 17; //Escala da FFT original =18
    if (y > fft_height) y = fft_height;
    y = dsp_height - y;
    int32_t py = prev_y[bx];
    if (y != py) {
      M5Cardputer.Display.fillRect(x, y, bw - 1, py - y, bar_color[(y < py)]);
      prev_y[bx] = y;
    }
    py = peak_y[bx] + ((peak_y[bx] - y) > 5 ? 2 : 1);
    if (py < y) {
      M5Cardputer.Display.writeFastHLine(x, py - 1, bw - 1, bgcolor(py - 1));
    } else {
      py = y - 1;
    }
    if (peak_y[bx] != py) {
      peak_y[bx] = py;
      M5Cardputer.Display.writeFastHLine(x, py, bw - 1, TFT_WHITE);
    }
  }
  
  M5Cardputer.Display.endWrite();
}

void toggleFFT() {
  fft_enabled = !fft_enabled;
  M5Cardputer.Display.fillRect(0, 51, 240, 89, TFT_BLACK);
  if (fft_enabled) {
    setupFFT();
  }
}

void updateBatteryDisplay(unsigned long updateInterval) {
  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate >= updateInterval) {
    lastUpdate = millis();
    int32_t batteryLevel = getBatteryLevel();
    uint16_t batteryColor = batteryLevel < 30 ? TFT_RED : TFT_GREEN;

    M5Cardputer.Display.fillRect(215, 5, 40, 12, TFT_BLACK); 

    M5Cardputer.Display.fillRect(215, 5, 20, 10, TFT_DARKGREY);
    M5Cardputer.Display.fillRect(235, 7, 3, 6, TFT_DARKGREY);
    M5Cardputer.Display.fillRect(217, 7, (batteryLevel * 16) / 100, 6, batteryColor);
  }
}

void loadDefaultStations() {
  numStations = std::min(sizeof(defaultStations)/sizeof(defaultStations[0]), static_cast<size_t>(MAX_STATIONS));
  memcpy(stations, defaultStations, sizeof(RadioStation) * numStations);
}

void mergeRadioStations() {
  if (!SD.begin()) {
    M5Cardputer.Display.drawString("/station_list.txt ", 8, 30);
    M5Cardputer.Display.drawString("NOT Found on SD", 8, 50);
    delay(4000);
    loadDefaultStations();
    M5Cardputer.Display.fillScreen(BLACK);
    return;
  }

  File file = SD.open("/station_list.txt");
  if (!file) {
    loadDefaultStations();
    return;
  }

  numStations = 0;
  
  String line;
  while (file.available() && numStations < MAX_STATIONS) {
    line = file.readStringUntil('\n');
    int commaIndex = line.indexOf(',');
    
    if (commaIndex > 0) {
      String name = line.substring(0, commaIndex);
      String url = line.substring(commaIndex + 1);
      
      name.trim();
      url.trim();
      
      if (name.length() > 0 && url.length() > 0) {
        strncpy(stations[numStations].name, name.c_str(), MAX_NAME_LENGTH - 1);
        strncpy(stations[numStations].url, url.c_str(), MAX_URL_LENGTH - 1);
        stations[numStations].name[MAX_NAME_LENGTH - 1] = '\0';
        stations[numStations].url[MAX_URL_LENGTH - 1] = '\0';
        numStations++;
      }
    }
  }

  file.close();
  if (numStations == 0) {
    loadDefaultStations();
  }
}

void showStation() {
  fftSimON = false;
  M5Cardputer.Display.fillRect(0, 15, 240, 35, TFT_BLACK);
  M5Cardputer.Display.drawString(stations[curStation].name, 0, 15);
  showVolume();
}


void Playfile() {
  isAudioPlay = false;
  audio.stopSong();
    
  String url = stations[curStation].url; // Armazena a URL para facilitar o acesso
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
      g_stationName = stations[curStation].name;
      g_streamTitle = "";
      g_id3Title = "";
      infoChanged = true;
      xSemaphoreGive(g_mutex);
  }
  
  vTaskDelay(20 / portTICK_PERIOD_MS);
  
  if (url.indexOf("http") != -1) { 
    audio.connecttohost(stations[curStation].url);
    isAudioPlay = true;
  } else if (url.indexOf("/mp3") != -1) {
    audio.connecttoFS(SD,stations[curStation].url);
    isAudioPlay = true;
  }
}


bool isMuted = false;
uint16_t prevVolume = 0;

void volumeUp() {
  if(isMuted) {
    prevVolume = min(prevVolume + 10, 200);
    if (prevVolume > 200) prevVolume = 200;
  } else {
    curVolume = min(curVolume + 10, 200);
    if (curVolume > 200) curVolume = 200;
    requestChangeVolume = true;
  }
}

void volumeDown() {
  if(isMuted) {
    prevVolume = max(prevVolume - 10, 0);
    if (prevVolume < 1) prevVolume = 1;
  } else {
    curVolume = max(curVolume - 10, 0);
    if (curVolume < 1) curVolume = 1;
    requestChangeVolume = true;
  }
}

void volumeMute() {
  if (!isMuted) {
    prevVolume = curVolume;
    curVolume = 0;
    isMuted = true;
  } else {
    curVolume = prevVolume;
    isMuted = false;
  }
  requestChangeVolume = true;
}

void showVolume() {
  static uint8_t lastVolume = 0; // Rastrear o último volume
  uint8_t currentVolume = (isMuted)? prevVolume : curVolume; // Usar variável global

  if (currentVolume != lastVolume) {
    lastVolume = currentVolume;

    int barHeight = 4; // Altura da barra
    M5Cardputer.Display.fillRect(0, 6, 200, 6, TFT_BLACK);
    int barWidth = map(currentVolume, 0, 200, 0, M5Cardputer.Display.width());
    if (barWidth < 200) {
      M5Cardputer.Display.fillRect(0, 6, barWidth, barHeight, 0xAAFFAA); // Verde claro
    }
  }
}

void stationUp() {
  if (numStations > 0) {
    curStation = (curStation + 1) % numStations;
    requestReconnect = true;
  }
}

void stationDown() {
  if (numStations > 0) {
    curStation = (curStation - 1 + numStations) % numStations; 
    requestReconnect = true;
  }
}

void setup() {
  Serial.begin(115200);
  g_mutex = xSemaphoreCreateMutex();
  requestReconnect = true;
  
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  
  M5Cardputer.Display.begin();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::FreeMono9pt7b);

  auto spk_cfg = M5Cardputer.Speaker.config();
  /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
  spk_cfg.sample_rate = 128000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
  //spk_cfg.task_pinned_core = APP_CPU_NUM;
  M5Cardputer.Speaker.config(spk_cfg);
  M5Cardputer.Speaker.begin();

  audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT);
  audio.setVolume((uint8_t)map(curVolume, 0, 200, 0, 20));
  audio.setBalance(0);
  audio.setBufferSize(8*1024);
  //audio.setAudioTaskCore(0);
  
  connectToWiFi();
  M5.delay(5);
  M5Cardputer.Display.clear();

  mergeRadioStations();
  xTaskCreatePinnedToCore(Task_Audio, "Task_Audio", 20480, NULL, 1, &handleAudioTask, 1); // Core 1
}

void loop() {
  M5Cardputer.update();
  updateBatteryDisplay(5000);

  String station, title, id3title;

  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
      station = g_stationName;
      title   = g_streamTitle;
      id3title = g_id3Title;
      xSemaphoreGive(g_mutex);
  }

  if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {
      if (M5Cardputer.Keyboard.isKeyPressed(';')) volumeUp();
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) volumeDown();
      else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
      else if (M5Cardputer.Keyboard.isKeyPressed('/')) stationUp();
      else if (M5Cardputer.Keyboard.isKeyPressed(',')) stationDown();
      else if (M5Cardputer.Keyboard.isKeyPressed('r')) {
        requestReconnect = true;
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('p')) {
        M5Cardputer.Display.fillRect(0, 15, 240, 49, TFT_BLACK);  
        M5Cardputer.Display.drawString("Stop", 0, 15);
        requestStop = true;
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('f')) {
        toggleFFT();  //tecla 'f' para ativar/desativar FFT
      }
      
      lastButtonPress = millis();
   }
  
  if (fft_enabled) {
    updateFFT();
  }

  if(infoChanged) {
    infoChanged = false;
    //M5Cardputer.Display.startWrite();
    if(station) {
      M5Cardputer.Display.fillRect(0, 15, 240, 15, TFT_BLACK);
      M5Cardputer.Display.drawString(station.c_str(), 0, 15);
    }

    if(title) {
      M5Cardputer.Display.fillRect(0, 33, 240, 15, TFT_BLACK);
      M5Cardputer.Display.drawString(title.c_str(), 0, 33);
    }
    if(id3title) M5Cardputer.Display.drawString(id3title.c_str(), 0, 33);
    //M5Cardputer.Display.endWrite();
  }
  
  showVolume();

  M5.delay(20);
}

void Task_Audio(void *pvParameters) {
  int stopCount = 0;
  while (1) {
    if(requestChangeVolume) {
      requestChangeVolume = false;
      audio.setVolume(map(curVolume, 0, 200, 0, 20));
    }
    if(requestStop) {
      requestStop = false;
      isAudioPlay = false;
      audio.stopSong();
    }
    if(requestReconnect) {
      requestReconnect = false;
      Playfile();
    }
    if(isAudioPlay) {
      if(audio.isRunning()) {
        stopCount = 0;
        audio.loop();
      } else {
        stopCount++;
        if(stopCount > 100) {
          stopCount = 0;
          isAudioPlay = false;
          requestReconnect = true;
        }
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
    } else {
      vTaskDelay(20 / portTICK_PERIOD_MS);
    }
  }
}

void audio_showstation(const char *showstation) {
  if (showstation && *showstation) { // Verifica se a string não é nula e não está vazia
    String tmp(showstation);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    fftSimON = true;
    g_stationName = tmp;
    infoChanged = true;
    xSemaphoreGive(g_mutex);
  }
}

void audio_showstreamtitle(const char *info) {
  if (info && *info) { 
    String tmp(info);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_streamTitle = tmp;
    infoChanged = true;
    xSemaphoreGive(g_mutex);
  }
}

void audio_id3data(const char *info){
  if (info && *info) { 
    String tmp(info);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_id3Title = tmp;
    infoChanged = true;
    xSemaphoreGive(g_mutex);
  }
}

// optional
//void audio_info(const char *info){M5Cardputer.Display.drawString(info, 0, 46);}
//void audio_id3data(const char *info){M5Cardputer.Display.drawString(info, 0, 46);}
//void audio_eof_mp3(const char *info){M5Cardputer.Display.drawString(info, 0, 56);}
//void audio_bitrate(const char *info){M5Cardputer.Display.drawString(info, 0, 70);}
//void audio_commercial(const char *info){M5Cardputer.Display.drawString(info, 0, 50);}
//void audio_showstation(const char *info){M5Cardputer.Display.drawString(info, 0, 50);}
//void audio_icyurl(const char *info){M5Cardputer.Display.drawString(info, 0, 50);}
//void audio_lasthost(const char *info){M5Cardputer.Display.drawString(info, 0, 50);}
//void audio_eof_speech(const char *info){M5Cardputer.Display.drawString(info, 0, 50);}
