#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "SPI.h"
#include "ERGFX.h"
#include "TFTM0.85-1.h"
#include "esp_sleep.h"

#define TFT_RST 11
#define TFT_DC 10
#define TFT_CS 9
#define TFT_CLK 13
#define TFT_MOSI 12

// TFT példány
GC9107 tft = GC9107(TFT_CS, TFT_DC, TFT_MOSI, TFT_CLK, TFT_RST);

// --- WiFi kliens beállítások (írjad át a saját hálózati adataidra) ---
const char* sta_ssid     = "HalLak90";
const char* sta_password = "DzsungelSZOKOZ90";
const char* http_hostname = "animanails";   // mDNS név: animanails.local

// Webszerver
AsyncWebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin-top: 30px; }
    img { max-width: 90%%; height: auto; margin-bottom: 20px; }
    form { margin-top: 20px; }
    input[type=submit] { padding: 10px 20px; font-size: 16px; }
  </style>
</head>
<body>
  <h2>Digital nail picture uploader (Station mode)</h2>
  <img src="/picture.bmp" alt="No image yet">
  <form method="POST" action="/upload" enctype="multipart/form-data">
    <input type="file" name="upload" accept=".bmp">
    <input type="submit" value="Upload">
  </form>
  <form action="/sleep" method="GET">
    <input type="submit" value="Sleep">
  </form>
</body>
</html>)rawliteral";

// === Feltöltés kezelése ===
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
        if (uploadFile)
        {
            uploadFile.close();
        }
        Serial.println("Upload complete: picture.bmp");

        // TFT frissítés
        drawBMP("/picture.bmp", tft, 0, 0);

        request->send(200, "text/plain", "Upload OK. Refresh page!");
    }
}

// === BMP kirajzolás ===
void drawBMP(const char *filename, GC9107 &tft, int16_t x, int16_t y)
{
    File bmpFile;
    int bmpWidth, bmpHeight;
    uint8_t bmpDepth;
    uint32_t bmpImageOffset;
    uint32_t rowSize;
    bool flip = true;

    if ((x >= tft.width()) || (y >= tft.height()))
    {
        return;
    }

    bmpFile = LittleFS.open(filename, "r");
    if (!bmpFile)
    {
        Serial.println("drawBMP: Nem sikerült megnyitni a fájlt!");
        return;
    }

    if (read16(bmpFile) == 0x4D42)
    {
        (void)read32(bmpFile);
        (void)read32(bmpFile);
        bmpImageOffset = read32(bmpFile);
        (void)read32(bmpFile);
        bmpWidth  = read32(bmpFile);
        bmpHeight = read32(bmpFile);

        if (read16(bmpFile) == 1)
        {
            bmpDepth = read16(bmpFile);
            if ((bmpDepth == 24) && (read32(bmpFile) == 0))
            {
                Serial.printf("BMP %dx%d, %d-bit\n", bmpWidth, bmpHeight, bmpDepth);

                rowSize = (bmpWidth * 3 + 3) & ~3;

                if (bmpHeight < 0)
                {
                    bmpHeight = -bmpHeight;
                    flip = false;
                }

                uint16_t *lineBuffer = (uint16_t *)malloc(bmpWidth * sizeof(uint16_t));
                if (!lineBuffer)
                {
                    Serial.println("Nincs memória a sor bufferhez!");
                    bmpFile.close();
                    return;
                }

                for (int row = 0; row < bmpHeight; row++)
                {
                    uint32_t pos;
                    if (flip)
                    {
                        pos = bmpImageOffset + (bmpHeight - 1 - row) * rowSize;
                    }
                    else
                    {
                        pos = bmpImageOffset + row * rowSize;
                    }

                    bmpFile.seek(pos);

                    for (int col = 0; col < bmpWidth; col++)
                    {
                        uint8_t b = bmpFile.read();
                        uint8_t g = bmpFile.read();
                        uint8_t r = bmpFile.read();
                        lineBuffer[col] = tft.color565(r, g, b);
                    }

                    tft.pushImage(x, y + row, bmpWidth, 1, lineBuffer);
                }

                free(lineBuffer);
            }
            else
            {
                Serial.println("Csak 24-bites, tömörítetlen BMP támogatott!");
            }
        }
    }
    else
    {
        Serial.println("drawBMP: Nem BMP fájl!");
    }
    bmpFile.close();
}

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

void setup()
{
    Serial.begin(115200);

    // TFT init
    //tft.begin();
    //tft.setRotation(1);
    //tft.fillScreen(GC9107_BLACK);

    // LittleFS
    if (!LittleFS.begin())
    {
        Serial.println("LittleFS mount failed");
        return;
    }

    // --- WiFi kliens mód ---
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(http_hostname);                 // helyi hostname (esetleg routerben megjelenik)
    WiFi.begin(sta_ssid, sta_password);

    Serial.printf("Csatlakozás a WiFi hálózathoz: %s\n", sta_ssid);

    // Várakozás csatlakozásra (max 15s)
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 150000)
    {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi csatlakoztatva!");
        Serial.print("IP cím: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("Nem sikerült WiFi-hez csatlakozni. Ellenőrizd az SSID/jelszó-t.");
        // dönthetsz úgy, hogy itt leállítod a setup-ot vagy újrapróbálod később
    }

    // --- mDNS szolgáltatás indítása: animanails.local ---
    if (MDNS.begin(http_hostname))
    {
        Serial.printf("mDNS inicializálva: http://%s.local\n", http_hostname);
        MDNS.addService("http", "tcp", 80);
    }
    else
    {
        Serial.println("mDNS start sikertelen.");
    }

    // TFT frissítés (ha van kép)
    //drawBMP("/picture.bmp", tft, 0, 0);

    // Weboldal
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send_P(200, "text/html", index_html); });

    server.on("/picture.bmp", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                  if (LittleFS.exists("/picture.bmp"))
                  {
                      request->send(LittleFS, "/picture.bmp", "image/bmp");
                  }
                  else
                  {
                      request->send(404, "text/plain", "No picture uploaded");
                  }
              });

    server.on("/upload", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              handleUpload);

    server.on("/sleep", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(200, "text/plain", "ESP32 is going to sleep... Reset to wake up!");
        delay(500);
        esp_deep_sleep_start();
    });

    // opcionális: minden "not found" kérésre index visszaadás (ha a kliens a host fejlécet másként adja)
    server.onNotFound([](AsyncWebServerRequest *request)
    {
        // egyszerű fallback: ha nem kérem statikus fájlt, adja vissza a fő oldalt
        if (request->method() == HTTP_GET)
        {
            request->send_P(200, "text/html", index_html);
        }
        else
        {
            request->send(404);
        }
    });

    server.begin();

    Serial.println("Webszerver elindult.");
    Serial.printf("Elérhető: http://%s.local vagy http://%s (ha a hálózat DNS beállítása erre mutat)\n",
                  http_hostname, WiFi.localIP().toString().c_str());
}

void loop()
{
    // nincs dolga, AsyncWebServer kezeli a kéréseket
}
