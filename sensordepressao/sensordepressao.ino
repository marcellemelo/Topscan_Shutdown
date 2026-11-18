#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// --- Mapeamento de Hardware ---
const int pinoMosfet = 33;      // Saída para o Gate do MOSFET
const int pinoLedDados = 25;    // Fio de dados do LED RGB (WS2812B)
const int SDA_PIN = 19;         // I2C Dados
const int SCL_PIN = 22;         // I2C Clock

// --- Configuração da Rede Wi-Fi Criada pelo ESP32 ---
const char* ssidAP = "TopScan_Shutdown";     // Nome da rede Wi-Fi
const char* senhaAP = "topscan123";      // Senha (mínimo 8 caracteres)
WebServer server(80);

// --- LED RGB (WS2812B) ---
#define NUM_LEDS 1
Adafruit_NeoPixel led(NUM_LEDS, pinoLedDados, NEO_RGB + NEO_KHZ800);

const uint32_t COR_VERDE    = led.Color(0, 255, 0);    // Normal (Seguro - Automático)
const uint32_t COR_LARANJA  = led.Color(255, 80, 0);   // Erro de Pressão / Desligado
const uint32_t COR_AZUL     = led.Color(0, 80, 255);   // Modo Manual (Forçado ON)
const uint32_t COR_APAGADO  = led.Color(0, 0, 0);      // Auxiliar para piscar

unsigned long tempoAnterior = 0;
const long intervaloPisca = 300;
bool estadoPisca = false;
uint32_t corAtual = 0;

// --- Configuração I2C e Calibração ---
byte enderecoSensor = 0;
const float limiarSuperior = 3;     // >= 3 PSI-> Circuito LIGA (MOSFET ON)
const float limiarInferior = 2.5;     // <= 2.5 PSI -> Circuito DESLIGA (MOSFET OFF)
//const float offsetCalibracao = 138.99; 

// --- Variáveis Globais (Compartilhadas com o Servidor Web) ---
float pressaoAbsoluta = 0.0;
bool pressaoSegura = false;   // Resultado da avaliação automática do sensor
bool estadoMosfet = false;    // Estado físico final aplicado ao MOSFET
bool leituraI2C_OK = false;

// Modos de operação: 0 = Automático | 1 = Forçado ON | 2 = Forçado OFF
int modoOperacao = 0;

void mostrarCor(uint32_t novaCor) {
  if (corAtual != novaCor) {
    led.setPixelColor(0, novaCor);
    led.show();
    corAtual = novaCor;
  }
}

// Rota para alterar o modo de operação pelos botões do site
void handleModo() {
  if (server.hasArg("set")) {
    String valor = server.arg("set");
    if (valor == "on") {
      modoOperacao = 1;
    } else if (valor == "off") {
      modoOperacao = 2;
    } else {
      modoOperacao = 0;
    }
  }
  server.send(200, "text/plain", "OK");
}

// Rota que fornece os dados em JSON para atualização contínua
void handleStatus() {
  String json = "{";
  json += "\"pressao\":" + String(pressaoAbsoluta, 2) + ",";
  json += "\"mosfet\":" + String(estadoMosfet ? "true" : "false") + ",";
  json += "\"seguro\":" + String(pressaoSegura ? "true" : "false") + ",";
  json += "\"sensor\":" + String(leituraI2C_OK ? "true" : "false") + ",";
  json += "\"modo\":" + String(modoOperacao);
  json += "}";
  server.send(200, "application/json", json);
}

// Página principal exibida no navegador do celular/PC
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>TopScan - Controle e Monitoramento</title>
  <style>
    body { font-family: Arial, sans-serif; background: #121212; color: #f5f5f5; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 16px; box-sizing: border-box; }
    .card { background: #1e1e1e; padding: 26px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); text-align: center; width: 100%; max-width: 380px; }
    h1 { font-size: 1.3rem; margin: 0 0 16px 0; color: #cccccc; }
    .valor { font-size: 3rem; font-weight: bold; margin: 8px 0; color: #ffffff; }
    .unidade { font-size: 1rem; color: #888888; }
    .badge { display: block; padding: 10px 14px; border-radius: 8px; font-weight: bold; margin-top: 16px; font-size: 0.95rem; }
    .on { background: #1b5e20; color: #a5d6a7; }
    .off { background: #e65100; color: #ffe0b2; }
    .manual-on { background: #0d47a1; color: #bbdefb; }
    .erro { background: #b71c1c; color: #ffcdd2; }
    .alerta-sensor { font-size: 0.85rem; margin-top: 10px; color: #ffab40; min-height: 18px; }
    .controles { margin-top: 22px; display: flex; flex-direction: column; gap: 10px; border-top: 1px solid #333; padding-top: 18px; }
    .titulo-ctrl { font-size: 0.9rem; color: #aaaaaa; margin-bottom: 4px; text-transform: uppercase; letter-spacing: 0.5px; }
    button { padding: 13px; border: none; border-radius: 8px; font-size: 0.95rem; font-weight: bold; cursor: pointer; transition: opacity 0.2s; }
    button:active { opacity: 0.75; }
    .btn-on { background: #2e7d32; color: #ffffff; }
    .btn-off { background: #c62828; color: #ffffff; }
    .btn-auto { background: #37474f; color: #eceff1; }
    .ativo { outline: 2px solid #ffffff; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Monitoramento do Vaso</h1>
    <div class="valor" id="pressao">--.--</div>
    <div class="unidade">PSI (Pressão Absoluta)</div>
    
    <div id="status" class="badge off">Aguardando leitura...</div>
    <div id="alertaSensor" class="alerta-sensor"></div>

    <div class="controles">
      <div class="titulo-ctrl">Controle Manual (Override)</div>
      <button id="btnOn" class="btn-on" onclick="definirModo('on')">FORÇAR LIGAR (ON)</button>
      <button id="btnOff" class="btn-off" onclick="definirModo('off')">FORÇAR DESLIGAR (OFF)</button>
      <button id="btnAuto" class="btn-auto" onclick="definirModo('auto')">MODO AUTOMÁTICO (SEGURANÇA)</button>
    </div>
  </div>

  <script>
    function definirModo(modo) {
      fetch('/modo?set=' + modo).then(() => atualizarStatus());
    }

    function atualizarStatus() {
      fetch('/status')
        .then(res => res.json())
        .then(data => {
          const elPressao = document.getElementById('pressao');
          const elStatus = document.getElementById('status');
          const elAlerta = document.getElementById('alertaSensor');

          // Atualiza valor da pressão
          elPressao.innerText = data.sensor ? data.pressao.toFixed(1) : 'ERRO';

          // Atualiza aviso da condição real do sensor
          if (!data.sensor) {
            elAlerta.innerText = 'Aviso: Falha na leitura do sensor I2C';
          } else if (!data.seguro) {
            elAlerta.innerText = 'Aviso: Pressão fora da faixa segura (< 2.5 PSI)';
          } else {
            elAlerta.innerText = 'Sensor dentro da faixa normal (> 3 PSI)';
          }

          // Atualiza badge principal conforme modo e estado do MOSFET
          if (data.modo === 1) {
            elStatus.innerText = 'MOSFET: ON (FORÇADO MANUAL)';
            elStatus.className = 'badge manual-on';
          } else if (data.modo === 2) {
            elStatus.innerText = 'MOSFET: OFF (FORÇADO MANUAL)';
            elStatus.className = 'badge erro';
          } else {
            if (data.mosfet) {
              elStatus.innerText = 'MOSFET: ON (AUTOMÁTICO)';
              elStatus.className = 'badge on';
            } else {
              elStatus.innerText = 'MOSFET: OFF (BLOQUEIO DE SEGURANÇA)';
              elStatus.className = 'badge off';
            }
          }

          // Destaca o botão ativo
          document.getElementById('btnOn').classList.toggle('ativo', data.modo === 1);
          document.getElementById('btnOff').classList.toggle('ativo', data.modo === 2);
          document.getElementById('btnAuto').classList.toggle('ativo', data.modo === 0);
        })
        .catch(() => {
          document.getElementById('status').innerText = 'SEM CONEXÃO COM ESP32';
        });
    }

    setInterval(atualizarStatus, 1000);
    atualizarStatus();
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(9600);

  pinMode(pinoMosfet, OUTPUT);
  digitalWrite(pinoMosfet, LOW);

  led.begin();
  led.setBrightness(100);
  mostrarCor(COR_LARANJA);

  // Inicia o Ponto de Acesso Wi-Fi
  WiFi.softAP(ssidAP, senhaAP);
  Serial.print("\nRede Wi-Fi iniciada: ");
  Serial.println(ssidAP);
  Serial.print("Acesse no navegador: http://");
  Serial.println(WiFi.softAPIP());

  // Configura as rotas do Servidor Web
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/modo", handleModo);
  server.begin();

  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println("A procurar o sensor de pressao no barramento I2C...");
  for (byte endereco = 1; endereco < 127; endereco++) {
    Wire.beginTransmission(endereco);
    if (Wire.endTransmission() == 0) {
      enderecoSensor = endereco;
      Serial.print("SUCESSO: Sensor encontrado no endereço 0x");
      Serial.println(endereco, HEX);
      break;
    }
  }

  if (enderecoSensor == 0) {
    Serial.println("ERRO: Nenhum dispositivo I2C encontrado.");
  }
  delay(1000);
}

void loop() {
  server.handleClient();

  // ==========================================
  // 1. LEITURA SENSOR I2C (PRESSÃO) 
  // ==========================================
  long pressaoBruta = 0;
  float deltaPressao = 0.0;
  leituraI2C_OK = false;

  if (enderecoSensor > 0) {
    Wire.beginTransmission(enderecoSensor);
    Wire.write(0x30);
    Wire.write(0x0A);
    Wire.endTransmission();

    delay(20);

    Wire.beginTransmission(enderecoSensor);
    Wire.write(0x06); 
    Wire.endTransmission(false);

    Wire.requestFrom((int)enderecoSensor, 3);
    if (Wire.available() == 3) {
      pressaoBruta |= (long)Wire.read() << 16;
      pressaoBruta |= (long)Wire.read() << 8;
      pressaoBruta |= (long)Wire.read();

      if (pressaoBruta & 0x800000) pressaoBruta -= 0x1000000;
      
      // deltaPressao = (pressaoBruta / 2097152.0) * 100.0;
      // pressaoAbsoluta = (101.32 - deltaPressao) + offsetCalibracao;
      //pressaoAbsoluta = -2.35 * pow(10,-6) * (float)pressaoBruta + 17.44;
      pressaoAbsoluta = -2.353146E-06 * (float)pressaoBruta ;
      leituraI2C_OK = true;
    }
  }

  // ==========================================
  // 2. AVALIAÇÃO DE ESTADOS (HISTERESE + OVERRIDE)
  // ==========================================
  if (leituraI2C_OK) {
    if (pressaoAbsoluta >= limiarSuperior) pressaoSegura = true;
    else if (pressaoAbsoluta <= limiarInferior) pressaoSegura = false;
  } else {
    pressaoSegura = false;
  }

  // Aplica o modo selecionado na interface web
  if (modoOperacao == 1) {
    estadoMosfet = true;   // Forçado ON (ignora qualquer erro)
  } else if (modoOperacao == 2) {
    estadoMosfet = false;  // Forçado OFF
  } else {
    estadoMosfet = pressaoSegura; // Modo Automático
  }
  
  // ==========================================
  // 3. AÇÃO FÍSICA E SINALIZAÇÃO VISUAL
  // ==========================================
  digitalWrite(pinoMosfet, estadoMosfet ? HIGH : LOW);

  unsigned long tempoAtual = millis();
  if (tempoAtual - tempoAnterior >= intervaloPisca) {
    tempoAnterior = tempoAtual;
    estadoPisca = !estadoPisca;
  }

  if (modoOperacao == 1) {
    // Azul fixo indica visualmente no vaso que a proteção está em override manual (ON)
    mostrarCor(COR_AZUL);
  } else if (!estadoMosfet) {
    // Laranja piscando quando desligado (por erro de pressão ou forçado OFF)
    mostrarCor(estadoPisca ? COR_LARANJA : COR_APAGADO);
  } else {
    // Verde fixo em operação automática segura
    mostrarCor(COR_VERDE);
  }

  // ==========================================
  // 4. MONITORIZAÇÃO SERIAL
  // ==========================================
  Serial.print("Modo:");
  if (modoOperacao == 1) Serial.print("MAN_ON ");
  else if (modoOperacao == 2) Serial.print("MAN_OFF");
  else Serial.print("AUTO   ");

  Serial.print("\tMosfet:");
  Serial.print(estadoMosfet ? "ON " : "OFF");

  if (!leituraI2C_OK) {
    Serial.println("\tI2C: FALHA/AUSENTE");
  } else {
    Serial.print("\tPressao_Absoluta_PSI:");
    Serial.println(pressaoAbsoluta, 2);
  }

  delay(10); 
}