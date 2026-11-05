// Sentinela - ESP32 + 2 sensores PIR (Entrada e Corredor) + Telegram + Firebase (Wokwi)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>

#define BOT_TOKEN "SEU_TOKEN_AQUI"
#define CHAT_ID   "SEU_CHAT_ID_AQUI"
// so o endereco, sem https:// e sem barra no final
#define FIREBASE_HOST "seu-projeto-default-rtdb.firebaseio.com"

#define PIN_PIR_A   32   // zona A: Entrada
#define PIN_PIR_B   33   // zona B: Corredor
#define PIN_LED_VD  18
#define PIN_LED_VM  19
#define PIN_BUZZER  21
#define PIN_BOTAO   22

const char* LOCAL = "Residencia - Andar Terreo";
const char* ZONA_NOME[2] = {"Entrada", "Corredor"};
const char* ZONA_COD[2]  = {"ENTRADA", "CORREDOR"};

const unsigned long COOLDOWN_MS = 10000;  // intervalo minimo entre notificacoes
const unsigned long ALARME_MS   = 8000;   // duracao do alarme apos o movimento
const unsigned long DEBOUNCE_MS = 50;
const unsigned long ESTADO_MS   = 2000;   // envio periodico do estado
const unsigned long COMANDO_MS  = 1000;   // consulta de comandos da dashboard
const int FREQ_ALARME = 1800;             // Hz do beep

WiFiClientSecure client;                  // Telegram
WiFiClientSecure fbClient;                // Firebase (conexao mantida aberta)
HTTPClient fbHttp;
UniversalTelegramBot bot(BOT_TOKEN, client);
QueueHandle_t filaTelegram;
QueueHandle_t filaFirebase;

struct Evento {
  char tipo[16];     // INTRUSAO, ARMADO, DESARMADO
  char zona[12];     // ENTRADA, CORREDOR ou vazio
  int ocorrencias;
  unsigned long uptime;
};

bool armado = false;
bool alarme = false;
bool telegramOk = false;
bool buzzerLigado = false;
volatile bool pirAtual[2] = {false, false};
volatile bool forcaEstado = true;
volatile int comandoPendente = 0;         // 1 = armar, 2 = desarmar
int zonaAlarme = -1;                      // zona que disparou o alarme atual
unsigned long inicioAlarme = 0;
unsigned long ultimoEnvio = 0;
bool jaEnviou = false;
int ocorrencias = 0;
int ocZona[2] = {0, 0};

bool botaoAnterior = HIGH;
unsigned long ultimoDebounce = 0;
bool pirAnterior[2] = {false, false};

String horario() {
  unsigned long s = millis() / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(buf);
}

// ---------------------------------------------------------------- Telegram
void tarefaTelegram(void* param) {
  for (;;) {
    String* msg = nullptr;
    if (xQueueReceive(filaTelegram, &msg, portMAX_DELAY) == pdTRUE && msg) {
      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
      unsigned long t0 = millis();
      if (bot.sendMessage(CHAT_ID, *msg, "")) {
        Serial.printf("Telegram: MENSAGEM ENVIADA (%lu ms)\n", millis() - t0);
      } else {
        Serial.println("Telegram: FALHA NO ENVIO (confira BOT_TOKEN, CHAT_ID e se enviou /start ao bot)");
      }
      delete msg;
    }
  }
}

void enviar(const String& texto) {
  String* msg = new String(texto);
  if (xQueueSend(filaTelegram, &msg, 0) != pdTRUE) {
    Serial.println("Fila cheia, mensagem descartada");
    delete msg;
  }
}

// ---------------------------------------------------------------- Firebase
int fbRequest(const char* metodo, const String& caminho, const String& corpo, String* resposta = nullptr) {
  String url = String("https://") + FIREBASE_HOST + caminho + ".json";
  fbHttp.setReuse(true);
  fbHttp.setTimeout(5000);
  fbHttp.setConnectTimeout(5000);
  if (!fbHttp.begin(fbClient, url)) {
    Serial.println("Firebase: begin() falhou, URL: " + url);
    return -1;
  }
  fbHttp.addHeader("Content-Type", "application/json");
  int code = fbHttp.sendRequest(metodo, corpo);
  String body;
  if (code > 0) body = fbHttp.getString();
  if (resposta) *resposta = body;
  if (code < 0) {
    Serial.printf("Firebase: %s (heap livre: %u)\n", fbHttp.errorToString(code).c_str(), ESP.getFreeHeap());
    fbClient.stop();
  }
  fbHttp.end();
  return code;
}

String estadoJson() {
  String j = "{\"armado\":";
  j += armado ? "true" : "false";
  j += ",\"alarme\":";
  j += alarme ? "true" : "false";
  j += ",\"zonaAlarme\":\"";
  if (alarme && zonaAlarme >= 0) j += ZONA_COD[zonaAlarme];
  j += "\",\"entrada\":";
  j += pirAtual[0] ? "true" : "false";
  j += ",\"corredor\":";
  j += pirAtual[1] ? "true" : "false";
  j += ",\"ocEntrada\":" + String(ocZona[0]);
  j += ",\"ocCorredor\":" + String(ocZona[1]);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"ocorrencias\":" + String(ocorrencias);
  j += ",\"atualizadoEm\":{\".sv\":\"timestamp\"}}";
  return j;
}

void tarefaFirebase(void* param) {
  unsigned long ultEstado = 0, ultComando = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long agora = millis();

      if (forcaEstado || agora - ultEstado >= ESTADO_MS) {
        forcaEstado = false;
        ultEstado = agora;
        int c = fbRequest("PUT", "/sentinela/estado", estadoJson());
        if (c != 200) Serial.printf("Firebase: falha ao enviar estado (HTTP %d)\n", c);
      }

      Evento ev;
      while (xQueueReceive(filaFirebase, &ev, 0) == pdTRUE) {
        String corpo = String("{\"tipo\":\"") + ev.tipo + "\",\"zona\":\"" + ev.zona +
                       "\",\"ocorrencias\":" + ev.ocorrencias +
                       ",\"uptime\":" + ev.uptime + ",\"ts\":{\".sv\":\"timestamp\"}}";
        int c = fbRequest("POST", "/sentinela/eventos", corpo);
        Serial.printf("Firebase: evento %s %s (HTTP %d)\n", ev.tipo, ev.zona, c);
      }

      if (agora - ultComando >= COMANDO_MS) {
        ultComando = agora;
        String resp;
        int c = fbRequest("GET", "/sentinela/comando", "", &resp);
        if (c == 200 && resp.indexOf("armar") >= 0) {
          // "desarmar" contem "armar": testa primeiro
          comandoPendente = (resp.indexOf("desarmar") >= 0) ? 2 : 1;
          fbRequest("PUT", "/sentinela/comando", "\"\"");
          Serial.printf("Firebase: comando recebido: %s\n", resp.c_str());
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void evento(const char* tipo, const char* zona) {
  Evento ev;
  strncpy(ev.tipo, tipo, sizeof(ev.tipo) - 1);
  ev.tipo[sizeof(ev.tipo) - 1] = 0;
  strncpy(ev.zona, zona, sizeof(ev.zona) - 1);
  ev.zona[sizeof(ev.zona) - 1] = 0;
  ev.ocorrencias = ocorrencias;
  ev.uptime = millis() / 1000;
  if (xQueueSend(filaFirebase, &ev, 0) != pdTRUE) Serial.println("Fila do Firebase cheia");
}

// ---------------------------------------------------------------- Hardware
void buzzer(bool ligar) {
  if (ligar == buzzerLigado) return;
  buzzerLigado = ligar;
  if (ligar) tone(PIN_BUZZER, FREQ_ALARME);
  else noTone(PIN_BUZZER);
}

void beep(int freq, int ms) {
  tone(PIN_BUZZER, freq);
  delay(ms);
  noTone(PIN_BUZZER);
}

void atualizaLeds() {
  digitalWrite(PIN_LED_VD, armado ? LOW : HIGH);
  digitalWrite(PIN_LED_VM, armado ? HIGH : LOW);
}

void imprimeStatus() {
  Serial.println("================================");
  Serial.println(" SENTINELA - 2 ZONAS");
  Serial.println("================================");
  Serial.printf("Wi-Fi: %s\n", WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");
  Serial.printf("Telegram: %s\n", telegramOk ? "CONECTADO" : "INDISPONIVEL");
  Serial.printf("Sistema: %s\n", armado ? "ARMADO" : "DESARMADO");
  Serial.printf("Entrada: %s | Corredor: %s\n",
                pirAtual[0] ? "MOVIMENTO" : "livre", pirAtual[1] ? "MOVIMENTO" : "livre");
  Serial.printf("Buzzer: %s\n\n", alarme ? "LIGADO" : "DESLIGADO");
}

void setArmado(bool v) {
  if (v == armado) return;
  armado = v;
  alarme = false;
  zonaAlarme = -1;
  buzzer(false);
  atualizaLeds();
  forcaEstado = true;
  evento(armado ? "ARMADO" : "DESARMADO", "");
  beep(armado ? 2400 : 1000, 150);
  imprimeStatus();
  if (armado) {
    jaEnviou = false;
    enviar("SENTINELA ARMADA\nAs zonas Entrada e Corredor estao sendo monitoradas.");
  } else {
    enviar("SENTINELA DESARMADA\nO monitoramento foi desativado.");
  }
}

void disparaAlarme(int zona) {
  ocorrencias++;
  ocZona[zona]++;
  alarme = true;
  zonaAlarme = zona;
  inicioAlarme = millis();
  digitalWrite(PIN_LED_VM, HIGH);
  buzzer(true);
  forcaEstado = true;
  evento("INTRUSAO", ZONA_COD[zona]);

  Serial.println("--------------------------------");
  Serial.printf("MOVIMENTO NA ZONA: %s\n", ZONA_NOME[zona]);
  Serial.println("--------------------------------");
  Serial.printf("Ocorrencia n. %d (%s)\n\n", ocorrencias, horario().c_str());

  // o Telegram so recebe se passou o cooldown (o historico registra tudo)
  if (!jaEnviou || millis() - ultimoEnvio >= COOLDOWN_MS) {
    String msg = "ALERTA DE INTRUSAO\n\n";
    msg += String("Local: ") + LOCAL + "\n";
    msg += String("Zona: ") + ZONA_NOME[zona] + "\n\n";
    msg += "Movimento detectado com o sistema armado.\n";
    msg += "Horario: " + horario() + "\n";
    msg += "Ocorrencias: " + String(ocorrencias) +
           " (Entrada " + String(ocZona[0]) + " / Corredor " + String(ocZona[1]) + ")";
    enviar(msg);
    ultimoEnvio = millis();
    jaEnviou = true;
  } else {
    Serial.printf("Notificacao suprimida (cooldown, faltam %lus)\n\n",
                  (COOLDOWN_MS - (millis() - ultimoEnvio)) / 1000);
  }
}

// padrao sonoro: BEEP-BEEP-BEEP (200ms ligado / 200ms desligado)
void tocaBuzzer() {
  buzzer((millis() / 200) % 2 == 0);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR_A, INPUT);
  pinMode(PIN_PIR_B, INPUT);
  pinMode(PIN_LED_VD, OUTPUT);
  pinMode(PIN_LED_VM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  atualizaLeds();

  Serial.print("Conectando ao Wi-Fi");
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  client.setInsecure();
  fbClient.setInsecure();

  filaTelegram = xQueueCreate(5, sizeof(String*));
  filaFirebase = xQueueCreate(10, sizeof(Evento));
  xTaskCreatePinnedToCore(tarefaFirebase, "firebase", 16384, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(tarefaTelegram, "telegram", 16384, nullptr, 1, nullptr, 0);

  telegramOk = bot.getMe();
  imprimeStatus();
  beep(1500, 100);
}

void loop() {
  bool leitura = digitalRead(PIN_BOTAO);
  if (leitura != botaoAnterior && millis() - ultimoDebounce > DEBOUNCE_MS) {
    ultimoDebounce = millis();
    botaoAnterior = leitura;
    if (leitura == LOW) setArmado(!armado);
  }

  if (comandoPendente) {
    int c = comandoPendente;
    comandoPendente = 0;
    setArmado(c == 1);
  }

  bool pir[2] = {(bool)digitalRead(PIN_PIR_A), (bool)digitalRead(PIN_PIR_B)};
  for (int i = 0; i < 2; i++) {
    if (pir[i] != pirAtual[i]) forcaEstado = true;   // dashboard acompanha cada sensor
    pirAtual[i] = pir[i];
  }

  if (armado) {
    // borda de subida em qualquer um dos dois sensores
    for (int i = 0; i < 2; i++) {
      if (pir[i] && !pirAnterior[i]) disparaAlarme(i);
    }
    if (alarme) {
      tocaBuzzer();
      if (millis() - inicioAlarme >= ALARME_MS) {
        alarme = false;
        zonaAlarme = -1;
        buzzer(false);
        forcaEstado = true;
        Serial.println("Alarme encerrado. Retomando monitoramento.\n");
      }
    }
  }
  for (int i = 0; i < 2; i++) pirAnterior[i] = pir[i];
  delay(10);
}
