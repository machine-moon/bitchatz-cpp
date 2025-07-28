#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <mutex>
#include <queue>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>
#include <mbedtls/sha256.h>

// ====== CONSTANTS ======
constexpr const char* DEVICE_NAME = "bitchat-esp32";
constexpr char SERVICE_UUID[] = "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C";
constexpr char CHAR_UUID[]    = "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D";
constexpr size_t MAX_QUEUE_SIZE = 32;
constexpr uint32_t SCAN_INTERVAL_SEC = 10;
constexpr size_t PROCESSED_ID_HISTORY = 128;
constexpr uint8_t LED_PIN = 2;
constexpr unsigned long LED_ON_DURATION = 300;

// Protocol fields
#define PKT_VERSION 1
#define PKT_TYPE_VERSION_HELLO 0x11
#define PKT_TYPE_VERSION_ACK   0x12
#define PKT_TTL 1

// ====== UTILS ======
std::string toHex(const std::vector<uint8_t>& v) {
  static const char hexmap[] = "0123456789abcdef";
  std::string s(v.size() * 2, ' ');
  for (size_t i = 0; i < v.size(); ++i) {
    s[2 * i] = hexmap[(v[i] & 0xF0) >> 4];
    s[2 * i + 1] = hexmap[v[i] & 0x0F];
  }
  return s;
}

// ====== HELLO/ACK PACKETS ======
std::vector<uint8_t> makeVersionHelloPacket() {
  std::vector<uint8_t> out;
  out.push_back(PKT_VERSION);                  // version
  out.push_back(PKT_TYPE_VERSION_HELLO);       // type
  out.push_back(PKT_TTL);                      // ttl

  // timestamp (uint64_t big endian)
  uint64_t now = std::time(nullptr);
  for (int i = 7; i >= 0; --i)
    out.push_back((now >> (i * 8)) & 0xFF);

  out.push_back(0); // flags

  // ---- payload ----
  std::vector<uint8_t> payload;
  payload.push_back(1);   // supported_versions count
  payload.push_back(1);   // supported_versions[0]
  payload.push_back(1);   // preferred_version
  std::string clientVer = "esp32";
  payload.push_back(clientVer.size());
  payload.insert(payload.end(), clientVer.begin(), clientVer.end());
  std::string platform = "esp32";
  payload.push_back(platform.size());
  payload.insert(payload.end(), platform.begin(), platform.end());
  payload.push_back(0);   // capabilities (empty)

  // payload length (big endian)
  uint16_t payloadLen = payload.size();
  out.push_back((payloadLen >> 8) & 0xFF);
  out.push_back(payloadLen & 0xFF);

  // senderID (8 bytes)
  std::string senderID = "ESP32ABC1";
  for (int i = 0; i < 8; ++i)
    out.push_back(i < senderID.size() ? senderID[i] : 0);

  // payload
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> makeVersionAckPacket(const std::vector<uint8_t>& rx) {
  // Copy fields from incoming VERSION_HELLO as base (esp senderID, timestamp)
  std::vector<uint8_t> out;
  out.push_back(PKT_VERSION);                // version
  out.push_back(PKT_TYPE_VERSION_ACK);       // type
  out.push_back(PKT_TTL);                    // ttl

  // Copy timestamp from incoming packet
  for (int i = 0; i < 8; ++i) out.push_back(rx[3 + i]);
  out.push_back(0); // flags

  // ---- payload ---- (mirror payload, or return empty just to signal ack)
  uint16_t payloadLen = 0;
  out.push_back(0); // payloadLen high
  out.push_back(0); // payloadLen low

  // senderID (8 bytes)
  std::string senderID = "ESP32ABC1";
  for (int i = 0; i < 8; ++i)
    out.push_back(i < senderID.size() ? senderID[i] : 0);
  // no payload
  return out;
}

// ====== THREAD-SAFE STRUCTURES ======
class SafePacketQueue {
public:
  bool push(const std::string& packetId, const std::vector<uint8_t>& payload) {
    std::scoped_lock lock(mutex);
    if (queue.size() >= MAX_QUEUE_SIZE) queue.pop();
    queue.push({ packetId, payload });
    return true;
  }
  bool pop(std::string& packetId, std::vector<uint8_t>& payload) {
    std::scoped_lock lock(mutex);
    if (queue.empty()) return false;
    auto& pkt = queue.front();
    packetId = pkt.packetId;
    payload = pkt.payload;
    queue.pop();
    return true;
  }
private:
  struct RelayPacket {
    std::string packetId;
    std::vector<uint8_t> payload;
  };
  std::queue<RelayPacket> queue;
  mutable std::mutex mutex;
};

class PacketDeduplicator {
public:
  bool isDuplicate(const std::string& id) {
    std::scoped_lock lock(mutex);
    if (std::find(history.begin(), history.end(), id) != history.end()) return true;
    if (history.size() >= PROCESSED_ID_HISTORY) history.erase(history.begin());
    history.push_back(id);
    return false;
  }
private:
  std::vector<std::string> history;
  std::mutex mutex;
};

class PeerRegistry {
public:
  bool add(const std::string& addr) {
    std::scoped_lock lock(mutex);
    if (std::find(peers.begin(), peers.end(), addr) == peers.end()) {
      peers.push_back(addr);
      Serial.printf("[PeerRegistry] New peer discovered: %s\n", addr.c_str());
      sendHelloToPeer(addr);
      return true;
    }
    return false;
  }
  void remove(const std::string& addr) {
    std::scoped_lock lock(mutex);
    auto it = std::remove(peers.begin(), peers.end(), addr);
    if (it != peers.end()) {
      Serial.printf("[PeerRegistry] Peer removed: %s\n", addr.c_str());
      peers.erase(it, peers.end());
    }
  }
  std::vector<std::string> getPeers() const {
    std::scoped_lock lock(mutex);
    return peers;
  }
  int count() const {
    std::scoped_lock lock(mutex);
    return peers.size();
  }
private:
  std::vector<std::string> peers;
  mutable std::mutex mutex;

  // Sends a VERSION_HELLO to peer when added
  static void sendHelloToPeer(const std::string& addr) {
    BLEClient* client = BLEDevice::createClient();
    bool connected = false;
    try {
      BLEAddress bleAddr(addr.c_str());
      connected = client->connect(bleAddr);
    } catch (...) {}
    if (!connected) {
      delete client;
      return;
    }
    BLERemoteService* remoteService = nullptr;
    try {
      remoteService = client->getService(SERVICE_UUID);
    } catch (...) {}
    if (!remoteService) {
      client->disconnect();
      delete client;
      return;
    }
    BLERemoteCharacteristic* remoteChar = nullptr;
    try {
      remoteChar = remoteService->getCharacteristic(CHAR_UUID);
    } catch (...) {}
    if (remoteChar && remoteChar->canWrite()) {
      std::vector<uint8_t> hello = makeVersionHelloPacket();
      try {
        remoteChar->writeValue(hello.data(), hello.size(), false);
        Serial.printf("[Hello] Sent VERSION_HELLO to %s\n", addr.c_str());
      } catch (...) {
        Serial.printf("[Hello] Failed to send to %s\n", addr.c_str());
      }
    }
    client->disconnect();
    delete client;
  }
};

// ====== GLOBALS ======
SafePacketQueue packetQueue;
PacketDeduplicator deduplicator;
PeerRegistry peerRegistry;
volatile bool ledActive = false;
unsigned long ledTurnOffTime = 0;
BLEServer* server = nullptr;

// ====== BLE CALLBACKS ======
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    Serial.println("[BLE] Client connected!");
  }
  void onDisconnect(BLEServer* pServer) override {
    Serial.println("[BLE] Client disconnected!");
  }
};

class BitchatCharCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String rxString = characteristic->getValue();
    std::vector<uint8_t> rxVec(rxString.begin(), rxString.end());
    if (rxVec.empty()) return;

    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, rxVec.data(), rxVec.size());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    std::string packetId = toHex(std::vector<uint8_t>(hash, hash + 32));

    Serial.printf("[CharCallback] Received packet of %d bytes, id=%s\n", (int)rxVec.size(), packetId.c_str());

    if (deduplicator.isDuplicate(packetId)) {
      Serial.printf("[CharCallback] Duplicate packet %s ignored.\n", packetId.c_str());
      return;
    }

    // Handle VERSION_HELLO and reply with VERSION_ACK
    if (rxVec.size() > 2 && rxVec[1] == PKT_TYPE_VERSION_HELLO) {
      Serial.println("[CharCallback] VERSION_HELLO received, replying VERSION_ACK.");
      sendVersionAckToSender(rxVec);
    }
    else if (rxVec.size() > 2 && rxVec[1] == PKT_TYPE_VERSION_ACK) {
      Serial.println("[CharCallback] VERSION_ACK received.");
      // Optionally handle mesh handshake logic here
    } else {
      // Enqueue for relay (if not version negotiation)
      if (packetQueue.push(packetId, rxVec)) {
        Serial.printf("[CharCallback] Queued packet %s\n", packetId.c_str());
      }
    }
  }

  // Helper: reply VERSION_ACK to sender
  static void sendVersionAckToSender(const std::vector<uint8_t>& rxVec) {
    // For full mesh, send VERSION_ACK to all known peers (optionally just to sender if you parse senderID)
    auto peers = peerRegistry.getPeers();
    for (const auto& addrStr : peers) {
      BLEClient* client = BLEDevice::createClient();
      bool connected = false;
      try {
        BLEAddress addr(addrStr.c_str());
        connected = client->connect(addr);
      } catch (...) {}
      if (!connected) {
        delete client;
        continue;
      }
      BLERemoteService* remoteService = nullptr;
      try {
        remoteService = client->getService(SERVICE_UUID);
      } catch (...) {}
      if (!remoteService) {
        client->disconnect();
        delete client;
        continue;
      }
      BLERemoteCharacteristic* remoteChar = nullptr;
      try {
        remoteChar = remoteService->getCharacteristic(CHAR_UUID);
      } catch (...) {}
      if (remoteChar && remoteChar->canWrite()) {
        std::vector<uint8_t> ack = makeVersionAckPacket(rxVec);
        try {
          remoteChar->writeValue(ack.data(), ack.size(), false);
          Serial.printf("[Hello] Sent VERSION_ACK to %s\n", addrStr.c_str());
        } catch (...) {
          Serial.printf("[Hello] Failed to send VERSION_ACK to %s\n", addrStr.c_str());
        }
      }
      client->disconnect();
      delete client;
    }
  }
};

class BitchatScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!advertisedDevice.haveServiceUUID()) return;
    if (!advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;
    std::string addr = std::string(advertisedDevice.getAddress().toString().c_str());
    peerRegistry.add(addr);
  }
};

// ====== RELAY LOGIC ======
void relayPacketToPeers(const std::string& packetId, const std::vector<uint8_t>& payload) {
  auto peers = peerRegistry.getPeers();
  if (peers.empty()) {
    Serial.println("[Relay] No peers to relay to.");
    return;
  }
  for (const auto& addrStr : peers) {
    BLEClient* client = BLEDevice::createClient();
    bool connected = false;
    try {
      BLEAddress addr(addrStr.c_str());
      connected = client->connect(addr);
    } catch (...) {}
    if (!connected) {
      Serial.printf("[Relay] Could not connect to %s\n", addrStr.c_str());
      delete client;
      continue;
    }
    BLERemoteService* remoteService = nullptr;
    try {
      remoteService = client->getService(SERVICE_UUID);
    } catch (...) {}
    if (!remoteService) {
      Serial.printf("[Relay] No service on %s, removing.\n", addrStr.c_str());
      client->disconnect();
      peerRegistry.remove(addrStr);
      delete client;
      continue;
    }
    BLERemoteCharacteristic* remoteChar = nullptr;
    try {
      remoteChar = remoteService->getCharacteristic(CHAR_UUID);
    } catch (...) {}
    if (remoteChar && remoteChar->canWrite()) {
      try {
        remoteChar->writeValue((uint8_t*)payload.data(), payload.size(), false);
        Serial.printf("[Relay] Relayed packet %s to %s\n", packetId.c_str(), addrStr.c_str());
      } catch (...) {
        Serial.printf("[Relay] Failed to relay to %s\n", addrStr.c_str());
      }
    }
    client->disconnect();
    delete client;
  }
  digitalWrite(LED_PIN, HIGH);
  ledActive = true;
  ledTurnOffTime = millis() + LED_ON_DURATION;
}

// ====== BLE SCAN COMPLETE CALLBACK ======
void onScanComplete(BLEScanResults results) {
  int knownPeers = peerRegistry.count();
  Serial.printf("[Scan] Scan completed: %d devices found (known peers: %d)\n", results.getCount(), knownPeers);
  BLEDevice::getScan()->start(SCAN_INTERVAL_SEC, onScanComplete, false);
}

// ====== BLE SETUP ======
void setupBLE() {
  BLEDevice::init(DEVICE_NAME);

  server = BLEDevice::createServer();
  server->setCallbacks(new MyServerCallbacks());

  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* characteristic = service->createCharacteristic(
    CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  characteristic->setCallbacks(new BitchatCharCallback());
  characteristic->addDescriptor(new BLE2902());
  characteristic->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new BitchatScanCallback(), true);
  scan->setActiveScan(true);
  scan->start(SCAN_INTERVAL_SEC, onScanComplete, false);

  Serial.println("[Setup] BLE initialized.");
}

// ====== MAIN ARDUINO LOOP ======
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  setupBLE();
  Serial.println("[Setup] Bitchat relay ready.");
}

void loop() {
  std::string packetId;
  std::vector<uint8_t> payload;
  if (packetQueue.pop(packetId, payload)) {
    relayPacketToPeers(packetId, payload);
  }
  if (ledActive && millis() >= ledTurnOffTime) {
    digitalWrite(LED_PIN, LOW);
    ledActive = false;
  }
  delay(50);
}
