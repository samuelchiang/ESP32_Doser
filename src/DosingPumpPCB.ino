#include <FS.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <SPIFFS.h>
#include <HTTPClient.h>

const int LEDPin = BUILTIN_LED;

//{Motor1, Motor2, Motor3, Motor4}
// Motor1 {motor1Pin1, motor1Pin2, enable1Pin}
int MotorPins[4][3] = {{13, 27, 12}, {26, 32, 14}, {16, 17, 4}, {18, 19, 5}};

bool block_pump[4] = {false, false, false, false};

// Setting PWM properties
const int freq = 30000;
const int resolution = 8;
int dutyCycle = 200;

//define your default values here, if there are different values in config.json, they are overwritten.
char mqttServer[40] = "";  // MQTT伺服器位址
char mqttPort[6]  = "1883";
char mqttUserName[32] = "";  // 使用者名稱
char mqttPwd[32] = "";  // MQTT密碼
char DeviceId[32] = "dosing_box_pcb";  // Device ID

char EventTopic[64];
char CommandTopic[64];
char CommandRespTopic[64];

//flag for saving data
bool shouldSaveConfig = false;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

//callback notifying us of the need to save configcommand_id
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

const int buttonPin = 0;  
int buttonState = 0;

//--------------- Wifi
void setup_wifi() {
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  //wm.resetSettings();

  //WifiManager 還提供一個特別的功能，就是可以在 Config Portal 多設定其他的參數，
  // 這等於是提供了參數的設定介面，可以避免將參數寫死在程式碼內。流程如下所述
  //     1. 開機時，讀取 SPIFFS, 取得 config.json 檔案，讀入各參數，若沒有，則採用預設值
  //     2. 若 ESP 32 進入 AP 模式，則在 Config Portal 可設定參數
  //     3. 設定完之後，將參數寫入 SPIFFS 下的 config.json
  //
  // setup custom parameters
  //
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqttServer("mqttServer", "mqtt server", mqttServer, 40);
  WiFiManagerParameter custom_mqttPort("mqttPort", "mqtt port", mqttPort, 6);
  WiFiManagerParameter custom_mqttUserName("mqttUserName", "mqtt user name", mqttUserName, 32);
  WiFiManagerParameter custom_mqttPwd("mqttPwd", "mqtt password", mqttPwd, 32);
  WiFiManagerParameter custom_DeviceId("DeviceId", "Device ID", DeviceId, 32);

  //add all your parameters here
  wm.addParameter(&custom_mqttServer);
  wm.addParameter(&custom_mqttPort);
  wm.addParameter(&custom_mqttUserName);
  wm.addParameter(&custom_mqttPwd);
  wm.addParameter(&custom_DeviceId);

  bool res;
  // res = wm.autoConnect(); // auto generated AP name from chipid
  //res = wm.autoConnect("SmartPlug"); // anonymous ap
  //若重啟後，還是連不上家裡的  Wifi AP, ESP32 就會進入設定的 AP 模式，必須將這個模式設定三分鐘 timeout 後重啟，
  //萬一家裡的 Wifi AP 恢復正常，ESP32 就可以直接連線。這裡的三分鐘 timeout, 經過測試，只要有在 Config Portal 頁面操作，
  //每次都會重置三分鐘timeout，所以在設定時，不需緊張。
  wm.setConfigPortalTimeout(180);//seconds
  res = wm.autoConnect(DeviceId, "1qaz2wsx"); // password protected ap
  if (!res) {
    Serial.println("Failed to connect");
    ESP.restart();
  }
  else {
    //if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");

    //IPAddress ip; 
    //ip = WiFi.localIP();
    //notify_ip_by_line(IpAddress2String( ip));
  }

  //read updated parameters
  strcpy(mqttServer, custom_mqttServer.getValue());
  strcpy(mqttPort, custom_mqttPort.getValue());
  strcpy(mqttUserName, custom_mqttUserName.getValue());
  strcpy(mqttPwd, custom_mqttPwd.getValue());
  strcpy(DeviceId, custom_DeviceId.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonDocument doc(1024);

    doc["mqttServer"]   = mqttServer;
    doc["mqttPort"]     = mqttPort;
    doc["mqttUserName"] = mqttUserName;
    doc["mqttPwd"]      = mqttPwd;
    doc["DeviceId"]     = DeviceId;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    serializeJsonPretty(doc, Serial);
    serializeJson(doc, configFile);
    configFile.close();
    //end save
    shouldSaveConfig = false;
  }
}

//--------------- MQTT
void mqttReconnect() {
  int countdown = 5;
  while (!mqttClient.connected()) {
    if (mqttClient.connect(DeviceId, mqttUserName, mqttPwd)) {
      Serial.println("MQTT connected");
      mqttClient.subscribe(CommandTopic);
      mqttClient.setCallback(mqttCallback);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);  // 等5秒之後再重試'
      //設置 timeout, 過了 25 秒仍無法連線, 就重啟 EPS32
      countdown--;
      if (countdown == 0) {
        Serial.println("Failed to reconnect");
        ESP.restart();
      }
    }
  }
}

void mqtt_publish(const char* topic, String str) {
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  // 宣告字元陣列
  byte arrSize = str.length() + 1;
  char msg[arrSize];
  Serial.print("Publish topic: ");
  Serial.print(topic);
  Serial.print(" message: ");
  Serial.print(str);
  Serial.print(" arrSize: ");
  Serial.println(arrSize);
  str.toCharArray(msg, arrSize); // 把String字串轉換成字元陣列格式
  if (!mqttClient.publish(topic, msg)) {
    Serial.println("Faliure to publish, maybe you should check the message size: MQTT_MAX_PACKET_SIZE 128");   // 發布MQTT主題與訊息
  }
}

void setup_motor() {
  // sets the pins as outputs:
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 3; j++) {
      pinMode(MotorPins[i][j], OUTPUT);
    }
    // configure LED PWM functionalitites
    int pwmChannel = i;
    ledcSetup(pwmChannel, freq, resolution);
    // attach the channel to the GPIO to be controlled
    ledcAttachPin(MotorPins[i][2], pwmChannel);
    //Stop all
    stop_motor(i);
  }
}

void stop_motor(int pumpNumber) {
  // Stop the DC motor
  Serial.print("[");
  Serial.print(pumpNumber);
  Serial.println("]Motor stopped");
  digitalWrite(MotorPins[pumpNumber][0], LOW);
  digitalWrite(MotorPins[pumpNumber][1], LOW);
}

bool move_pump(int pumpNumber, int orientation, int dutyCycle, int duration, char* error) {
  if (pumpNumber > 3 || pumpNumber < 0) {
    sprintf(error, "Error pumpNumber %d", pumpNumber);
    Serial.println(error);
    return false;
  }

  if (orientation != 1 && orientation != -1) {
    sprintf(error, "Error orientation %d", orientation);
    Serial.println(error);
    return false;
  }

  if (dutyCycle > 255 || dutyCycle < 200) {
    sprintf(error, "Error dutyCycle %d", dutyCycle);
    Serial.println(error);
    return false;
  }

  if (block_pump[pumpNumber]) {
    sprintf(error, "pump %d is already blocked", pumpNumber);
    Serial.println(error);
    return false;
  } else {
    block_pump[pumpNumber] = true;
  }


  ledcWrite(pumpNumber, dutyCycle);
  stop_motor(pumpNumber);
  Serial.printf("move_pump pumpNumber:%d, orientation:%d, dutyCycle:%d, durtion %d\r\n", pumpNumber, orientation, dutyCycle, duration);
  Serial.println(error);
  if (orientation == 1) {
    digitalWrite(MotorPins[pumpNumber][0], HIGH);
    digitalWrite(MotorPins[pumpNumber][1], LOW);
  } else {
    digitalWrite(MotorPins[pumpNumber][0], LOW);
    digitalWrite(MotorPins[pumpNumber][1], HIGH);
  }
  delay(duration);
  stop_motor(pumpNumber);
  block_pump[pumpNumber] = false;
  return true;
}

void setup_topic() {
  sprintf(&EventTopic[0], "status/%s", DeviceId); //"status/dosing_pump_x";
  sprintf(&CommandTopic[0], "cmd/%s", DeviceId); //"cmd/dosing_pump_x";
  sprintf(&CommandRespTopic[0], "cmd_resp/%s", DeviceId); //"cmd_resp/dosing_pump_x";
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // handle message arrived
  Serial.printf("Message arrived [%s] len:%d\r\n", topic, length);

  if (strcmp(topic, CommandTopic) != 0 ) {
    Serial.println("Error Command topic!!");
    return;
  }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, payload, length);
  if (doc.isNull()) {
    Serial.println("failed to load json commands");
    return;
  }
  serializeJsonPretty(doc, Serial);
  Serial.println("");

  // Payload like
  /* {
       "command_id": int,
       "command_type": int //1:move pump, 2:stop all pump
       "pumpNumber": int ,
       "orientation": int,
       "dutyCycle": int,
       "duration": int
     }
  */

  JsonVariant a0 = doc["command_id"];
  JsonVariant a01 = doc["command_type"];
  JsonVariant a1 = doc["pumpNumber"];
  JsonVariant a2 = doc["orientation"];
  JsonVariant a3 = doc["dutyCycle"];
  JsonVariant a4 = doc["duration"];

  if (a0.isNull() || a01.isNull()) {
    Serial.println("error command arguments");
    return;
  }

  int command_id, command_type;
  command_id = a0.as<int>();
  command_type = a01.as<int>();

  if (command_type == 1) { //1:move pump
    if (a1.isNull() || a2.isNull() || a3.isNull() || a4.isNull()) {
      Serial.println("error move pump command arguments");
      return;
    }
    int pumpNumber, orientation, dutyCycle, duration;
    pumpNumber = a1.as<int>();
    orientation = a2.as<int>();
    dutyCycle = a3.as<int>();
    duration = a4.as<int>();
    char error[64];
    if (move_pump(pumpNumber, orientation, dutyCycle, duration, &error[0])) {
      doc["result"]   = true;
    } else {
      doc["result"]  = false;
      doc["error"]   = error;
    }
    String str;
    serializeJsonPretty(doc, str);
    mqtt_publish(CommandRespTopic, str);
  } else if (command_type == 2) { // 2:stop all pump, currently this is not work, because delay inside move_pump
    doc["result"]  = true;
    String str;
    serializeJsonPretty(doc, str);
    mqtt_publish(CommandRespTopic, str);
    Serial.println("ESP.restart");
    ESP.restart();
  } else {
    Serial.println("error command type");
    return;
  }
  return;
}


void setup_spiffs() {
  //clean FS, for testing
  // SPIFFS.format();

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin(true)) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument doc(1024);

        deserializeJson(doc, buf.get(), DeserializationOption::NestingLimit(20));
        serializeJsonPretty(doc, Serial);

        if (!doc.isNull()) {
          Serial.println("\nparsed json");

          if (doc.containsKey("mqttServer")) {
            strcpy(mqttServer, doc["mqttServer"]);
          }
          if (doc.containsKey("mqttPort")) {
            strcpy(mqttPort, doc["mqttPort"]);
          }
          if (doc.containsKey("mqttUserName")) {
            strcpy(mqttUserName, doc["mqttUserName"]);
          }
          if (doc.containsKey("mqttPwd")) {
            strcpy(mqttPwd, doc["mqttPwd"]);
          }
          if (doc.containsKey("DeviceId")) {
            strcpy(DeviceId, doc["DeviceId"]);
          }

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read
}

void setup_wifi_reset(){
  pinMode(LEDPin, OUTPUT);
  pinMode(buttonPin, INPUT);
}

void wifi_reset_lisenser(){
   buttonState = digitalRead(buttonPin);
   // check if the pushbutton is pressed. If it is, the buttonState is HIGH:
   if (buttonState == LOW) {
      for(int i=0;i<5;i++){
        digitalWrite(LEDPin, HIGH);   // turn the LED on (HIGH is the voltage level)
        delay(300);                       // wait for a second
        digitalWrite(LEDPin, LOW);    // turn the LED off by making the voltage LOW
        delay(300);                       // wait for a second  
      }
      Serial.println("To reset wifi");
      WiFiManager wm;
      wm.resetSettings();
      ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  setup_spiffs(); // Read parameter from FS, if no data, use default
  setup_wifi(); // If running on AP mode, get the paramters from config portal
  setup_topic(); // Configure Topic with Device ID
  mqttClient.setServer(mqttServer, atoi(mqttPort));
  setup_motor();
  setup_wifi_reset();
}

void loop() {
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  wifi_reset_lisenser();
}
