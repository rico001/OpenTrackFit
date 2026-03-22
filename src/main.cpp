#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

BLEScan* pBLEScan = nullptr;

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        Serial.printf("  [%s] RSSI: %d  Name: %s\n",
            device.getAddress().toString().c_str(),
            device.getRSSI(),
            device.haveName() ? device.getName().c_str() : "(unknown)");
    }
};

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenTrackFit BLE Scanner ===");

    BLEDevice::init("OpenTrackFit");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

void loop() {
    Serial.println("\nScanning...");
    BLEScanResults results = pBLEScan->start(5, false);
    Serial.printf("Scan done. %d devices found.\n", results.getCount());
    pBLEScan->clearResults();
    delay(3000);
}
