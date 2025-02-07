#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "esp_system.h"

// Create a Preferences object to store configuration in flash.
Preferences preferences;

// Define the relay control pin.
const int relayPin = 23;
const int relayPinMinus = 22;

const char* endpointUrl = "https://develop.messengerapi.chytravrata.eu/receive";

// Global strings to hold stored configuration.
String savedSSID;
String savedPassword;
String savedAPIKey;


// Create two WebServer objects—one for normal operation and one for setup mode.
WebServer server(80);       // Normal operating mode server
WebServer setupServer(80);  // Setup portal server

// --- Swagger UI and JSON content (served from flash) ---
const char swaggerUI[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>Gate Control API Documentation</title>
    <link rel="stylesheet" type="text/css" href="https://unpkg.com/swagger-ui-dist/swagger-ui.css">
  </head>
  <body>
    <div id="swagger-ui"></div>
    <script src="https://unpkg.com/swagger-ui-dist/swagger-ui-bundle.js"></script>
    <script>
      const ui = SwaggerUIBundle({
        url: "/swagger.json",
        dom_id: '#swagger-ui'
      });
    </script>
  </body>
</html>
)rawliteral";

const char swaggerJSON[] PROGMEM = R"rawliteral({
  "swagger": "2.0",
  "info": {
    "title": "Gate Control API",
    "version": "1.0.0"
  },
  "paths": {
    "/open": {
      "get": {
        "summary": "Open Gate",
        "parameters": [{
            "name": "Authorization",
            "in": "header",
            "required": true,
            "type": "string",
            "description": "Bearer token"
        }],
        "responses": {
          "200": {"description": "Gate opened successfully"},
          "401": {"description": "Unauthorized"}
        }
      }
    },
    "/close": {
      "get": {
        "summary": "Close Gate",
        "parameters": [{
            "name": "Authorization",
            "in": "header",
            "required": true,
            "type": "string",
            "description": "Bearer token"
        }],
        "responses": {
          "200": {"description": "Gate closed successfully"},
          "401": {"description": "Unauthorized"}
        }
      }
    },
    "/docs": {
      "get": {
        "summary": "Swagger UI Documentation",
        "parameters": [{
            "name": "Authorization",
            "in": "header",
            "required": true,
            "type": "string",
            "description": "Bearer token"
        }],
        "responses": {
          "200": {"description": "Swagger UI page"},
          "401": {"description": "Unauthorized"}
        }
      }
    },
    "/swagger.json": {
      "get": {
        "summary": "Swagger API Spec",
        "parameters": [{
            "name": "Authorization",
            "in": "header",
            "required": true,
            "type": "string",
            "description": "Bearer token"
        }],
        "responses": {
          "200": {"description": "Swagger JSON spec"},
          "401": {"description": "Unauthorized"}
        }
      }
    }
  }
})rawliteral";

// --- Utility: Generate a random 16-character API key ---
String generateAPIKey() {
  String key = "";
  for (int i = 0; i < 16; i++) {
    int r = random(0, 16);
    if (r < 10) {
      key += String(r);
    } else {
      key += char('A' + (r - 10));
    }
  }
  return key;
}

// --- Normal Mode: Bearer token authentication helper ---
bool checkAuth() {
  if (!server.hasHeader("Authorization")) {
    return false;
  }
  String auth = server.header("Authorization");
  String expected = "Bearer " + savedAPIKey;

  if(auth == expected) {
    return true;
  }
  return false;
}

void handleUnauthorized() {
  server.sendHeader("WWW-Authenticate", "Bearer");
  server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
}

// --- Normal Mode: Endpoint Handlers ---
void handleOpen() {
  if (!checkAuth()) {
    handleUnauthorized();
    return;
  }
  openOrCloseGate();
  server.send(200, "application/json", "{\"status\": \"gate opened\"}");
}

void handleClose() {
  if (!checkAuth()) {
    handleUnauthorized();
    return;
  }
  openOrCloseGate();
  server.send(200, "application/json", "{\"status\": \"gate closed\"}");
}

void handleDocs() {
  server.send_P(200, "text/html", swaggerUI);
}

void handleSwaggerJSON() {
  server.send_P(200, "application/json", swaggerJSON);
}

void handleNotFound() {
  if (!checkAuth()) {
    handleUnauthorized();
    return;
  }
  server.send(404, "application/json", "{\"error\": \"Not found\"}");
}

// --- Setup Portal: Serve the configuration web page ---
void handleSetupRoot() {
  String html = "<html><head><title>ESP32 Setup</title></head><body>";
  html += "<h1>ESP32 WiFi Setup</h1>";
  html += "<form action='/save' method='POST'>";
  html += "WiFi SSID: <input type='text' name='ssid'><br>";
  html += "WiFi Password: <input type='text' name='password'><br>";
  html += "<input type='submit' value='Save'>";
  html += "</form>";
  html += "<br><hr><br>";
  html += "<h2>API Setup (JSON POST)</h2>";
  html += "<p>You can also POST your WiFi credentials to <code>/config</code> and get your API key in return.</p>";
  html += "<p>Example JSON:</p>";
  html += "<pre>{\"ssid\": \"yourSSID\", \"password\": \"yourPassword\"}</pre>";
  html += "</body></html>";
  setupServer.send(200, "text/html", html);
}

// --- Setup Portal: Process the form submission (web page) ---
void handleSetupSave() {
  if (setupServer.hasArg("ssid") && setupServer.hasArg("password")) {
    String newSSID = setupServer.arg("ssid");
    String newPassword = setupServer.arg("password");
    String newAPIKey = generateAPIKey();
    // Save the credentials and API key to nonvolatile storage.
    preferences.putString("ssid", newSSID);
    preferences.putString("password", newPassword);
    preferences.putString("apikey", newAPIKey);

    String response = "<html><body><h1>Configuration Saved!</h1>";
    response += "<p>Your API Key is: " + newAPIKey + "</p>";
    response += "<p>The device will restart now.</p>";
    response += "</body></html>";
    setupServer.send(200, "text/html", response);
    delay(2000);
    ESP.restart();
  } else {
    setupServer.send(400, "text/html", "Missing SSID or password");
  }
}

// --- Setup Portal: Process the POST request (JSON or URL-encoded) ---
void handleConfigPost() {
  // Only allow configuration if not already set.
  if (preferences.getString("ssid", "") != "") {
    setupServer.send(403, "application/json", "{\"error\":\"Configuration already set.\"}");
    return;
  }

  String newSSID, newPassword;

  // Check for POST arguments (this works for URL-encoded forms)
  if (setupServer.hasArg("ssid") && setupServer.hasArg("password")) {
    newSSID = setupServer.arg("ssid");
    newPassword = setupServer.arg("password");
  } else {
    // Alternatively, if posting JSON, get the body and parse minimally.
    String body = setupServer.arg("plain");
    // Expect JSON in the form: {"ssid": "yourSSID", "password": "yourPassword"}
    int ssidIndex = body.indexOf("\"ssid\"");
    int passIndex = body.indexOf("\"password\"");
    if (ssidIndex != -1 && passIndex != -1) {
      int colonIndex = body.indexOf(":", ssidIndex);
      int commaIndex = body.indexOf(",", ssidIndex);
      if (colonIndex != -1) {
        newSSID = body.substring(colonIndex + 1, commaIndex == -1 ? body.length() : commaIndex);
        newSSID.trim();
        newSSID.replace("\"", "");
      }
      colonIndex = body.indexOf(":", passIndex);
      int endIndex = body.indexOf("}", passIndex);
      if (colonIndex != -1 && endIndex != -1) {
        newPassword = body.substring(colonIndex + 1, endIndex);
        newPassword.trim();
        newPassword.replace("\"", "");
      }
    }
  }

  if (newSSID == "" || newPassword == "") {
    setupServer.send(400, "application/json", "{\"error\":\"Missing SSID or password\"}");
    return;
  }

  String newAPIKey = generateAPIKey();
  // Save the credentials and API key to nonvolatile storage.
  preferences.putString("ssid", newSSID);
  preferences.putString("password", newPassword);
  preferences.putString("apikey", newAPIKey);

  String jsonResponse = "{\"apikey\":\"" + newAPIKey + "\"}";
  setupServer.send(200, "application/json", jsonResponse);
  delay(2000);
  ESP.restart();
}

String getRequestData() {
  return  "{\"apiKey\": \"7d4eb3da-ba58-47a8-801c-5f74285d69b4\" }";
}

void sendPostMessage() {
  HTTPClient http;
  http.begin(endpointUrl);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST(getRequestData());

  // Check the response
  if (httpResponseCode == 200) {
      String response = http.getString();
      Serial.println("Response Code: " + String(httpResponseCode));
      Serial.println("Response: " + response);
      openOrCloseGate();
  } else {
      Serial.println("Error on sending POST: " + String(httpResponseCode));
  }
}

// --- Run the Setup Portal in AP mode ---
void runSetupPortal() {
  // Switch to AP mode and create an access point for setup.
  WiFi.mode(WIFI_AP);
  const char* apSSID = "ESP32-Setup";
  WiFi.softAP(apSSID);
  Serial.print("Setup AP started. Connect to: ");
  Serial.println(apSSID);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Seed the random generator (used for API key generation).
  randomSeed((unsigned long)esp_random());

  // Configure endpoints for the setup portal.
  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.on("/config", HTTP_POST, handleConfigPost);

  setupServer.begin();
  Serial.println("Setup web server started");

  Serial.println("HTTP server started in normal mode");
  Serial.println("Testing click of gate button.");
  Serial.println("End of the testing click of gate button.");

  // Handle client requests indefinitely until configuration is saved.
  while (true) {
    setupServer.handleClient();
    delay(1);
  }
}

void openOrCloseGate() {
  Serial.println("Open/Close started...");
  pinMode(relayPin, OUTPUT);
  pinMode(relayPinMinus, OUTPUT);
  
  Serial.println("Clicking for 3 sec...");
  delay(3000);
  Serial.println("Ended 3 sec click...");

  //digitalWrite(relayPin, LOW);
  pinMode(relayPin, INPUT);
  pinMode(relayPinMinus, INPUT);
  Serial.println("Open/Clocse ended");
}

// --- Main Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize the Preferences library.
  preferences.begin("config", false);

  // Read stored configuration.
  savedSSID = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedAPIKey = preferences.getString("apikey", "");

  // If no SSID or API key is stored, launch the setup portal.
  if (savedSSID == "" || savedAPIKey == "") {
    Serial.println("No configuration found. Starting setup portal.");
    runSetupPortal();  // This function does not return.
    return;
  }

  // Otherwise, switch to station mode and connect to your WiFi.
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  // Set up the relay control pin.
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  // Configure endpoints for the normal (secured) API.
  server.on("/open", HTTP_GET, handleOpen);
  server.on("/close", HTTP_GET, handleClose);
  server.on("/docs", HTTP_GET, handleDocs);
  server.on("/swagger.json", HTTP_GET, handleSwaggerJSON);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started in normal mode");
}

void loop() {
  // In normal mode, handle incoming client requests.
  server.handleClient();
  Serial.println("Waiting 1 second.");
  delay(1000);
  Serial.println("Asking for messsages...");
  sendPostMessage();
}
