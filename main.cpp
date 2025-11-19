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

// --- WiFi beállítások ---
const char* sta_ssid     = "HalLak90";
const char* sta_password = "DzsungelSZOKOZ90";
const char* http_hostname = "animanails";

// Webszerver
AsyncWebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>

    body {
      font-family: Arial, sans-serif;
      text-align: center;
      margin: 30px;
      background-color: #121212;
      color: #FFFFFF;
    }

    img {
      max-width: 90%;
      height: auto;
      margin-bottom: 20px;
      border: 2px solid #444;
      border-radius: 8px;
    }

    form {
      margin-top: 20px;
    }

    /* GOMBOK – minden gomb egységes méret */
    .button {
      display: inline-block;
      width: 180px;
      height: 45px;
      line-height: 45px;
      font-size: 16px;
      background-color: #1E1E1E;
      color: #FFFFFF;
      border: 1px solid #444;
      border-radius: 5px;
      cursor: pointer;
      transition: 0.3s;
      position: relative;
    }

    .button:hover {
      background-color: #333;
      border-color: #666;
    }

    /* TOOLTIP */
    .button .tooltip {
      visibility: hidden;
      width: 240px;
      background-color: #222;
      color: #fff;
      text-align: left;
      border-radius: 6px;
      padding: 10px;
      position: absolute;
      z-index: 1;
      bottom: 125%;
      left: 50%;
      transform: translateX(-50%);
      opacity: 0;
      transition: opacity 0.3s;
      font-size: 13px;
      border: 1px solid #555;
      box-shadow: 0 2px 6px rgba(0,0,0,0.4);
    }

    .button .tooltip::after {
      content: "";
      position: absolute;
      top: 100%;
      left: 50%;
      margin-left: -5px;
      border-width: 5px;
      border-style: solid;
      border-color: #222 transparent transparent transparent;
    }

    .button:hover .tooltip {
      visibility: visible;
      opacity: 1;
    }

    /* Rejtett fájl input */
    input[type=file] {
      display: none;
    }

    footer {
      margin-top: 60px;
      font-size: 14px;
      color: #AAAAAA;
    }
  </style>
</head>
<body>

  <img src="Logo.bmp" alt="No image yet">

  <!-- UPLOAD FORM -->
  <form id="uploadForm" enctype="multipart/form-data" onsubmit="return false;">
    <input type="file" id="fileInput" name="upload" accept=".bmp">

    <button type="button" class="button" onclick="selectFile()">
      Upload
      <span class="tooltip">
        Only <strong>.BMP</strong> files are supported.<br>
        Recommended resolution: <strong>64x64 pixels</strong>.<br>
        The upload starts automatically after selection.
      </span>
    </button>

    <br><br>
    Only 64x64 *.BMP files are supported.
  </form>

  <br><br><br>

  <!-- WIFI OFF FORM -->
  <form action="/sleep" method="GET">
    <button type="submit" class="button">Wifi off</button>
    <br><br>
    <small>(Turn off the WiFi of your Anima Nails to save battery)</small>
  </form>

  <footer>
    Anima Nails - 2026
  </footer>


  <!-- JAVASCRIPT – AJAX UPLOAD -->
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

          // Késleltetett automatikus frissítés
          setTimeout(() => {
            location.reload();
          }, 500);
        })
        .catch(err => {
          console.error("Upload failed:", err);
          alert("Upload failed!");
        });
      };
    }
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

    // BMP header átugrás (54 byte)
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
    // SparkFun OLED BGR565 formátumot vár!
    return ((b & 0xF8) << 8) |  // <-- b megy a felső 5 bitre
           ((g & 0xFC) << 3) |
           (r >> 3);           // <-- r megy az alsó 5 bitre
}




// --- Feltöltés ---
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index,
                  uint8_t *data, size_t len, bool final)
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

        //request->send(200, "text/plain", "Upload OK. Refresh page!");
        request->send(200);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Boot...");

    if (!LittleFS.begin())
    {
        Serial.println("LittleFS mount failed!");
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(http_hostname);
    WiFi.begin(sta_ssid, sta_password);

    Serial.printf("Csatlakozás: %s\n", sta_ssid);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000)
    {
        delay(200);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi OK");
        Serial.println(WiFi.localIP());

        if (MDNS.begin(http_hostname))
        {
            Serial.printf("mDNS: http://%s.local\n", http_hostname);
            MDNS.addService("http", "tcp", 80);
        }

        SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
        myOLED.begin(OLED_DC, OLED_RST, OLED_CS, SPI, 8000000);

        myOLED.clearDisplay();
        drawBMP("/picture.bmp", 0, 0);
    }
    else
    {
        Serial.println("WiFi nem csatlakozott.");
    }

    // --- Web UI ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send_P(200, "text/html", index_html);
    });

    server.on("/upload", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              handleUpload);

    server.serveStatic("/", LittleFS, "/");
    server.begin();
}

void loop()
{
}
