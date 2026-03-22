#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define SCALE_NAME "FitTrack"

static BLEUUID SCALE_SERVICE_UUID("0000ffb0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHAR_FFB2_UUID("0000ffb2-0000-1000-8000-00805f9b34fb");
static BLEUUID CHAR_FFB3_UUID("0000ffb3-0000-1000-8000-00805f9b34fb");

// Packet type byte (index 6)
#define PKT_LIVE   0xCE  // Live measurement (weight fluctuating)
#define PKT_STABLE 0xCA  // Stable/final weight
#define PKT_POST   0xCB  // Post-measurement / stepping off

BLEScan* pBLEScan = nullptr;
BLEAdvertisedDevice* pScaleDevice = nullptr;
BLEClient* pClient = nullptr;
volatile bool connected = false;
bool doConnect = false;

class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* client) override {}
    void onDisconnect(BLEClient* client) override {
        connected = false;
        Serial.println("\n>> Scale disconnected. Rescanning...");
    }
};

void parseScaleData(uint8_t* data, size_t length) {
    if (length < 8 || data[0] != 0xAC || data[1] != 0x02) return;

    uint8_t type = data[6];

    // Verify checksum (low byte of sum bytes 2-6)
    uint8_t sum = 0;
    for (int i = 2; i <= 6; i++) sum += data[i];
    if (sum != data[7]) {
        Serial.printf("  Checksum mismatch: expected %02X got %02X\n", sum, data[7]);
        return;
    }

    // Only parse weight from live/stable packets
    if (type == PKT_LIVE || type == PKT_STABLE) {
        uint16_t raw = (data[2] << 8) | data[3];
        float weight = raw / 10.0f;

        if (type == PKT_STABLE) {
            Serial.printf(">>> FINAL WEIGHT: %.1f kg <<<\n", weight);
        } else {
            Serial.printf("  Measuring: %.1f kg\n", weight);
        }
    } else if (type == PKT_POST) {
        Serial.println("  [post-measurement data]");
    }
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    parseScaleData(pData, length);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        if (device.haveName() && device.getName() == SCALE_NAME) {
            Serial.println("FitTrack found!");
            if (pScaleDevice) delete pScaleDevice;
            pScaleDevice = new BLEAdvertisedDevice(device);
            doConnect = true;
            pBLEScan->stop();
        }
    }
};

bool connectToScale() {
    Serial.printf("Connecting to %s...\n", pScaleDevice->getAddress().toString().c_str());

    if (pClient) {
        delete pClient;
        pClient = nullptr;
    }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());

    if (!pClient->connect(pScaleDevice)) {
        Serial.println("Connection failed!");
        return false;
    }

    BLERemoteService* pService = pClient->getService(SCALE_SERVICE_UUID);
    if (!pService) {
        Serial.println("Scale service 0xFFB0 not found!");
        pClient->disconnect();
        return false;
    }

    BLERemoteCharacteristic* pFFB2 = pService->getCharacteristic(CHAR_FFB2_UUID);
    if (pFFB2 && pFFB2->canNotify()) {
        pFFB2->registerForNotify(notifyCallback);
    }

    BLERemoteCharacteristic* pFFB3 = pService->getCharacteristic(CHAR_FFB3_UUID);
    if (pFFB3 && pFFB3->canNotify()) {
        pFFB3->registerForNotify(notifyCallback);
    }

    Serial.println("Connected. Waiting for measurement...\n");
    connected = true;
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenTrackFit BLE ===");

    BLEDevice::init("OpenTrackFit");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

void loop() {
    if (doConnect && !connected) {
        connectToScale();
        doConnect = false;
    }

    if (!connected && !doConnect) {
        pBLEScan->start(5, false);
        pBLEScan->clearResults();
    }

    delay(500);
}
