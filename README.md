# esp32_socketcand_adapter

Program that lets the [Olimex ESP32-EVB](https://www.olimex.com/Products/IoT/ESP32/ESP32-EVB/open-source-hardware)
act as a [socketcand](https://github.com/linux-can/socketcand/) adapter.
It runs a server that translates traffic between TCP socketcand clients and its CAN bus.

Clients can connect to the adapter via TCP socketcand
to communicate with the CAN bus connected to the adapter.

```
Client1 <---TCP Socketcand---> |---------------------------|
                               | esp32_socketcand_adapter  |
Client2 <---TCP Socketcand---> |        running on         |
                               |     Olimex ESP32-EVB      |
Physical CAN bus (can0) <----> |---------------------------|
```

## Installation
1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

2. Download this repository.

3. Build and flash your
[Olimex ESP32-EVB](https://www.olimex.com/Products/IoT/ESP32/ESP32-EVB/open-source-hardware)
with this program using
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

4. Your ESP32 will print its curent network settings over USB UART.
Hold button `BUT1` for 1 second to reset the settings to default.
The default settings are:
    - Ethernet uses statically assigned IP:
        - Address: 192.168.2.163
        - Netmask: 255.255.255.0
        - Gateway: 192.168.2.1
    - Wifi is disabled.
    - CAN bitrate is 500 kbits.

5. Use an ethernet cable to connect the ESP32 to your computer or a local network.

6. In a web browser, go to `http://192.168.2.163/`,
or whatever the ESP32's ethernet IP is.
You should see a web interface served by the ESP32 over HTTP.
Change network settings to your liking.

7. Connect the ESP32 to a CAN bus.

8. Use a tool such as [socketcand](https://github.com/linux-can/socketcand)
to connect to your ESP32's IP address.
The ESP32 serves socketcand over port 9999.

9. You should be able to send and receive messages to the CAN bus
connected to the ESP32.

If the CAN bus has [OpenCyphal](https://opencyphal.org/) nodes on it,
you can interact with them using
[yakut](https://github.com/OpenCyphal/yakut).
Here are some examples:

```bash
# Monitor the CAN bus using:
yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" monitor

# Get the info of node 18 on the CAN bus using:
yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.node.GetInfo.1.0 '{}'

# -- Other example commands below --
# (these haven't been tested)

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'VREC'}}"

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'NAME'},'value':{'unstructured':{'value':'cantest'}}}"

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'NAME'}}"

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'BRPC'},'value':{'natural32':{'value':1374}}}"

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'ACCS'},'value':{'natural32':{'value':15}}}" # Access level to 15

yakut --transport "CAN(can.media.socketcand.SocketcandMedia('can0','192.168.2.163',9999),99)" call 18 uavcan.register.Access.1.0 "{'name':{'name':'AVR'},'value':{'natural32':{'value':5}}}" # ADCViewRate to 5ms
```



