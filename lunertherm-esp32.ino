// #include <StreamDebugger.h>
#include <Wire.h>
#include <M2M_LM75A.h>
#include <Adafruit_GFX.h>
#include <OakOLED.h>
#include "settings.h"

// SIM800C modem
#define MODEM_PWRKEY 4
#define MODEM_POWER_ON 25
#define MODEM_TX 27
#define MODEM_RX 26
#define MODEM_DTR 32
#define MODEM_RI 33

// LED
#define LED_GPIO 12
#define LED_ON LOW
#define LED_OFF HIGH
#define RELAY_1 14
#define RELAY_2 2


// I2C addresses
#define AXP192_SLAVE_ADDRESS (0x34U)
#define SSD1306_SLAVE_ADDRESS (0x3DU)
#define LM_SLAVE_ADDRESS (0x48U)


// Deep sleep timer
#define uS_TO_S_FACTOR 1000000
#define TIME_TO_SLEEP 30

// Serial interfaces
#define SerialMon Serial
#define SerialAT  Serial1
// StreamDebugger modem(SerialAT, SerialMon);
#define modem Serial1
M2M_LM75A lm75a;
OakOLED oled;

// GPRS APN
const char apn[] = "luner";

bool new_sms_available = false;
uint16_t target_temp_mc = 5000;
uint16_t current_temp_mc = 5000;
bool timer_enabled = false;
uint32_t timer_millis = 0;
bool thermostat_enabled = false;
uint16_t bandwidth = 500;
bool relay_1 = false;
bool relay_2 = false;
String log_message = "";

void power_cycle_modem()
{
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(100);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(6000);
}

bool negotiate(const char *command, uint16_t timeout) {
    modem.setTimeout(timeout);

    modem.print(command);
    if (!modem.find("OK\r\n")) {
        modem.print(command);
        if (!modem.find("OK\r\n")) {
            modem.print(command);
            if (!modem.find("OK\r\n")) {
                SerialMon.print("Unable to negotiate ");
                SerialMon.print(command);
                return false;
            }
        }
    }
    SerialMon.print("Successfully negotiated ");
    SerialMon.print(command);
    return true;
}

bool negotiate(const char *command) {
    return negotiate(command, 2500);
}

bool wait_for_ok() {
    modem.setTimeout(2500);
    return modem.find("OK\r\n");
}

bool wait_for_ok_long() {
    modem.setTimeout(26000);
    return modem.find("OK\r\n");
}

bool wait_for_sms_ready() {
    modem.setTimeout(15000);
    return modem.find("SMS Ready\r\n");
}

void IRAM_ATTR process_sms() {
    new_sms_available = true;
}

bool gprs_has_ip() {
    modem.print("AT+CIFSR\r\n");
    if (!modem.find("\r\n")) { return false; }
    if (!modem.find("\r\n")) { return false; }

    return true;
}

bool gprs_open() {
    modem.setTimeout(2500);

    modem.print("AT+CGATT?\r\n");
    if (!modem.find("+CGATT: ")) { return false; }
    uint8_t status = modem.parseInt();
    if (!modem.find("OK\r\n")) { return false; }
    if (status == 0) {
        if (!negotiate("AT+CGATT=1\r\n", 10000)) { return false; }
    }

    if (!negotiate("AT+CSTT=\"luner\"\r\n")) { return false; }

    if (!negotiate("AT+CIICR\r\n")) { return false; }

    if (!gprs_has_ip()) { return false; }

    return true;
}

bool gprs_send() {   
    if (!negotiate("AT+CIPSTART=\"UDP\",\"" UDP_IP "\"," UDP_PORT "\r\n")) { return false; }

    if (!modem.find("CONNECT OK\r\n")) { return false; }

    modem.print("AT+CIPSEND\r\n");
    if (!modem.find('>')) { return false; }

    uint32_t now = millis();

    modem.print(current_temp_mc);
    modem.print('/');
    modem.print(target_temp_mc);
    modem.print('/');
    modem.print(thermostat_enabled ? '1' : '0');
    modem.print('/');
    modem.print(timer_enabled ? '1' : '0');
    modem.print('/');
    modem.print(now);
    modem.print('/');
    modem.print(timer_millis);
    modem.print('/');
    modem.write(relay_1 ? '1' : '0');
    modem.print('/');
    modem.write(relay_2 ? '1' : '0');

    modem.write(0x1a);

    if (!modem.find("SEND OK\r\n")) { return false; }

    if (!negotiate("AT+CIPCLOSE\r\n")) { return false; }

    return true;
}

bool gprs_close() {
    if (!negotiate("AT+CIPSHUT\r\n")) { return false; }

    if (!negotiate("AT+CGATT=0\r\n")) { return false; }
    
    return true;
}

bool gprs_upload() {
    if (gprs_has_ip()) {
        SerialMon.println("GPRS has IP. Sending...");
        if (!gprs_send()) {
            SerialMon.println("Sending failed. Closing connection...");
            if (!gprs_close()) { return false; }
            SerialMon.println("Re-opening connection...");
            if (!gprs_open()) { return false; }
            SerialMon.println("Re-attempting send...");
            if (!gprs_send()) { return false; }
        }
        SerialMon.println("Send successful.");
        return true;
    } else {
        SerialMon.println("GPRS has no IP.");
        if (!gprs_open()) {
            SerialMon.println("GPRS open failed. Closing connection...");
            if (!gprs_close()) { return false; }
            SerialMon.println("Re-opening connection...");
            if (!gprs_open()) { return false; }
            SerialMon.println("Re-attempting send...");
            if (!gprs_send()) { return false; }
            return true;
        } else {
            SerialMon.println("GPRS open was successful. Sending...");
            return gprs_send();
        }
    }
}

bool initialize_modem() {
    // Power cycle modem
    SerialMon.println("Power-cycling modem...");
    power_cycle_modem();

    // Wait for 6 seconds
    SerialMon.println("Starting auto-bauding sequence...");

    // Baud sync
    while (true) {
        modem.print("AT\r\n");

        // Short timeout
        modem.setTimeout(500);

        if (modem.find("OK\r\n")) {
            modem.print("AT&F\r\n");
            break;
        }
    }

    // Wait for OK on factory reset
    if (!wait_for_ok()) { return false; }

    // Echo off
    if (!negotiate("ATE0\r\n")) { return false; }

    // Set baud rate
    if (!negotiate("AT+IPR=115200\r\n")) { return false; }

    // No flow control
    if (!negotiate("AT+IFC=0,0\r\n")) { return false; }

    // Wait for SMS ready
    wait_for_sms_ready();

    // Text mode
    if (!negotiate("AT+CMGF=1\r\n")) { return false; }

    // Slow clock
    if (!negotiate("AT+CSCLK=1\r\n")) { return false; }

    // Disable DTR special behavior
    if (!negotiate("AT&D0\r\n")) { return false; }

    // Save settings in NVRAM
    if (!negotiate("AT&W\r\n")) { return false; }

    // Enable GPRS session
    if (!gprs_open()) { return false; }

    attachInterrupt(MODEM_RI, process_sms, FALLING);
    
    SerialMon.println("Modem initialization complete");
    log_message = "Modem connected";
    update_oled();
}

void update_oled() {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.setTextSize(2);
    oled.println("ESZVBoreas");
    oled.setTextSize(1);
    oled.print("current temp: ");
    oled.println(((float)current_temp_mc) / 1000.0F);
    oled.print("target temp : ");
    oled.println(((float)target_temp_mc) / 1000.0F);
    oled.print("heating     : ");
    oled.println(relay_1 ? "yes" : "no");
    oled.print("extra relay : ");
    oled.println(relay_2 ? "yes" : "no");
    oled.println();
    oled.println(log_message);

    oled.display();

    delay(1000);
}

void setup() {
    // Pins
    pinMode(MODEM_PWRKEY, OUTPUT);
    pinMode(MODEM_POWER_ON, OUTPUT);
    pinMode(MODEM_DTR, OUTPUT);
    pinMode(MODEM_RI, INPUT);
    pinMode(LED_GPIO, OUTPUT);
    pinMode(RELAY_1, OUTPUT);
    pinMode(RELAY_2, OUTPUT);

    // Default GPIO values
    digitalWrite(LED_GPIO, LED_OFF);
    digitalWrite(MODEM_PWRKEY, HIGH);
    digitalWrite(MODEM_DTR, LOW);
    digitalWrite(MODEM_POWER_ON, HIGH);
    digitalWrite(RELAY_1, HIGH);
    digitalWrite(RELAY_2, HIGH);

    // Serial connections
    SerialMon.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    lm75a.begin();
    oled.begin();

    log_message = "Initializing modem...";
    update_oled();

    while (!initialize_modem());
}

void process_timer() {
    if (!timer_enabled) {
        return;
    }

    uint32_t now = millis();

    // Either:
    // - millis() is past timer_millis by at most 30 minutes
    // - millis() overflowed, and timer_millis is within last 30 minutes of uint32_t
    //     and millis() is within first 30 minutes of uint32_t
    if ((now > timer_millis && now - timer_millis < 1800000) || (now < timer_millis && now < 1800000 && timer_millis > 4294967295 - 1800000)) {
        timer_enabled = false;
        timer_millis = 0;
        thermostat_enabled = false;
    }
}

void handle_sms() {
    new_sms_available = false;

    log_message = "Processing SMS...";
    update_oled();

    SerialMon.println("Requesting messages...");

    // Request all messages
    modem.print("AT+CMGL=\"ALL\"\r\n");
    modem.setTimeout(1000);

    if (!modem.find("+CMGL:")) {
        SerialMon.println("No messages found on the modem");
        return;
    }

    bool do_upload = false;
    bool do_restart = false;

    while (modem.find("\"\r\n")) {
        String s = modem.readStringUntil('\r');
        SerialMon.print("Message content: ");
        SerialMon.println(s);

        if (s.startsWith("set_temperature ")) {
            uint16_t t = s.substring(16).toInt();
            if (t >= 0 && t <= 30000) {
                target_temp_mc = t;
                thermostat_enabled = true;
                SerialMon.print("New temperature set to ");
                SerialMon.println(t);
            }
            do_upload = true;
        } else if (s.startsWith("set_timer ")) {
            uint16_t t = s.substring(10).toInt();
            if (t == 0) {
                timer_enabled = false;
                timer_millis = 0;
            } else {
                timer_enabled = true;
                timer_millis = millis() + (t * 60000);
            }
            SerialMon.print("Timer set to ");
            SerialMon.println(t);
            do_upload = true;
        } else if (s == "get") {
            do_upload = true;
        } else if (s == "thermostat_on") {
            thermostat_enabled = true;
            do_upload = true;
        } else if (s == "thermostat_off") {
            thermostat_enabled = false;
            do_upload = true;
        } else if (s == "relay_on") {
            relay_2 = true;
            digitalWrite(RELAY_2, LOW);
            do_upload = true;
        } else if (s == "relay_off") {
            relay_2 = false;
            digitalWrite(RELAY_2, HIGH);
            do_upload = true;
        } else if (s == "restart") {
            do_restart = true;
        }
    }
    
    // Delete old messages
    SerialMon.println("Deleting messages...");
    modem.print("AT+CMGDA=\"DEL ALL\"\r\n");
    wait_for_ok_long();

    control_heating();

    if (do_upload) {
        gprs_upload();
    }

    if (do_restart) {
        SerialMon.println("Restarting ESP");
        ESP.restart();
    }

    log_message = "Modem connected";
    update_oled();
    SerialMon.println("SMS handled");
}

void control_heating() {
    if (!thermostat_enabled) {
        relay_1 = false;
        digitalWrite(RELAY_1, HIGH);
        return;
    }

    if (!relay_1 && current_temp_mc + bandwidth <= target_temp_mc) {
        // Getting too cold
        relay_1 = true;
        digitalWrite(RELAY_1, LOW);
    } else if (relay_1 && current_temp_mc - bandwidth >= target_temp_mc) {
        // Getting too hot
        relay_1 = false;
        digitalWrite(RELAY_1, HIGH);
    }
}

void loop() {
    current_temp_mc = lm75a.getTemperature() * 1000;

    if (new_sms_available) {
        handle_sms();
        // will also do updated_oled
    } else {
        process_timer();
        control_heating();
        update_oled();
    }

    delay(5000);
}
