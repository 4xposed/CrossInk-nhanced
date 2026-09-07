#include "NearbyStatsSyncActivity.h"

#include "components/TouchHeaderBackButton.h"

#ifdef SIMULATOR

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() = default;

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::onExit() { Activity::onExit(); }

void NearbyStatsSyncActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back))
    exitViaBack();
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_NEARBY_STATS_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_NEARBY_STATS_SYNC));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NEARBY_STATS_SIMULATOR_UNAVAILABLE), true,
                            EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t*, const uint8_t*, int) {}

void NearbyStatsSyncActivity::exitViaBack() {
  mappedInput.suppressNextBackRelease();
  finish();
}

void NearbyStatsSyncActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}

#else

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/TouchActionButtons.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* LOG_TAG = "NSYNC";
constexpr const char* CROSSPOINT_ROOT = "/.crosspoint";
constexpr const char* GLOBAL_STATS_PATH = "/.crosspoint/global_stats.bin";
constexpr const char* SYNCED_STATS_DIR = "/.crosspoint/synced_stats";
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint8_t MAX_STATS_BYTES = static_cast<uint8_t>(GlobalReadingStats::CURRENT_FILE_SIZE);
constexpr uint8_t PACKET_HEADER_BYTES = 14;
constexpr uint8_t MAX_DEVICE_NAME_BYTES = static_cast<uint8_t>(CrossPointSettings::MAX_DEVICE_NAME_LENGTH);
constexpr uint32_t HELLO_INTERVAL_MS = 750;
constexpr uint8_t BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

NearbyStatsSyncActivity* activeActivity = nullptr;

TouchActionButtons::Layout touchActionLayout(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  constexpr uint8_t buttonCount = 2;
  constexpr int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string statsFileNameForDeviceMac(const std::array<uint8_t, 6>& mac) {
  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

std::string syncedStatsPathForDeviceMac(const std::array<uint8_t, 6>& mac) {
  return std::string(SYNCED_STATS_DIR) + "/" + statsFileNameForDeviceMac(mac);
}

bool isValidStatsPayload(const uint8_t* data, const uint8_t size) {
  return validateGlobalReadingStatsSummary(data, size);
}

bool ensureSyncedStatsDirectory() {
  return Storage.ensureDirectoryExists(CROSSPOINT_ROOT) && Storage.ensureDirectoryExists(SYNCED_STATS_DIR);
}

bool readSmallFile(const char* path, std::array<uint8_t, MAX_STATS_BYTES>& out, uint8_t& size) {
  return readGlobalReadingStatsSummary(path, out.data(), size);
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (!activeActivity || !info || !info->src_addr) return;
  activeActivity->enqueueEspNowPacket(info->src_addr, data, length);
}

}  // namespace

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput), eventMutex_(xSemaphoreCreateMutex()) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  setState(State::STARTING);

  if (esp_efuse_mac_get_default(localDeviceMac_.data()) != ESP_OK) {
    setError("Could not read device id");
    return;
  }

  if (!beginEspNow()) {
    setError("Could not start nearby sync");
    return;
  }

  setState(State::READY);
}

void NearbyStatsSyncActivity::onExit() {
  Activity::onExit();
  endEspNow();
}

void NearbyStatsSyncActivity::loop() {
  processEvents();

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitViaBack();
    return;
  }

  const bool canStartSync = state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR;
  if (mappedInput.hasTouch() && canStartSync) {
    const auto actions = touchActionLayout(renderer);
    int touchedAction = -1;
    const auto touch = mappedInput.rowTouch(touchedAction, actions.buttons[0].y,
                                            TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
                                            actions.count, actions.buttons[0].x,
                                            actions.buttons[0].x + actions.buttons[0].width, actions.buttons[0].height);
    if (touch == MappedInputManager::RowTouch::Down) return;
    if (touch == MappedInputManager::RowTouch::Tap) {
      if (touchedAction == 0) {
        startSync();
      } else if (touchedAction == 1) {
        exitViaBack();
      }
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && canStartSync) {
    startSync();
    return;
  }

  updateSyncProgress();
}

bool NearbyStatsSyncActivity::beginEspNow() {
  WiFi.mode(WIFI_STA);
  // A saved station can begin reconnecting as soon as STA mode starts. ESP-IDF
  // cannot change channels while it is associating, which is especially easy
  // to hit on the X4 Pro's faster S3 radio.
  if (!WiFi.disconnect(false, false, 1000)) {
    LOG_DBG(LOG_TAG, "Disconnect before ESP-NOW setup timed out");
  }
  delay(100);
  WiFi.setSleep(false);
  const esp_err_t channelResult = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (channelResult != ESP_OK) {
    LOG_ERR(LOG_TAG, "Could not select ESP-NOW channel: %d", static_cast<int>(channelResult));
    return false;
  }
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) return false;
  espNowStarted_ = true;

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) return false;
  if (!addPeer(BROADCAST_MAC)) return false;
  activeActivity = this;
  return true;
}

void NearbyStatsSyncActivity::endEspNow() {
  if (activeActivity == this) activeActivity = nullptr;
  if (espNowStarted_) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    espNowStarted_ = false;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

bool NearbyStatsSyncActivity::prepareLocalStats() {
  localStatsReady_ = false;
  if (!ensureSyncedStatsDirectory()) {
    setError("could not create synced stats directory");
    return false;
  }

  // Ensure a valid local stats payload exists before exchanging stats.
  if (!GlobalReadingStats::load().save()) {
    setError("local stats save failed");
    return false;
  }

  if (!readSmallFile(GLOBAL_STATS_PATH, localStats_, localStatsSize_)) {
    setError("local stats unavailable");
    return false;
  }

  localStatsReady_ = true;
  return true;
}

void NearbyStatsSyncActivity::startSync() {
  protocol_.start(millis());
  errorMessage_.clear();
  peerSeen_ = false;
  peerSourceMac_ = {};
  peerDeviceMac_ = {};
  peerId_.clear();
  peerName_.clear();
  lastHelloMs_ = 0;

  if (!prepareLocalStats()) return;

  setState(State::DISCOVERING);
  sendHello();
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, const int length) {
  if (!eventMutex_ || length < 0) return;
  SyncEvent event;
  if (!nearby_stats::decode(sourceMac, data, static_cast<size_t>(length), event) || event.deviceMac == localDeviceMac_)
    return;

  if (xSemaphoreTake(eventMutex_, 0) != pdTRUE) return;
  for (uint8_t offset = 0; offset < eventCount_; ++offset) {
    const uint8_t eventIndex = static_cast<uint8_t>((eventHead_ + offset) % MAX_SYNC_EVENTS);
    SyncEvent& queuedEvent = events_[eventIndex];
    if (queuedEvent.type == event.type && queuedEvent.sourceMac == event.sourceMac &&
        queuedEvent.deviceMac == event.deviceMac && queuedEvent.capability == event.capability) {
      queuedEvent = event;
      xSemaphoreGive(eventMutex_);
      return;
    }
  }

  if (eventOverflow_ || eventCount_ >= MAX_SYNC_EVENTS) {
    eventOverflow_ = true;
    eventHead_ = 0;
    eventCount_ = 0;
  } else {
    const uint8_t eventTail = static_cast<uint8_t>((eventHead_ + eventCount_) % MAX_SYNC_EVENTS);
    events_[eventTail] = event;
    eventCount_++;
  }
  xSemaphoreGive(eventMutex_);
}

void NearbyStatsSyncActivity::processEvents() {
  while (true) {
    SyncEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (eventOverflow_) {
        eventOverflow_ = false;
        eventHead_ = 0;
        eventCount_ = 0;
        hasOverflow = true;
      }
      if (eventCount_ > 0) {
        event = events_[eventHead_];
        eventHead_ = static_cast<uint8_t>((eventHead_ + 1) % MAX_SYNC_EVENTS);
        eventCount_--;
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }

    if (hasOverflow) {
      protocol_.overflow();
      setError("sync event queue overflow");
      return;
    }
    if (!hasEvent) return;
    handleEvent(event);
  }
}

nearby_stats::Session::Callbacks NearbyStatsSyncActivity::protocolCallbacks() {
  return {this,
          [](void* context, PacketType type) {
            auto& activity = *static_cast<NearbyStatsSyncActivity*>(context);
            activity.peerSourceMac_ = activity.protocol_.sourceMac;
            activity.peerDeviceMac_ = activity.protocol_.deviceMac;
            return activity.sendPacket(type, activity.peerSourceMac_.data());
          },
          [](void*, const SyncEvent& event) {
            return ensureSyncedStatsDirectory() &&
                   nearby_stats::publishSummary(syncedStatsPathForDeviceMac(event.deviceMac).c_str(),
                                                event.stats.data(), event.statsSize);
          }};
}
void NearbyStatsSyncActivity::applyProtocolState() {
  peerSeen_ = protocol_.peerSeen;
  peerSourceMac_ = protocol_.sourceMac;
  peerDeviceMac_ = protocol_.deviceMac;
  using S = nearby_stats::Session::State;
  switch (protocol_.state) {
    case S::Discovering:
      break;
    case S::Syncing:
      setState(State::SYNCING);
      break;
    case S::Synced:
      setState(State::SYNCED);
      break;
    case S::VersionMismatch:
      setError(tr(STR_NEARBY_STATS_VERSION_MISMATCH));
      break;
    case S::StorageError:
      setError("could not save stats");
      break;
    case S::Timeout:
      setError(peerSeen_ ? "stats sync timed out" : "no reader found");
      break;
    case S::Overflow:
      setError("sync event queue overflow");
      break;
  }
}
void NearbyStatsSyncActivity::handleEvent(const SyncEvent& event) {
  if (state_ == State::ERROR) return;
  if (state_ != State::DISCOVERING && state_ != State::SYNCING &&
      !(state_ == State::SYNCED && event.type == PacketType::STATS && event.deviceMac == peerDeviceMac_)) {
    if (event.type == PacketType::ACK || event.type == PacketType::NAME) return;
    protocol_.start(millis());
    if (!prepareLocalStats()) return;
  }
  protocol_.receive(event, millis(), protocolCallbacks());
  applyProtocolState();
  if (event.deviceMac == peerDeviceMac_ && event.sourceMac == peerSourceMac_) {
    peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
    if (event.type == PacketType::NAME) {
      peerName_ = event.deviceName.data();
      requestUpdate();
    }
  }
}

bool NearbyStatsSyncActivity::addPeer(const uint8_t* peerMac) {
  if (!peerMac) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, ESP_NOW_ETH_ALEN);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  const esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

bool NearbyStatsSyncActivity::sendPacket(const PacketType type, const uint8_t* peerMac) {
  if (!peerMac || !espNowStarted_) return false;
  if (!addPeer(peerMac)) return false;

  std::array<uint8_t, PACKET_HEADER_BYTES + MAX_STATS_BYTES> packet = {};
  const uint8_t* payload = nullptr;
  uint8_t payloadSize = 0;
  if (type == PacketType::STATS) {
    if (!localStatsReady_ || !isValidStatsPayload(localStats_.data(), localStatsSize_)) return false;
    payload = localStats_.data();
    payloadSize = localStatsSize_;
  } else if (type == PacketType::NAME) {
    const char* name = SETTINGS.getEffectiveDeviceName();
    payload = reinterpret_cast<const uint8_t*>(name);
    payloadSize = static_cast<uint8_t>(std::min(std::strlen(name), static_cast<size_t>(MAX_DEVICE_NAME_BYTES)));
  }
  const size_t length = nearby_stats::encode(type, localDeviceMac_.data(), GlobalReadingStats::CURRENT_FILE_VERSION,
                                             payload, payloadSize, packet.data());
  if (!length) return false;

  const esp_err_t result = esp_now_send(peerMac, packet.data(), length);
  if (result != ESP_OK) {
    LOG_ERR(LOG_TAG, "esp_now_send failed: %d", static_cast<int>(result));
    return false;
  }
  return true;
}

bool NearbyStatsSyncActivity::sendHello() {
  lastHelloMs_ = millis();
  return sendPacket(PacketType::HELLO, BROADCAST_MAC);
}

void NearbyStatsSyncActivity::exitViaBack() {
  mappedInput.suppressNextBackRelease();
  finish();
}

void NearbyStatsSyncActivity::updateSyncProgress() {
  if (state_ != State::DISCOVERING && state_ != State::SYNCING) return;
  const uint32_t now = millis();
  protocol_.tick(now, protocolCallbacks());
  applyProtocolState();
  if (!protocol_.terminal() && !protocol_.peerSeen && now - lastHelloMs_ >= HELLO_INTERVAL_MS) sendHello();
}

void NearbyStatsSyncActivity::setState(const State state) {
  if (state_ == state) return;
  state_ = state;
  requestUpdate();
}

void NearbyStatsSyncActivity::setError(const std::string& error) {
  LOG_ERR(LOG_TAG, "%s", error.c_str());
  errorMessage_ = error;
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_NEARBY_STATS_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_NEARBY_STATS_SYNC));
  }

  const int centerY = pageHeight / 2 - 20;
  std::string primary;
  std::string detailPrimary;
  std::string detailSecondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::READY:
      primary = tr(STR_NEARBY_STATS_READY);
      detailPrimary = std::string(tr(STR_DEVICE_NAME)) + ": " + SETTINGS.getEffectiveDeviceName();
      break;
    case State::DISCOVERING:
      primary = tr(STR_NEARBY_STATS_SCANNING);
      break;
    case State::SYNCING:
      primary = tr(STR_NEARBY_STATS_SYNCING);
      detailPrimary = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) +
                      ": " + (peerName_.empty() ? peerId_ : peerName_);
      if (peerDeviceMac_ != std::array<uint8_t, 6>{}) {
        detailSecondary = std::string(tr(STR_FILENAME)) + ": " + statsFileNameForDeviceMac(peerDeviceMac_);
      }
      break;
    case State::SYNCED:
      primary = tr(STR_NEARBY_STATS_SYNCED);
      detailPrimary = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) +
                      ": " + (peerName_.empty() ? peerId_ : peerName_);
      if (peerDeviceMac_ != std::array<uint8_t, 6>{}) {
        detailSecondary = std::string(tr(STR_FILENAME)) + ": " + statsFileNameForDeviceMac(peerDeviceMac_);
      }
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      detailPrimary = errorMessage_;
      break;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    renderReady(primary, detailPrimary, detailSecondary);
    if (mappedInput.hasTouch()) {
      const auto actions = touchActionLayout(renderer);
      const char* actionLabels[] = {tr(STR_NEARBY_STATS_SYNC_BUTTON), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
    } else {
      const auto labels =
          mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_NEARBY_STATS_SYNC_BUTTON), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!detailPrimary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8,
                              detailPrimary.c_str());
  }
  if (!detailSecondary.empty()) {
    renderer.drawCenteredText(
        SMALL_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 14,
        detailSecondary.c_str());
  }
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}

void NearbyStatsSyncActivity::renderReady(const std::string& primary, const std::string& detailPrimary,
                                          const std::string& detailSecondary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop =
      metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  const Rect textArea{metrics.contentSidePadding, 0, renderer.getScreenWidth() - metrics.contentSidePadding * 2,
                      renderer.getScreenHeight()};
  int y = contentTop + 70;

  y += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, primary.c_str(), 2, true,
                                        EpdFontFamily::BOLD) +
       metrics.verticalSpacing;
  if (!detailPrimary.empty()) {
    y += UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID, y, detailPrimary.c_str(), 3) +
         metrics.verticalSpacing;
  }
  if (!detailSecondary.empty()) {
    y += UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID, y, detailSecondary.c_str(), 2) +
         metrics.verticalSpacing;
  }
  if (state_ == State::READY) {
    UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID, y, tr(STR_NEARBY_STATS_READY_HINT), 2);
  }
}

#endif
