/*
 * Project: Mail Notifier - WiFi Edition
 * Author: Zak Kemble, contact@zakkemble.net
 * Copyright: (C) 2022 by Zak Kemble
 * License: 
 * Web: https://blog.zakkemble.net/mail-notifier-wifi-edition/
 */

#include <ESP8266WiFi.h>
#include <include/WiFiState.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <Ticker.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// Third party libraries:
// ArduinoJson https://arduinojson.org/

// IDE Settings:
// Board: Generic ESP8285 Module (or Generic ESP8266 Module)
// Builtin LED: 2
// CPU Freq: 80MHz
// Xtal freq: 26MHz
// Flash size: 1MB
// Flash mode: DOUT (only for ESP8266)
// Flash freq: 40MHz (only for ESP8266)

// Other links:
// https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/generic-class.html

// To enter update mode:
// Make sure ESP is off
// Connect IO3 (UART RX) to GND
// Trigger and hold the mailbox flap input (must be held for entire update process)
// LED was flash rapidly, now remove IO3 connection
// (Short RST to GND to exit update mode without updating firmware)
// Wait for WiFi connect then connect to ESP web server updater and upload new firmware
// Wait for update to complete and stop holding the mailbox flap input

#define FW_VERSION		"1.0.0 211109"
#define FW_BUILD		__DATE__ " " __TIME__

#define BAUD			115200

#define HTTP_HOST		"example.com"
#define HTTP_PORT		80
#define HTTP_URI		"/mailnotifier.php"

#define HTTP_API_KEY	"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

#define WEBSERVER_PORT	80

#define STA_SSID		F("MyWiFi123")
#define STA_PASS		F("password69")

#define BATT_R1			47000
#define BATT_R2			10000
#define ADC_OFFSET		-35

#define PIN_LED			LED_BUILTIN
#define	PIN_PWRCTRL		15
#define	PIN_UPDATEMODE	3

#define SHT3X_ADDR		0x44

#define STATE_IDLE				0
#define STATE_GETTEMPHUMIDITY	1
#define STATE_HTTPREQUEST		2
#define STATE_WIFIDISCONNECTING	3
#define STATE_POWEROFF			4

typedef uint32_t millis_t;

typedef struct {
	uint32_t success;
	uint32_t wifiFail; // WiFi connect failed
	uint32_t netFail; // Web server connect failed
} counts_t;

static WiFiEventHandler staConnectedHandler;
static WiFiEventHandler staDisconnectedHandler;
static WiFiEventHandler staGotIPHandler;
static WiFiEventHandler staDHCPTimeoutHandler;
static ESP8266WebServer server(WEBSERVER_PORT);
static ESP8266HTTPUpdateServer httpUpdater; // https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266HTTPUpdateServer/examples/WebUpdater/WebUpdater.ino

static counts_t counts;
static uint16_t lastFailReason;
static float temperature;
static float humidity;
static uint32_t battery; // millivolts
static bool isUpdateMode;
static WiFiState shutdownState;

static String macToString(const unsigned char* mac)
{
	char buf[20];
	snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return String(buf);
}

static void ledFlash()
{
	static Ticker ledFlash;

	digitalWrite(PIN_LED, LOW);
	ledFlash.once_ms_scheduled(10, []() {
		digitalWrite(PIN_LED, HIGH);
	});
}

static String makeJSON(String notification)
{
	StaticJsonDocument<1024> doc;

	doc["key"]					= HTTP_API_KEY;
	doc["millis"]				= millis();
	doc["notify"]				= notification;
	doc["battery"]				= battery;
	doc["lastfail"]				= lastFailReason;

	doc["fw"]["version"]		= FW_VERSION;
	doc["fw"]["build"]			= FW_BUILD;

	doc["counts"]["success"]	= counts.success;
	doc["counts"]["wififail"]	= counts.wifiFail;
	doc["counts"]["netfail"]	= counts.netFail;
	
	doc["net"]["ip"]		= WiFi.localIP().toString();
	doc["net"]["subnet"]	= WiFi.subnetMask().toString();
	doc["net"]["gateway"]	= WiFi.gatewayIP().toString();
	doc["net"]["dns1"]		= WiFi.dnsIP(0).toString();
	doc["net"]["dns2"]		= WiFi.dnsIP(1).toString();
	doc["net"]["hostname"]	= WiFi.hostname();
	
	doc["wifi"]["ssid"]		= WiFi.SSID();
	doc["wifi"]["bssid"]	= WiFi.BSSIDstr();
	doc["wifi"]["channel"]	= WiFi.channel();
	doc["wifi"]["mac"]		= WiFi.macAddress();
	doc["wifi"]["rssi"]		= WiFi.RSSI();
	
	doc["env"]["t"]			= temperature;
	doc["env"]["h"]			= humidity;

	String msg;
	serializeJson(doc, msg);

	return msg;
}

static void webHandleNotFound()
{
	String message = F("404 File Not Found\n\n");
	message += F("URI: ");
	message += server.uri();
	message += F("\nMethod: ");
	message += (server.method() == HTTP_GET) ? F("GET") : F("POST");
	message += F("\nArguments: ");
	message += server.args();
	message += F("\n");
	for(uint8_t i=0;i<server.args();i++)
		message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
	server.send(404, F("text/plain"), message);
	ledFlash();
}

static void onStaConnected(const WiFiEventStationModeConnected& evt)
{
	Serial.print(F("Connected: "));
	Serial.print(evt.ssid);
	Serial.print(F(" "));
	Serial.print(macToString(evt.bssid));
	Serial.print(F(" "));
	Serial.println(evt.channel);
	
	ledFlash();
}

static void onStaDisconnected(const WiFiEventStationModeDisconnected& evt)
{
	Serial.print(F("Disconnected: "));
	Serial.print(evt.ssid);
	Serial.print(F(" "));
	Serial.print(macToString(evt.bssid));
	Serial.print(F(" "));
	Serial.println(evt.reason);

	ledFlash();
}

static void onStaGotIP(const WiFiEventStationModeGotIP& evt)
{
	Serial.print(F("Got IP: "));
	Serial.print(evt.ip);
	Serial.print(F(" "));
	Serial.print(evt.mask);
	Serial.print(F(" "));
	Serial.println(evt.gw);

	ledFlash();
}

static void onStaDHCPTimeout()
{
	Serial.println(F("DHCP Timeout"));
	ledFlash();
}

static void sht3x_startConvertion()
{
	Wire.beginTransmission(SHT3X_ADDR);
	Wire.write(0x24);
	Wire.write(0x00);
	uint8_t res = Wire.endTransmission();
	if(res != 0)
	{
		Serial.print(F("SHT3X I2C Error: "));
		Serial.println(res);
	}
}

static bool sht3x_getData()
{
	if(Wire.requestFrom(SHT3X_ADDR, 6) == 0)
	{
		//Serial.println(F("BUSY"));
		return false;
	}

	uint8_t data[3];

	// Temperature is a linear scale of 0x0000 (-45C) to 0xFFFF (+130C)
	Wire.readBytes(data, (uint8_t)3);
	// checksum in data[2]
	data[2] = data[0];
	data[0] = data[1];
	data[1] = data[2];
	temperature = (175 * (*(uint16_t*)data / (float)0xFFFF)) - 45;

	// Humidity is a linear scale of 0x0000 (0%) to 0xFFFF (100%)
	Wire.readBytes(data, (uint8_t)3);
	// checksum in data[2]
	data[2] = data[0];
	data[0] = data[1];
	data[1] = data[2];
	humidity = 100 * (*(uint16_t*)data / (float)0xFFFF);
	
	return true;
}

static uint32_t eepromRead32(uint16_t addr)
{
	uint32_t value = EEPROM.read(addr++);
	value |= ((uint32_t)EEPROM.read(addr++))<<8;
	value |= ((uint32_t)EEPROM.read(addr++))<<16;
	value |= ((uint32_t)EEPROM.read(addr))<<24;
	return value;
}

static void eepromWrite32(uint16_t addr, uint32_t value)
{
	EEPROM.write(addr++, value);
	EEPROM.write(addr++, value>>8);
	EEPROM.write(addr++, value>>16);
	EEPROM.write(addr, value>>24);
}

static void saveCounts()
{
	eepromWrite32(0, counts.success);
	eepromWrite32(4, counts.wifiFail);
	eepromWrite32(8, counts.netFail);
	EEPROM.write(12, lastFailReason);
	EEPROM.write(13, lastFailReason>>8);
	EEPROM.commit();
}

void setup()
{
	pinMode(PIN_UPDATEMODE, INPUT_PULLUP);
	pinMode(PIN_PWRCTRL, OUTPUT);
	pinMode(PIN_LED, OUTPUT);
	digitalWrite(PIN_PWRCTRL, HIGH);
	digitalWrite(PIN_LED, HIGH);

	isUpdateMode = (digitalRead(PIN_UPDATEMODE) == LOW);

	Serial.begin(BAUD);

	Serial.println();
	Serial.println(F("START"));

	Wire.begin();

	WiFi.mode(WIFI_STA);

	// WiFi Events
	staConnectedHandler = WiFi.onStationModeConnected(&onStaConnected);
	staDisconnectedHandler = WiFi.onStationModeDisconnected(&onStaDisconnected);
	staGotIPHandler = WiFi.onStationModeGotIP(&onStaGotIP);
	staDHCPTimeoutHandler = WiFi.onStationModeDHCPTimeout(&onStaDHCPTimeout);
	
	if(!isUpdateMode)
	{
		Serial.println(F("NORMAL MODE"));

		sht3x_startConvertion();

		EEPROM.begin(14);
		counts.success = eepromRead32(0);
		counts.wifiFail = eepromRead32(4);
		counts.netFail = eepromRead32(8);
		lastFailReason = EEPROM.read(12) | ((uint16_t)EEPROM.read(13))<<8;
		
		if(counts.success == 0xFFFFFFFF)
			counts.success = 0;
		if(counts.wifiFail == 0xFFFFFFFF)
			counts.wifiFail = 0;
		if(counts.netFail == 0xFFFFFFFF)
			counts.netFail = 0;
		if(lastFailReason == 0xFFFF)
			lastFailReason = 0;
	}
	else
	{
		Serial.println(F("UPDATE MODE"));
		
		// Serial output must be disabled otherwise RX pin will always read as LOW
		Serial.flush();
		Serial.end();
		pinMode(PIN_UPDATEMODE, INPUT_PULLUP);

		// Flash LED
		for(uint8_t i=0;i<6;i++)
		{
			digitalWrite(PIN_LED, LOW);
			delay(10);
			digitalWrite(PIN_LED, HIGH);
			
			if(digitalRead(PIN_UPDATEMODE) == LOW)
				i = 0;

			delay(40);
		}

		Serial.begin(BAUD);

		WiFi.begin(STA_SSID, STA_PASS);

		// HTTP Server
		server.onNotFound(webHandleNotFound);
		server.on(F("/"), [](){
			server.send(200, F("text/html"), F("<html><head><title>Mail Notifier</title></head><body>Hi<br /><a href=\"/update\">UPDATE</a></body></html>"));
			ledFlash();
		});
		server.on(F("/favicon.ico"), []() {
			server.send(404, F("text/plain"), F("404"));
			//server.send(200, (PGM_P)F("image/x-icon"), (PGM_P)FPSTR(favicon), sizeof(favicon));
			//ledFlash();
		});
		httpUpdater.setup(&server);
		server.begin();
		
		Serial.println(F("READY"));
	}
}

void loop()
{
	// This delay(1) is needed so that the module can go into low-power mode
	// No delay(), delay(0) and yield() don't work and they also give weird ping times
	delay(1);

	if(isUpdateMode)
	{
		server.handleClient();
		return;
	}
	
	static millis_t timer;
	static bool stuck;
	static bool success;
	static uint8_t state;
	static uint8_t lastState = 99;

	if(state != lastState)
	{
		lastState = state;
		Serial.print(F("STATE: "));
		Serial.println(state);
	}

	switch(state)
	{
		case STATE_IDLE:
		{
			//uint8_t mac[6] = {0xd0, 0x57, 0x94, 0xcf, 0x89, 0x28};
			WiFi.begin(STA_SSID, STA_PASS);//, 1, mac);
			Serial.println(F("Waiting for WiFi connect..."));
			int8_t res = WiFi.waitForConnectResult(10000);
			if(res != WL_CONNECTED)
			{
				// TODO test DHCP timeout?
				// https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266WiFi/src/ESP8266WiFiSTA.cpp#L428
				// https://github.com/esp8266/Arduino/issues/7005

				WiFi.shutdown(shutdownState);

				// https://github.com/esp8266/Arduino/blob/da6ec83b5fdbd5b02f04cf143dcf8e158a8cfd36/cores/esp8266/wl_definitions.h#L50
				// 9 = Timeout
				// 10 = WL_IDLE_STATUS
				// ...
				// 17 = WL_DISCONNECTED
				lastFailReason = res + 10;

				counts.wifiFail++;
				saveCounts();
				state = STATE_WIFIDISCONNECTING;
				timer = millis();
				
				Serial.print(F("Unable to connect to WiFi: "));
				Serial.println(res);
				
				break;
			}
			else if(stuck)
			{
				state = STATE_HTTPREQUEST;
				break;
			}
			else
				state = STATE_GETTEMPHUMIDITY;
		}
			[[fallthrough]];
		case STATE_GETTEMPHUMIDITY:
			// It takes around 15ms for a conversion, but by the time we get here it's probably been a lot longer than that anyway
			if((millis() - timer) >= 20)
			{
				Serial.println(F("Getting temp & humidity..."));
				
				static uint8_t tries;
				tries++;

				if(sht3x_getData() || tries > 25)
					state = STATE_HTTPREQUEST;
				else
					timer = millis();
			}
			break;
		case STATE_HTTPREQUEST:
		{
			uint32_t adc = analogRead(0) + ADC_OFFSET;
			battery = ((uint32_t)((1 / (BATT_R2 / (float)(BATT_R2 + BATT_R1))) * 10) * adc) / 10;

			Serial.print(": T=");
			Serial.print(temperature);
			Serial.print("C, RH=");
			Serial.print(humidity);
			Serial.println("%");
			Serial.print("ADC=");
			Serial.print(adc);
			Serial.print(" BATT=");
			Serial.print(battery);
			Serial.println("mV");
			
			String jsonStr = makeJSON(stuck ? "stuck" : "mail");
			Serial.println(jsonStr);
			Serial.println(F("////////"));
			
			// https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266HTTPClient/examples/BasicHttpClient/BasicHttpClient.ino
			// https://arduinojson.org/v6/how-to/use-arduinojson-with-httpclient/

			// Send request
			WiFiClient client;
			HTTPClient http;
			http.begin(client, HTTP_HOST, HTTP_PORT, HTTP_URI, false);
			http.setTimeout(5000);
			http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
			http.addHeader("Content-Type", "application/json");

			// NOTE: This is blocking
			int httpCode = http.POST(jsonStr);

			Serial.print(F("CODE: "));
			Serial.println(httpCode);

			// Read response
			String res = http.getString();
			Serial.println(res);
			
			if(httpCode == HTTP_CODE_OK)
			{
				// Super simple response parsing
				if(res.indexOf("{\"result\":\"ok\"}") != -1)
				{
					success = true;
					lastFailReason = 0;
					counts.success++;
				}
				else
				{
					lastFailReason = 700;
					counts.netFail++;
				}
			}
			else
			{
				// https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266HTTPClient/src/ESP8266HTTPClient.h#L48
				// 89 = HTTPC_ERROR_READ_TIMEOUT
				// ...
				// 99 = HTTPC_ERROR_CONNECTION_FAILED
				// 200 = HTTP_CODE_CONTINUE
				// ..
				// 611 = HTTP_CODE_NETWORK_AUTHENTICATION_REQUIRED
				lastFailReason = httpCode + 100;
				counts.netFail++;
			}

			http.end();
			
			Serial.println();
			Serial.println(F("Disconnecting..."));

			WiFi.shutdown(shutdownState);
			
			saveCounts();

			state = STATE_WIFIDISCONNECTING;
			timer = millis();
		}
			[[fallthrough]];
		case STATE_WIFIDISCONNECTING:
			if(WiFi.status() == WL_DISCONNECTED || (millis() - timer) >= 3000)
			{
				digitalWrite(PIN_PWRCTRL, LOW);
				state = STATE_POWEROFF;
				timer = millis();
				Serial.println(F("Powering off..."));
			}
			break;
		case STATE_POWEROFF:
			if(!stuck && success) 
			{
				if((millis() - timer) >= 10000)
				{
					// Only send a "stuck" notification if the previous "mail" notification worked

					digitalWrite(PIN_PWRCTRL, HIGH);
					stuck = 1;
					state = STATE_IDLE;
					Serial.println(F("Sending stuck notification..."));
				}
			}
			else if((millis() - timer) >= 3000)
			{
				Serial.println(F("Stuck, going to sleep..."));
				Serial.flush();
				ESP.deepSleep(0);
			}
			break;
		default:
			break;
	}
}
