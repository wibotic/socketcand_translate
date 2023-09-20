# ESPSocketcand.

## Introduction
This is a software program designed for the Olimex ESP32-EVB microcontroller. It acts as a bridge between a Network socket and the Can bus of the microcontroller mimicking Linux Can/Socketcand raw mode.

## Requirements
This software was designed for the Olimex ESP32-EVB board. To flash it onto your microcontroller, you will need to use the Arduino IDE and set it up to support ESP32. Directions can be found here: https://github.com/espressif/arduino-esp32. You will also need to install the following Arduino libraries:
- ACAN_ESP32, https://github.com/pierremolinaro/acan-esp32
- socketcand_translate

You can install these libraries via Arduino library manager or download from Github.

_Note: It's developed in Arduino environment for ESP32, based on standard Arduino WiFi libraries and modified ACAN_ESP32 library which is found in the source folder itself._

## Configuration
Network settings (both Ethernet and WiFi) can be stored and retrieved from the ESP32's in-built preference features (equivalent to EEPROM).

### Basic Config
- Pressing and holding the button on pin 34 for 1 second resets the network settings.
- If WiFi is preferred, assign `wifi` preference true. By default it's set to Ethernet mode.
- LED on pin 21 flashes indicating various statuses - like network reset, client connection, etc.

### Network Preferences

Default Network settings are:
```c
IPAddress ip(192, 168, 2, 163);
IPAddress gateway(192, 168, 2, 1);
IPAddress subnet(255, 255, 255, 0);
ssid = "ssid";
password = "password";
```

These can be changed by using the ESP32-EVB Preferences library to change the keys.
The key asocketcand contains the IP, gateway, and subnet address info. To access this you will need to use the Preferences library and run:
```c
Preferences.begin(“asocketcand”);
Uint8_t net_settings[] = {ip1, ip2, ip3, ip4, gate1, gate2, gate3, gate4, sub1, sub2, sub3, sub4};
Preferences.putBytes(“asocketcand”, net_settings, sizeof(net_settings);
```

WiFi settings are under the Preference library of “wifi”, with three keys “ssid”, “pass” and a “On” (“On” used to determine whether ethernet or wifi is used). To change it to WiFi preferred, write this:
```c
Preferences.begin(“wifi”);
Preferences.putUInt(“On”, 1);
```

### How to connect to this Server:

- Your Olimex ESP32-EVB microcontroller will serve as a server hosting the network socket connected to CAN bus.
- Once the software is running, it will print the server IP and port in the Serial Monitor. Default port is 9999.
- Your client (PC, another ESP32, etc.) connects to this server IP and port over your local area network (Ethernet or WiFi). 
- A connection would establish, opens the session and the socketcand frames can be sent/received over this network.

## Initial Startup:
- Set up your Arduino IDE so it is compatible with the ESP32-EVB microcontroller.
- Install the ACAN_ESP32 library from the Arduino library manager or github.
- Upload espsocketcand.ino to the board, then press but1 to load config settings, then restart the board,
- Connect to the server at the default IP and port of 192.168.1.163:9999.

> __IMPORTANT__: This code makes this device (ESP32) act as a Server. You would need another device as a client (PC running python-can etc.) to connect and communicate with ESP32. 

## Usage examples:
- This was originally designed for use with OpenCyphal's Pycyphal Libary (https://github.com/OpenCyphal/pycyphal/tree/01b9a9b57143bc916bfa2fa26102a787b4e558e9). After installing and building the latest version of pycyphal from github, and the command line tool yakut from OpenCyphal, you can use the following commands with a WiBotic system connected:
    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.node.GetInfo.1.0 '{}'

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'VREC'}}"

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'NAME'},'value':{'unstructured':{'value':'cantest'}}}"

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'NAME'}}"

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'BRPC'},'value':{'natural32':{'value':1374}}}"

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'ACCS'},'value':{'natural32':{'value':15}}}" # Access level to 15

    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'AVR'},'value':{'natural32':{'value':5}}}" # ADCViewRate to 5ms

    # Commit changed params. COMMAND_STORE_PERSISTENT_STATES == 65530
    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.node.ExecuteCommand.1.0 "{'command':65530}"

    ## or monitor with
    yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" monitor

