#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ACAN_ESP32.h>
#include <ETH.h>
#include <Preferences.h>
#include <WiFi.h>
#include <socketcand_translate.h>

#define PREF_IP_ADDR 0
#define PREF_GATEWAY_ADDR 1
#define PREF_SUBNET_ADDR 2

// For WiFi
#define SSID "ssid"
#define PASSWORD "password"

#define BUTTON 34
// OPTIONAL: attach a LED to pPIO 21
#define LED 21


// Struct to store IP addr info
typedef struct {
  uint8_t val1;
  uint8_t val2;
  uint8_t val3;
  uint8_t val4;
} ipaddr_t;

static bool eth_connected = false;

static const uint32_t DESIRED_BIT_RATE = 500UL * 1000UL;

struct socketcand_translate_frame can_frame;

uint16_t port = 9999;
WiFiServer server(port);
WiFiClient client;

// ESP32-EVB preferences store config settings (like EEPROM)
Preferences prefs;
ipaddr_t *asocketcand;
uint8_t ip_buf[255];
bool wifi;
const char* ssid;
const char* pass;

void setup() {
  Serial.begin(115200);
  while (!Serial);
    delay (1000);

  pinMode(BUTTON, INPUT); // Button used to reset network config settings
  pinMode(LED, OUTPUT);
  
  #if defined ARDUINO_ESP32_EVB //Needed for olimex esp32-evb to connect to ethernet
  delay(400);
  #endif
  WiFi.onEvent(WiFiEvent); // Ethernet event handler

  get_network_prefs(); // Sets ETH/WiFi settings
  init_server(); // Setup ETH/WiFi and start server

  init_can(); // Sets up CAN on microcontroller 

  Serial.println("Waiting for client connection...");
}


void loop() {
  client = server.available();
  digitalWrite(LED, LOW);
  // Checks button press and hold for resetting network config settings
  if(!digitalRead (BUTTON)) {
    delay(1000); // Delay needed to check press and hold
    if(!digitalRead (BUTTON)) {
      reset_network_prefs();
      if(wifi) {
        init_wifi();
      } else {
        config_eth(); 
      } 
      blink(3);
    }
  }

  if(client) {
    int r = 0;
    Serial.println("new client");

    while(client.connected()) {
    digitalWrite(LED, HIGH);
      if(!digitalRead (BUTTON)) { // Same button press check needed in secondary loop
        delay(1000);
        if(!digitalRead (BUTTON)) {
          reset_network_prefs();
          if(wifi) {
        init_wifi();
      } else {
        config_eth(); 
      } 
          blink(3);
          break;
        }
      }

      if (socketcand_translate_is_open() == SOCKETCAND_TRANSLATE_CLOSED) { // Open raw socket if closed
        open_raw();
      }

      char buffer[255];
      char recvbuf[255];
      CANMessage message;
      
      if (client.available() ) { // Read from TCP socket and send to CAN 
        char c = client.read();
        if(c == '<') {
          r = 0;
        }
        r += sprintf(recvbuf + r,"%c", c);
        
        if(c == '>') {
          Serial.println(recvbuf);
          can_frame = socketcand_translate_string_to_frame(recvbuf, can_frame);
          print_frame(can_frame.id, can_frame.len, can_frame.data);
          message.id = can_frame.id; 
          message.len = can_frame.len; 
          message.ext = can_frame.ext;
          for(int i = 0; i < can_frame.len; i++) {
            message.data[i] = can_frame.data[i];
          }  

          print_frame(message.id, message.len, message.data);
          Serial.print("Send");
          ACAN_ESP32::can.tryToSend(message); 
        }
      }

      if (ACAN_ESP32::can.receive(message)) { // Read from CAN and send to TCP socket

        long secs = millis()/1000;
        long usecs = micros()%1000000;
        print_frame(message.id, message.len, message.data);

        socketcand_translate_frame_to_string(buffer, sizeof(buffer), message.id, secs, usecs, message.data, message.len, message.ext);
        client.write(buffer);

      }
      
    }
    client.stop();
    Serial.println("Client Disconnected.");
    socketcand_translate_set_state(SOCKETCAND_TRANSLATE_CLOSED);
  }
}


// Prints frame to serial moniter, used for development and debugging
void print_frame(uint32_t id, uint8_t len, uint8_t data[8]) {
  Serial.printf("%08X", id);
  Serial.print("   [");
  Serial.print(len);
  Serial.print("]  ");
  for(int i = 0; i < len; i++) {
    Serial.printf("%02X ", data[i]);
  }  
  Serial.println("");
}


// Sets up CAN on microcontroller and sends a test frame
void init_can() {
  ACAN_ESP32_Settings settings (DESIRED_BIT_RATE);
  settings.mRxPin = GPIO_NUM_35 ; // ESP32-EVB specific CAN rx and tx pins
  settings.mTxPin = GPIO_NUM_5 ;

  const uint32_t errorCode = ACAN_ESP32::can.begin (settings) ;

  if (errorCode != 0) {
    // Print the error
    Serial.print("Error Can: 0x");
    Serial.println(errorCode, HEX);
  } else {
    Serial.println("CAN Interface started successfully");
  }

  CANMessage frame;
  frame.id = 0x100;  // Set CAN frame id
  frame.len = 0;  // Set frame length

  if (ACAN_ESP32::can.tryToSend(frame)) {
    Serial.println("Test CAN frame sent");
  }
}


// Mimics opening a raw socket for python-can to connect
void open_raw() {
  char buffer[255];
  int open_level = 0;

  while((socketcand_translate_is_open() == 0)) {
    int r = 0;

    while(open_level > 0) {

      if (client.available()) {
        char c = client.read();
        r += sprintf(buffer + r,"%c", c);
        if(c == '>') {
          break;
        }
      }
    }

    if ((socketcand_translate_is_open() == 0)) {
      buffer[strlen(buffer)] = '\0';
      open_level = socketcand_translate_open_raw(open_level, buffer, sizeof(buffer));
      client.write(buffer);
    }
  }
}


// Ethernet event handler
void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      //set eth hostname here
      ETH.setHostname("esp32-ethernet");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH MAC: ");
      Serial.print(ETH.macAddress());
      Serial.print(", IPv4: ");
      Serial.print(ETH.localIP());
      if (ETH.fullDuplex()) {
        Serial.print(", FULL_DUPLEX");
      }
      Serial.print(", ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;
    default:
      break;
  }
}


// Checks to see if WiFi is preferred, if not sets up Ethernet
void init_server() {
  if(wifi) {
    init_wifi();
    return;
  }

  ETH.begin();
  while(!eth_connected) {
    delay(1000);
  }
  config_eth();
  
  server.begin();
  server.setNoDelay(true);
  Serial.print("Server started at: ");
  Serial.print(ETH.localIP());
  Serial.print(":");
  Serial.println(port);
}


// Sets up WiFi
void init_wifi() {

  size_t len;
  prefs.begin("wifi");
  String strSsid = prefs.getString("ssid");
  String strPass = prefs.getString("pass");
  ssid = strSsid.c_str();
  pass = strPass.c_str();
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }

  Serial.println("Connected to WiFi");
  server.begin();
  server.setNoDelay(true);
  Serial.print("Server started at: ");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(port);
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
}


// Sets the ethernet settings based on stored preferences
void config_eth() { 
  IPAddress ip(
    asocketcand[PREF_IP_ADDR].val1, 
    asocketcand[PREF_IP_ADDR].val2, 
    asocketcand[PREF_IP_ADDR].val3, 
    asocketcand[PREF_IP_ADDR].val4);

  IPAddress gateway(
    asocketcand[PREF_GATEWAY_ADDR].val1, 
    asocketcand[PREF_GATEWAY_ADDR].val2, 
    asocketcand[PREF_GATEWAY_ADDR].val3, 
    asocketcand[PREF_GATEWAY_ADDR].val4);

  IPAddress subnet(
    asocketcand[PREF_SUBNET_ADDR].val1, 
    asocketcand[PREF_SUBNET_ADDR].val2, 
    asocketcand[PREF_SUBNET_ADDR].val3, 
    asocketcand[PREF_SUBNET_ADDR].val4);

  ETH.config(ip,gateway,subnet);
}


// Resets network settings on microcontroller to default
void reset_network_prefs(){
  prefs.begin("asocketcand"); 
  prefs.clear();
  uint8_t content[] = {192, 168, 2, 163, 192, 168, 2, 1, 255, 255, 255, 0}; 
  prefs.putBytes("asocketcand", content, sizeof(content));
  prefs.end();

  prefs.begin("wifi", false);
  prefs.putString("ssid", SSID);
  prefs.putString("pass", PASSWORD);
  prefs.putUInt("On", false); // By default microcontroller uses ethernet
  prefs.end();
  get_network_prefs();
}


// Used to get the network preferences stored on the microcontroller
void get_network_prefs() {
  prefs.begin("asocketcand");
  size_t schLen = prefs.getBytesLength("asocketcand");
  prefs.getBytes("asocketcand", ip_buf, schLen);
  if (schLen % sizeof(ipaddr_t)) { // simple check that data fits
    log_e("Data is not correct size!");
    return;
  }
  asocketcand = (ipaddr_t *) ip_buf; // cast the bytes into a struct ptr
  prefs.end();

  prefs.begin("wifi");
  wifi = prefs.getUInt("On");
  prefs.end();
  return;
}

void blink(int num) {
  for(int i = 0; i < num; i++) {
    digitalWrite(LED, HIGH);
    delay(300);
    digitalWrite(LED, LOW);
    delay(300);
  }
}
