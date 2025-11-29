#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include "SparkFun_RGB_OLED_64x64.h"   // ÚJ OLED driver
#include "esp_sleep.h"

// --- OLED PIN-ek (SSD1357) ---
#define OLED_CS     7
#define OLED_DC     4
#define OLED_RST    3
#define OLED_CLK    5
#define OLED_MOSI   6

RGB_OLED_64x64 myOLED;

// --- WiFi beállítások (alapértelmezett, ha szükséges felülírható) ---
const char* http_hostname = "animanails";     // ha STA módban használjuk
const char* setup_hostname = "wifisetup";     // AP+mDNS név a setuphoz

// --- Globális AP név ---
const char* ap_ssid = "wifisetup";

// Webszerver
AsyncWebServer server(80);

// Globális cache és állapotok
String lastScanJson = "[]";
bool scanInProgress = false;

// --- index_html (fő oldal) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 30px; background-color: #121212; color: #FFFFFF; }
    img { max-width: 90%; height: auto; margin-bottom: 20px; border: 2px solid #444; border-radius: 8px; }
    form { margin-top: 20px; }
    .button { display: inline-block; width: 180px; height: 45px; line-height: 45px; font-size: 16px; background-color: #1E1E1E; color: #FFFFFF; border: 1px solid #444; border-radius: 5px; cursor: pointer; transition: 0.3s; position: relative; }
    .button:hover { background-color: #333; border-color: #666; }
    input[type=file] { display: none; }
    footer { margin-top: 60px; font-size: 14px; color: #AAAAAA; }
    #msg { margin-top: 8px; color: #9f9; word-break:break-all; white-space:pre-wrap; }
  </style>
</head>
<body>

  <img src="Logo.bmp" alt="No image yet">

  <!-- UPLOAD FORM -->
  <form id="uploadForm" enctype="multipart/form-data" onsubmit="return false;">
    <input type="file" id="fileInput" name="upload" accept=".bmp">

    <button type="button" class="button" onclick="selectFile()">
      Upload
    </button>

    <br><br>
    Only 64x64 *.BMP files are supported.
  </form>

  <br><br><br>

  <!-- WIFI OFF FORM -->
  <form id="wifioff" action="/sleep" method="GET">
    <button type="submit" class="button">Wifi off</button>
    <br><br>
    <small>(Turn off the WiFi of your Anima Nails to save battery)</small>
  </form>

  <!-- CLEAR SAVED NETWORKS (új gomb a főoldalon) -->
  <button id="clear_main" class="button" style="margin-top:12px;">Clear saved networks</button>
  <div id="msg"></div>

  <footer>
    Anima Nails - 2026
  </footer>


  <!-- JAVASCRIPT – AJAX UPLOAD + CLEAR -->
  <script>
    function selectFile() {
      const fileInput = document.getElementById('fileInput');
      fileInput.click();

      fileInput.onchange = function() {
        if (fileInput.files.length === 0) return;

        let formData = new FormData();
        formData.append("upload", fileInput.files[0]);

        fetch("/upload", {
          method: "POST",
          body: formData
        })
        .then(r => r.text())
        .then(text => {
          console.log("Upload response:", text);
          setTimeout(() => { location.reload(); }, 500);
        })
        .catch(err => {
          console.error("Upload failed:", err);
          alert("Upload failed!");
        });
      };
    }

    // Clear saved networks handler on main page
    document.getElementById('clear_main').onclick = function()
    {
      if (!confirm('Really clear all saved networks? This will remove saved SSID/password and reboot the device.'))
      {
        return;
      }

      document.getElementById('msg').innerText = 'Clearing saved networks and rebooting...';
      fetch('/clear', { method: 'POST' })
        .then(function(resp) { return resp.text(); })
        .then(function(text) {
          console.log('Clear response:', text);
          document.getElementById('msg').innerText = text;
        })
        .catch(function(e) {
          console.error('Clear request failed:', e);
          document.getElementById('msg').innerText = 'Clear request failed: ' + e;
        });
    };
  </script>

</body>
</html>
)rawliteral";

// --- BMP segédfüggvények ---
uint16_t read16(File &f)
{
    uint16_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    return result;
}

uint32_t read32(File &f)
{
    uint32_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    ((uint8_t *)&result)[2] = f.read();
    ((uint8_t *)&result)[3] = f.read();
    return result;
}

void drawBMP(const char *filename, int16_t x, int16_t y)
{
    File file = LittleFS.open(filename, "r");
    if (!file)
    {
        Serial.println("Nem sikerült megnyitni a BMP fájlt!");
        return;
    }
    uint8_t header[54];
    file.read(header, 54);
    int32_t width  = *(int32_t*)&header[18];
    int32_t height = *(int32_t*)&header[22];
    uint16_t bpp = *(uint16_t*)&header[28];
    if (bpp != 24)
    {
        Serial.println("Csak 24 bites BMP támogatott!");
        file.close();
        return;
    }
    for (int row = height - 1; row >= 0; row--)
    {
        for (int col = 0; col < width; col++)
        {
            uint8_t b = file.read();
            uint8_t g = file.read();
            uint8_t r = file.read();
            uint16_t color = RGBto565(r, g, b);
            myOLED.setPixel(x + col, y + row, color);
        }
    }
    file.close();
}

uint16_t RGBto565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3);
}

// --- Feltöltés ---
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    static File uploadFile;
    if (!index)
    {
        if (LittleFS.exists("/picture.bmp"))
        {
            LittleFS.remove("/picture.bmp");
        }
        uploadFile = LittleFS.open("/picture.bmp", FILE_WRITE);
    }
    if (uploadFile)
    {
        uploadFile.write(data, len);
    }
    if (final)
    {
        uploadFile.close();
        Serial.println("Upload complete!");
        myOLED.clearDisplay();
        drawBMP("/picture.bmp", 0, 0);
        request->send(200);
    }
}

// --- WiFi credential file helper ---
bool credentialsExist()
{
    return LittleFS.exists("/wifi.txt");
}

bool readCredentials(String &ssid, String &password)
{
    if (!LittleFS.exists("/wifi.txt"))
    {
        return false;
    }
    File f = LittleFS.open("/wifi.txt", "r");
    if (!f)
    {
        return false;
    }
    ssid = f.readStringUntil('\n');
    ssid.trim();
    password = f.readStringUntil('\n');
    password.trim();
    f.close();
    if (ssid.length() == 0)
    {
        Serial.println("readCredentials(): found file but SSID empty -> treat as no credentials");
        return false;
    }
    return true;
}

bool saveCredentials(const String &ssid, const String &password)
{
    File f = LittleFS.open("/wifi.txt", "w");
    if (!f)
    {
        Serial.println("saveCredentials(): failed to open /wifi.txt for write");
        return false;
    }
    f.println(ssid);
    f.println(password);
    f.close();
    Serial.printf("saveCredentials(): saved '%s'\n", ssid.c_str());
    return true;
}

// --- setup_html (AP setup page) ---
const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>WiFi setup</title>
  <style>
    body
    {
      font-family: Arial, sans-serif;
      text-align: center;
      margin: 30px;
      background-color: #121212;
      color: #FFFFFF;
    }
    h2
    {
      color: #FFFFFF;
    }
	img
    {
      max-width: 90%;
      height: auto;
      margin-bottom: 20px;
      border: 2px solid #444;
      border-radius: 8px;
    }
	
	/*label, select, input { display:block; width:100%; margin:8px 0; }*/
    select
	{
		position: relative;
		min-height: 40px;
		min-width: 200px;
		background: #1E1E1E;
		color: #fff;
		border: 1px solid #444;
		border-radius:5px;
		padding:8px;
	}
    input
	{
		min-height: 22px;
		min-width: 200px;
		
		position: relative;
		padding:8px;
		border-radius:5px;
		border:1px solid #444;
		background:#111;
		color:#fff;
	}
    button
	{
		position: relative;
		background-color: #1E1E1E;
		color: #FFFFFF;
		border: 1px solid #444;
        border-radius: 5px;
		padding: 10px 24px;
		font-size: 16px;
		margin-top:8px;
		min-width: 200px;
		border-radius:4px;
		cursor: pointer;
        transition: background-color 0.3s, border-color 0.3s;
	}
	  
      
      
      
      
      
      
	  
	  
	  
	  small { color:#aaa; }
    #msg { margin-top:8px; color:#9f9; word-break:break-all; white-space:pre-wrap; }
    #raw { margin-top:8px; color:#f99; font-size:12px; word-break:break-all; white-space:pre-wrap; }
	
	  footer
	  {
		margin-top: 40px;
		font-size: 12px;
		color: #AAAAAA;
	  }
	</style>
</head>
<body>
  <img src="Logo.bmp" alt="No image yet">
  <h2>WiFi Setup</h2>
  <p>Open this device at <strong>http://wifisetup.local</strong> (mDNS) or connect to the AP and open <em>http://192.168.4.1</em>.</p>
  <br><br>
  <label for="ssid">Available networks</label><br><br>
  <select id="ssid"><option>Scanning...</option></select><br><br>

  <label for="pwd">Password</label><br><br>
  <input id="pwd" type="password" placeholder="WiFi password"><br><br><br><br>

  <button id="rescan">Rescan</button>
	<br>
  <button id="save">Save & Reboot</button><br>
  <button id="clear">Clear saved networks</button><br>

  <p id="msg"></p>
  <pre id="raw"></pre>
  <br><br><br><br>
	<footer>
    Anima Nails - 2026
  </footer>
  
	<script>
  // Feltöltjük a select-et a kapott listából
  function populateSelectFromList(list)
  {
    const sel = document.getElementById('ssid');
    sel.innerHTML = '';
    if (!list || list.length === 0)
    {
      const opt = document.createElement('option');
      opt.value = '';
      opt.text = 'No networks found';
      sel.appendChild(opt);
      document.getElementById('msg').innerText = 'No networks found';
      return;
    }
    list.forEach(function(item) {
      var opt = document.createElement('option');
      opt.value = item.ssid;
      opt.text = item.ssid + ' (' + item.rssi + ' dBm)' + (item.secure? ' [secure]':' [open]');
      sel.appendChild(opt);
    });
    document.getElementById('msg').innerText = 'Loaded cached scan: ' + list.length + ' networks';
  }

  function loadCachedNetworks()
  {
    document.getElementById('msg').innerText = 'Loading cached list...';
    fetch(location.origin + '/lastscan', { cache: 'no-store' })
      .then(function(resp) { return resp.text(); })
      .then(function(text) {
        document.getElementById('raw').innerText = text;
        try {
          var list = JSON.parse(text);
        } catch (e) {
          console.error('Invalid JSON from /lastscan:', e);
          document.getElementById('msg').innerText = 'No cached data';
          return;
        }
        populateSelectFromList(list);
      })
      .catch(function(e) {
        console.error('Failed to fetch /lastscan:', e);
        document.getElementById('msg').innerText = 'Failed to load cached list';
      });
  }

  function doScanWithWarning()
  {
    if (!confirm('Scan will stop and restart the access point briefly. Your phone will disconnect and you will need to reconnect to the AP. Proceed?'))
    {
      return;
    }

    document.getElementById('msg').innerText = 'Starting scan... (you will be disconnected briefly)';
    document.getElementById('raw').innerText = '';
    document.getElementById('ssid').innerHTML = '<option>Scanning...</option>';

    const url = location.origin + '/scan';
    fetch(url, { cache: 'no-store' })
      .then(function(resp) { return resp.text(); })
      .then(function(text) {
        document.getElementById('raw').innerText = text;
        try {
          var list = JSON.parse(text);
          populateSelectFromList(list);
        } catch (e) {
          console.warn('Scan returned invalid JSON (or disconnected):', e);
        }
        waitForAPAndReload();
      })
      .catch(function(e) {
        console.error('scan request failed (probably disconnected):', e);
        document.getElementById('msg').innerText = 'Scan started — waiting for device to restore AP...';
        waitForAPAndReload();
      });
  }

  function waitForAPAndReload()
  {
    const pingUrl = location.origin + '/ping';
    document.getElementById('msg').innerText = 'Waiting for device to restore AP...';
    document.getElementById('raw').innerText = 'Waiting for AP...';
    const interval = setInterval(function()
    {
      fetch(pingUrl, { cache: 'no-store' })
        .then(function(r) {
          if (r.status === 200)
          {
            clearInterval(interval);
            setTimeout(function() { loadCachedNetworks(); }, 800);
          }
        })
        .catch(function() {
          // maradunk várakozóban
        });
    }, 1000);
  }

  // Események
  document.getElementById('rescan').onclick = function() { doScanWithWarning(); };

  // --- SAVE handler: POST form adatokkal a /save végpontnak ---
  document.getElementById('save').onclick = function()
  {
    const ssid = document.getElementById('ssid').value;
    const pwd = document.getElementById('pwd').value;
    if (!ssid)
    {
      document.getElementById('msg').innerText = 'Please choose an SSID.';
      return;
    }
    var form = new FormData();
    form.append('ssid', ssid);
    form.append('password', pwd);
    document.getElementById('msg').innerText = 'Saving credentials...';
    fetch(location.origin + '/save', { method: 'POST', body: form })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        document.getElementById('raw').innerText = t;
        document.getElementById('msg').innerText = 'Saved. Device will reboot.';
      })
      .catch(function(e) {
        console.error('Save failed:', e);
        document.getElementById('msg').innerText = 'Save failed: ' + e;
      });
  };

  // Clear saved networks: POST /clear (törli a /wifi.txt fájlt és újraindít)
  document.getElementById('clear').onclick = function()
  {
    if (!confirm('Really clear all saved networks? This will remove saved SSID/password and reboot the device.'))
    {
      return;
    }

    document.getElementById('msg').innerText = 'Clearing saved networks and rebooting...';
    fetch(location.origin + '/clear', { method: 'POST' })
      .then(function(resp) { return resp.text(); })
      .then(function(text) { document.getElementById('raw').innerText = text; })
      .catch(function(e) {
        console.error('Clear request failed:', e);
        document.getElementById('msg').innerText = 'Clear request failed: ' + e;
      });
  };

  window.addEventListener('load', function() { setTimeout(loadCachedNetworks, 300); });
  </script>

</body>
</html>
)rawliteral";

// --- Scan function (leállítja az AP, scannel, visszaállít) ---
String scanNetworksJson()
{
    Serial.println("scanNetworksJson(): preparing to scan...");
    scanInProgress = true;

    bool hadAP = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
    if (hadAP)
    {
        Serial.println("scanNetworksJson(): stopping softAP temporarily (preserve config)...");
        WiFi.softAPdisconnect(false);
        delay(200);
    }

    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(200);

    int n = -1;
    const int maxRetries = 3;
    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        Serial.printf("scanNetworksJson(): attempt %d to scan...\n", attempt);
        n = WiFi.scanNetworks();
        Serial.printf("scanNetworksJson(): attempt %d result = %d\n", attempt, n);
        if (n >= 0) break;
        delay(300);
    }

    String json = "[";
    if (n > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            String ssid = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) + ",\"secure\":" + (secure ? "true" : "false") + "}";
            if (i < n - 1) json += ",";
        }
    }
    json += "]";

    // Frissítjük a cache-t és státuszt
    lastScanJson = json;
    scanInProgress = false;

    if (hadAP)
    {
        Serial.println("scanNetworksJson(): restoring AP mode...");
        WiFi.mode(WIFI_AP_STA);
        delay(300);
        WiFi.softAP(ap_ssid);
        delay(400); // hosszabb várakozás
        MDNS.end();
        delay(100);
        if (MDNS.begin(setup_hostname))
        {
            MDNS.addService("http", "tcp", 80);
            Serial.println("scanNetworksJson(): mDNS restarted (wifisetup.local)");
        }
        delay(200); // extra buffer
    }

    return json;
}

// --- setup / loop ---
void setup()
{
    Serial.begin(115200);
    Serial.println("Boot...");

    if (!LittleFS.begin())
    {
        Serial.println("LittleFS mount failed!");
        return;
    }

    // --- Globális végpontok, elérhetők mind STA és AP módban ---
    // Clear saved credentials és reboot (elérhető lesz http://animanails.local/clear)
    server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        Serial.println("HTTP /clear requested - deleting /wifi.txt if exists (global handler)");

        bool ok = true;
        if (LittleFS.exists("/wifi.txt"))
        {
            if (!LittleFS.remove("/wifi.txt"))
            {
                Serial.println("Failed to remove /wifi.txt");
                ok = false;
            }
            else
            {
                Serial.println("/wifi.txt removed");
            }
        }
        else
        {
            Serial.println("/wifi.txt not present");
        }

        if (ok)
        {
            request->send(200, "text/plain", "Cleared saved networks. Rebooting...");
            delay(500);
            ESP.restart();
        }
        else
        {
            request->send(500, "text/plain", "Failed to clear saved networks");
        }
    });

    // OLED init
    SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
    myOLED.begin(OLED_DC, OLED_RST, OLED_CS, SPI, 8000000);
    myOLED.clearDisplay();

    // Ha vannak elmentett hitelesítők -> próbálunk STA módban csatlakozni
    String saved_ssid, saved_pwd;
    if (readCredentials(saved_ssid, saved_pwd))
    {
        Serial.printf("Found saved WiFi: '%s'\n", saved_ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(http_hostname);
        WiFi.begin(saved_ssid.c_str(), saved_pwd.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000)
        {
            delay(200);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("WiFi OK (STA)");
            Serial.println(WiFi.localIP());

            if (MDNS.begin(http_hostname))
            {
                Serial.printf("mDNS (STA): http://%s.local\n", http_hostname);
                MDNS.addService("http", "tcp", 80);
            }

            // OLED kép kirajzolása, ha van
            if (LittleFS.exists("/picture.bmp"))
            {
                drawBMP("/picture.bmp", 0, 0);
            }

            // Web UI (STA módban)
            server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
                request->send_P(200, "text/html", index_html);
            });

            server.on("/upload", HTTP_POST,
                      [](AsyncWebServerRequest *request) {},
                      handleUpload);

            server.serveStatic("/", LittleFS, "/");

            // a többi végpont (save/clear/scan/ping/lastscan) nem szükséges STA módban itt
            server.begin();
            return;
        }
        else
        {
            Serial.println("Saved WiFi nem csatlakozott, belépünk AP (setup) módba.");
            // továbbhaladunk AP setupra
        }
    }
    else
    {
        Serial.println("Nincs mentett WiFi - AP setup indul.");
    }

    // --- AP (setup) mód ---
    WiFi.mode(WIFI_AP_STA);   // AP + lehetővé teszi a scan-t
    const char* ap_pw = nullptr; // open AP (ha szeretnéd, megadhatsz jelszót)
    if (ap_pw)
    {
        WiFi.softAP(ap_ssid, ap_pw);
    }
    else
    {
        WiFi.softAP(ap_ssid);
    }

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP: ");
    Serial.println(apIP);

    // mDNS a setup névvel
    if (MDNS.begin(setup_hostname))
    {
        Serial.printf("mDNS (AP): http://%s.local\n", setup_hostname);
        MDNS.addService("http", "tcp", 80);
    }

    // --- Register endpoints (AP mode) ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send_P(200, "text/html", setup_html);
    });

    // /scan endpoint
    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        Serial.println("HTTP /scan requested");
        if (scanInProgress)
        {
            Serial.println("HTTP /scan: scan in progress, returning lastScanJson");
            request->send(200, "application/json", lastScanJson);
            return;
        }
        String json = scanNetworksJson();
        Serial.print("-> /scan response JSON: ");
        Serial.println(json);
        request->send(200, "application/json", json);
    });

    // /ping endpoint
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(200, "text/plain", "OK");
    });

    // /lastscan endpoint
    server.on("/lastscan", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        Serial.println("HTTP /lastscan requested");
        if (lastScanJson.length() == 0)
        {
            request->send(200, "application/json", "[]");
        }
        else
        {
            request->send(200, "application/json", lastScanJson);
        }
    });

    // Save endpoint (POST form)
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        if (request->hasParam("ssid", true) && request->hasParam("password", true))
        {
            String ssid = request->getParam("ssid", true)->value();
            String password = request->getParam("password", true)->value();

            bool ok = saveCredentials(ssid, password);
            if (ok)
            {
                request->send(200, "text/plain", "OK");
                Serial.println("Saved credentials. Rebooting in 1s...");
                delay(1000);
                ESP.restart();
            }
            else
            {
                request->send(500, "text/plain", "Save failed");
            }
        }
        else
        {
            request->send(400, "text/plain", "Missing params");
        }
    });

    // Clear saved credentials és reboot
    /*server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        Serial.println("HTTP /clear requested - deleting /wifi.txt if exists");

        bool ok = true;
        if (LittleFS.exists("/wifi.txt"))
        {
            if (!LittleFS.remove("/wifi.txt"))
            {
                Serial.println("Failed to remove /wifi.txt");
                ok = false;
            }
            else
            {
                Serial.println("/wifi.txt removed");
            }
        }
        else
        {
            Serial.println("/wifi.txt not present");
        }

        if (ok)
        {
            request->send(200, "text/plain", "Cleared saved networks. Rebooting...");
            delay(500);
            ESP.restart();
        }
        else
        {
            request->send(500, "text/plain", "Failed to clear saved networks");
        }
    });*/

    // Upload endpoint (AP mode)
    server.on("/upload", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              handleUpload);

    server.serveStatic("/", LittleFS, "/");

    // Start server AFTER all endpoints registered
    server.begin();

    // Rajzolhatunk alapképet is (ha van)
    if (LittleFS.exists("/picture.bmp"))
    {
        drawBMP("/picture.bmp", 0, 0);
    }
}

void loop()
{
    // semmi különös: AsyncWebServer kezeli a kéréseket
}
