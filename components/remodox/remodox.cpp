#include "remodox.h"

#include "usages.h"

#include "esp_system.h"

#ifdef USE_ESP32

namespace esphome {
namespace remodox {

static const char *const TAG = "remodox";

// Every line carries the remote's address so that several remotes can be told apart.
#define LOGD(fmt, ...) ESP_LOGD(TAG, "[%s] " fmt, this->parent()->address_str(), ##__VA_ARGS__)
#define LOGI(fmt, ...) ESP_LOGI(TAG, "[%s] " fmt, this->parent()->address_str(), ##__VA_ARGS__)
#define LOGW(fmt, ...) ESP_LOGW(TAG, "[%s] " fmt, this->parent()->address_str(), ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(TAG, "[%s] " fmt, this->parent()->address_str(), ##__VA_ARGS__)

static const std::string EMPTY = "";

// Seconds since a millis() timestamp, -1 if the event never happened.
static int age_s(uint32_t now, uint32_t then) {
  return then == 0 ? -1 : (int) ((now - then) / 1000);
}

// HCI error / disconnect reasons that matter for this remote.
static const char *hci_reason_name(uint8_t r) {
  switch (r) {
    case 0x08: return "connection timeout (link lost / supervision timeout)";
    case 0x13: return "remote user terminated (the remote hung up)";
    case 0x14: return "remote low resources";
    case 0x15: return "remote power off";
    case 0x16: return "local host terminated (we hung up)";
    case 0x22: return "LMP/LL response timeout";
    case 0x28: return "instant passed";
    case 0x3B: return "unacceptable connection parameters";
    case 0x3D: return "MIC failure (bad / missing encryption key)";
    case 0x3E: return "failed to establish connection";
    default:   return "other";
  }
}

// Maximum connection latency we will accept/request.
// With max_int=16 (20 ms × 1.25), effective wake interval =
//   20 ms × (MAX_CONN_LATENCY + 1) = 200 ms.
// At a supervision timeout of 8 s that gives ≥ 40 wakeup opportunities,
// which is far more robust to WiFi coexistence than the remote's typical
// preferred latency of 29 (~600 ms wake, ~13 opportunities per timeout).
static constexpr uint16_t MAX_CONN_LATENCY = 9;

// Return a human-readable name for a HIDState value (used in log messages).
static const char *hid_state_name(HIDState s) {
  switch (s) {
    case HIDState::INIT:                      return "INIT";
    case HIDState::SETUP:                     return "SETUP";
    case HIDState::HID_SERVICE_FOUND:         return "HID_SERVICE_FOUND";
    case HIDState::NO_HID_SERVICE:            return "NO_HID_SERVICE";
    case HIDState::BLE_CONNECTED:             return "BLE_CONNECTED";
    case HIDState::READING_CHARS:             return "READING_CHARS";
    case HIDState::READ_CHARS:                return "READ_CHARS";
    case HIDState::CONFIGURING:               return "CONFIGURING";
    case HIDState::NOTIFICATIONS_REGISTERING: return "NOTIFICATIONS_REGISTERING";
    case HIDState::NOTIFICATIONS_REGISTERED:  return "NOTIFICATIONS_REGISTERED";
    case HIDState::CONN_PARAMS_UPDATING:      return "CONN_PARAMS_UPDATING";
    case HIDState::CONFIGURED:                return "CONFIGURED";
    default:                                  return "UNKNOWN";
  }
}

void RemodoX::log_stats_() {
  uint32_t now = millis();
  LOGI("[stats] hid=%s link=%d up=%ds notifies=%u keys=%u last_notify=%ds last_key=%ds "
       "keepalives=%u connections=%u heap=%u",
       hid_state_name(this->hid_state), (int) this->parent()->state(),
       this->hid_state == HIDState::INIT ? -1 : age_s(now, this->connect_ms_),
       (unsigned) this->notify_count_, (unsigned) this->key_count_,
       age_s(now, this->last_notify_ms_), age_s(now, this->last_key_ms_),
       (unsigned) this->keepalive_count_, (unsigned) this->connection_count_,
       (unsigned) esp_get_free_heap_size());
}

void RemodoX::loop() {
  uint32_t stats_now = millis();
  if (stats_now - this->last_stats_ms_ >= STATS_INTERVAL_MS) {
    this->last_stats_ms_ = stats_now;
    this->log_stats_();
  }
  switch (this->hid_state) {
    case HIDState::BLE_CONNECTED:
      // Set READING_CHARS first so the fast-path inside read_client_characteristics()
      // can immediately override it to READ_CHARS (reconnect with cached report map).
      this->hid_state = HIDState::READING_CHARS;
      this->read_client_characteristics();
      LOGI("[state] BLE_CONNECTED → %s", hid_state_name(this->hid_state));
      break;
    case HIDState::READ_CHARS:
      LOGD("[state] READ_CHARS → configuring HID client");
      this->configure_hid_client();
      // Advance to NOTIFICATIONS_REGISTERING.  If no notify handles were found
      // (or all register_for_notify calls failed), the counter is already 0 and
      // no REG_FOR_NOTIFY_EVT will ever fire — advance directly to
      // NOTIFICATIONS_REGISTERED to avoid a permanent deadlock.
      if (this->handles_waiting_for_notify_registration == 0) {
        LOGW("[state] no notify registrations pending after configure "
                 "— advancing directly to NOTIFICATIONS_REGISTERED");
        this->hid_state = HIDState::NOTIFICATIONS_REGISTERED;
      } else {
        LOGD("[state] NOTIFICATIONS_REGISTERING: "
                 "waiting for %d REG_FOR_NOTIFY_EVT callbacks",
                 this->handles_waiting_for_notify_registration);
        this->hid_state = HIDState::NOTIFICATIONS_REGISTERING;
      }
      break;
    case HIDState::NOTIFICATIONS_REGISTERED:
      // Update connection parameters per device's preference.  This is
      // fire-and-forget — we do NOT block on the GAP event because that
      // event may never fire (remote rejects update, event is for another
      // device, etc.).  Move to CONFIGURED immediately so the keepalive
      // loop runs regardless.
      if (this->preferred_conn_params.max_int > 0) {
        esp_ble_conn_update_params_t p = this->preferred_conn_params;
        // Always refresh BDA to the current connection's remote address.
        memcpy(p.bda, this->parent()->get_remote_bda(), ESP_BD_ADDR_LEN);
        // Override supervision timeout to at least 8 s.  The remote asks for
        // 3 s; on an ESP32 sharing the 2.4 GHz radio with WiFi even a short
        // WiFi burst can cause consecutive BLE events to be missed.
        if (p.timeout < 800) p.timeout = 800;
        if (p.latency > MAX_CONN_LATENCY) p.latency = MAX_CONN_LATENCY;
        LOGI("[state] NOTIFICATIONS_REGISTERED → CONFIGURED: "
                 "requesting conn params interval=%u-%u latency=%u timeout=%u",
                 p.min_int, p.max_int, p.latency, p.timeout);
        esp_ble_gap_update_conn_params(&p);
      } else {
        LOGD("[state] NOTIFICATIONS_REGISTERED → CONFIGURED: "
                 "no preferred conn params, keeping current");
      }
      this->hid_state = HIDState::CONFIGURED;
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->last_keepalive_ms_ = millis();
      LOGI("[state] CONFIGURED — device ready. "
               "hid_cp=0x%04X  preferred_latency=%u",
               this->hid_control_point_handle_,
               this->preferred_conn_params.latency);
      break;
    case HIDState::CONFIGURED: {
      // Periodic keepalive to prevent the remote from self-disconnecting after
      // its application-level idle timeout (HCI reason 0x13).
      //
      // Strategy:
      //  PRIMARY: Write HID Control Point Exit Suspend (0x01) then immediately
      //  Suspend (0x00).  This uses Write Without Response — the remote does NOT
      //  need to send an ATT response, so there is NO pending GATT state left in
      //  the remote's stack if the connection drops mid-keepalive.  A pending
      //  GATT-read response (left over from GATT read keepalives) was the root
      //  cause of the remote crashing on reconnect — the remote's GATT stack was
      //  stuck in a "response queued" state when a new connection arrived.
      //
      //  Writing Exit Suspend resets the remote's idle timer.  Immediately
      //  following with Suspend puts the device back into suspend mode so that
      //  the next button press causes the expected LED flash (the LED fires when
      //  the device wakes from suspend to deliver the report).
      //
      //  FALLBACK: connection-parameter update (L2CAP only — last resort, does
      //  NOT reset idle timer but may help with supervision timeout survival).
      uint32_t now = millis();
      if ((now - this->last_keepalive_ms_) >= KEEPALIVE_INTERVAL_MS) {
        this->last_keepalive_ms_ = now;
        // Alert Level 0 = "no alert": no visible effect in Control Mode, but it reaches
        // the remote's GATT callback, which restarts its 5 min idle timer.
        this->keepalive_count_++;
        LOGI("[keepalive] #%u, connected %d s, last key %d s ago", (unsigned) this->keepalive_count_,
             age_s(now, this->connect_ms_), age_s(now, this->last_key_ms_));
        if (this->ias_alert_level_handle_ != 0) {
          this->write_alert_level_(0, "keepalive");
        }
        if (this->hid_control_point_handle_ != 0) {
          // Step 1: Exit Suspend — signals host is active, resets idle timer.
          uint8_t hid_exit_suspend_cmd = 0x01;
          LOGI("Keepalive: writing Exit Suspend (0x01) to HID CP 0x%04X",
                   this->hid_control_point_handle_);
          esp_err_t err = esp_ble_gattc_write_char(
              this->parent()->get_gattc_if(),
              this->parent()->get_conn_id(),
              this->hid_control_point_handle_,
              sizeof(hid_exit_suspend_cmd), &hid_exit_suspend_cmd,
              ESP_GATT_WRITE_TYPE_NO_RSP,
              ESP_GATT_AUTH_REQ_NONE);
          if (err != ESP_OK) {
            LOGW("Keepalive: Exit Suspend write failed (err=0x%x)", err);
          }
          // Step 2: Suspend — returns device to suspend mode so the next button
          // press triggers the remote's LED feedback.  Also Write Without
          // Response, so no pending state if the connection drops between here
          // and the next keepalive.
          uint8_t hid_suspend_cmd = 0x00;
          LOGD("Keepalive: writing Suspend (0x00) to HID CP 0x%04X "
                   "(restores suspend mode → LED will flash on next button press)",
                   this->hid_control_point_handle_);
          err = esp_ble_gattc_write_char(
              this->parent()->get_gattc_if(),
              this->parent()->get_conn_id(),
              this->hid_control_point_handle_,
              sizeof(hid_suspend_cmd), &hid_suspend_cmd,
              ESP_GATT_WRITE_TYPE_NO_RSP,
              ESP_GATT_AUTH_REQ_NONE);
          if (err != ESP_OK) {
            LOGW("Keepalive: Suspend write failed (err=0x%x)", err);
          }
        } else {
          // No HID Control Point found — attempt a conn-param update to
          // generate some link-layer traffic.  This does NOT reset the remote's
          // application-level idle timer but may help keep the supervision
          // timeout from firing.
          if (this->preferred_conn_params.max_int > 0) {
            esp_ble_conn_update_params_t p = this->preferred_conn_params;
            if (p.timeout < 800) p.timeout = 800;
            if (p.latency > MAX_CONN_LATENCY) p.latency = MAX_CONN_LATENCY;
            LOGW("Keepalive: no HID CP — conn param update only "
                     "(idle disconnect after 5 min may still occur)");
            esp_ble_gap_update_conn_params(&p);
          } else {
            LOGW("Keepalive: no mechanism available — "
                     "idle disconnect after 5 min of inactivity is expected");
          }
        }
      }
      break;
    }
    case HIDState::NO_HID_SERVICE:
      // Connected to a sleeping remote (no HID service).  It ignores keys while a host
      // is connected, so wake it once per connection with Alert Level 0.
      if (!this->wake_attempted_) {
        this->wake_attempted_ = true;
        this->find_ias_handle_();
        if (this->ias_alert_level_handle_ != 0) {
          this->write_alert_level_(0, "wake sleeping remote (no HID service)");
          LOGI("[wake] sent; if the remote was asleep it resets within ~1 s, drops this "
               "link and re-advertises as Remodo_X_* with the HID service");
        } else {
          LOGW("No HID and no Immediate Alert service: cannot wake the remote");
        }
      }
      break;
    default:
      break;
  }
}

void RemodoX::find_ias_handle_() {
  this->ias_alert_level_handle_ = 0;
  auto *svc = this->parent()->get_service(0x1802);
  if (svc == nullptr) return;
  auto *chr = svc->get_characteristic(0x2A06);
  if (chr != nullptr && (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
    this->ias_alert_level_handle_ = chr->handle;
  }
}

void RemodoX::write_alert_level_(uint8_t level, const char *why) {
  esp_err_t err = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
      this->ias_alert_level_handle_, sizeof(level), &level,
      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  LOGI("IAS Alert Level=%u (%s) handle=0x%04X -> %s", (unsigned) level, why,
           this->ias_alert_level_handle_, err == ESP_OK ? "sent" : "FAILED");
}

void RemodoX::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Client HID:");
  ESP_LOGCONFIG(TAG, "  MAC address        : %s",
                this->parent()->address_str());
}

void RemodoX::gap_event_handler(esp_gap_ble_cb_event_t event,
  esp_ble_gap_cb_param_t *param) {
   switch (event)
   {
   case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
    // Validate BDA so we don't react to updates for other connected devices.
    if (memcmp(param->update_conn_params.bda, this->parent()->get_remote_bda(), 6) != 0)
      break;
    LOGI("[gap] conn params updated (status=%d, %d s after connect): interval=%.2f ms, latency=%u, timeout=%.1f ms",
         (int) param->update_conn_params.status, age_s(millis(), this->connect_ms_),
         param->update_conn_params.conn_int * 1.25f, param->update_conn_params.latency,
         param->update_conn_params.timeout * 10.f);
     break;
   case ESP_GAP_BLE_SEC_REQ_EVT:
    if (memcmp(param->ble_security.ble_req.bd_addr, this->parent()->get_remote_bda(), 6) != 0)
      break;
    LOGI("[gap] security request from the remote (%d s after connect)", age_s(millis(), this->connect_ms_));
    break;
   case ESP_GAP_BLE_AUTH_CMPL_EVT:
    if (memcmp(param->ble_security.auth_cmpl.bd_addr, this->parent()->get_remote_bda(), 6) != 0)
      break;
    if (param->ble_security.auth_cmpl.success) {
      LOGI("[gap] pairing/encryption OK (%d s after connect), auth_mode=%d key_present=%d",
           age_s(millis(), this->connect_ms_), (int) param->ble_security.auth_cmpl.auth_mode,
           (int) param->ble_security.auth_cmpl.key_present);
    } else {
      LOGW("[gap] pairing/encryption FAILED, reason=0x%02X (esp_ble_auth_fail_rsn_t)",
           (unsigned) param->ble_security.auth_cmpl.fail_reason);
    }
    break;
   default:
     break;
   }
  }

void RemodoX::read_client_characteristics() {
  // Reconnect fast-path: if we already have the report map from a prior
  // connection, skip ALL GATT reads.  The report map, handle→report_id
  // mappings and preferred conn params are all static device properties that
  // do not change between connections.  Issuing GATT reads (which require ATT
  // Read Responses from the remote) can leave the remote's GATT stack in an
  // inconsistent state if the connection drops mid-read — causing the remote to
  // crash and require a battery pull on the next reconnect.  Skipping reads on
  // reconnect eliminates this risk entirely.
  if (this->hid_report_map != nullptr) {
    LOGI("[setup] Reconnect: using cached report map — "
             "skipping all GATT reads, advancing directly to READ_CHARS");
    this->hid_state = HIDState::READ_CHARS;
    return;
  }

  LOGD("[setup] First connection: scheduling GATT reads");
  using namespace ble_client;
  BLEService *hid_service = this->parent()->get_service(ESP_GATT_UUID_HID_SVC);
  BLEService *generic_access_service = this->parent()->get_service(0x1800);

  // === GATT Service Discovery Dump ===
  LOGD("=== GATT Services ===");
  struct { BLEService *svc; const char *name; } services[] = {
    {generic_access_service, "GenericAccess(0x1800)"},
    {hid_service,            "HID(0x1812)"},
  };
  for (auto &entry : services) {
    if (entry.svc == nullptr) {
      LOGD("  Service %s: not present", entry.name);
      continue;
    }
    LOGD("  Service %s: %d characteristics", entry.name,
             entry.svc->characteristics.size());
    for (auto *chr : entry.svc->characteristics) {
      LOGD("    Char UUID=0x%04X handle=0x%04X props=0x%02X",
               chr->uuid.get_uuid().uuid.uuid16, chr->handle, chr->properties);
      BLEDescriptor *rpt_ref = chr->get_descriptor(ESP_GATT_UUID_RPT_REF_DESCR);
      if (rpt_ref != nullptr) {
        LOGD("      RPT_REF_DESCR handle=0x%04X", rpt_ref->handle);
      }
    }
  }

  // Only read preferred connection parameters from Generic Access (0x1800).
  // The Remodo X only advertises HID (0x1812); device info and device name
  // are not present and would cause unnecessary failed GATT reads.
  if (generic_access_service != nullptr) {
    BLECharacteristic *pref_conn_params_char =
        generic_access_service->get_characteristic(ESP_GATT_UUID_GAP_PREF_CONN_PARAM);
    this->schedule_read_char(pref_conn_params_char, "GAP_PREF_CONN_PARAM");
  }
  if (hid_service != nullptr) {
    BLECharacteristic *hid_report_map_char =
        hid_service->get_characteristic(ESP_GATT_UUID_HID_REPORT_MAP);
    this->schedule_read_char(hid_report_map_char, "HID_REPORT_MAP");
    LOGD("Found %d characteristics",
             hid_service->characteristics.size());
    for (auto *chr : hid_service->characteristics) {
      if (chr->uuid.get_uuid().uuid.uuid16 != ESP_GATT_UUID_HID_REPORT) {
        continue;
      }

      bool has_notify = (chr->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0;
      BLEDescriptor *rpt_ref_desc =
          chr->get_descriptor(ESP_GATT_UUID_RPT_REF_DESCR);
      LOGD("  HID_REPORT char handle=0x%04X props=0x%02X notify=%s rpt_ref=%s",
               chr->handle, chr->properties,
               has_notify ? "yes" : "no",
               rpt_ref_desc != nullptr ? "yes" : "no");

      if (rpt_ref_desc != nullptr) {
        if (esp_ble_gattc_read_char_descr(
                this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                rpt_ref_desc->handle, ESP_GATT_AUTH_REQ_NO_MITM) != ESP_OK) {
          LOGW("scheduling reading of RPT_REF_DESCR failed for handle 0x%04X.",
                   chr->handle);
        }
        this->handles_to_read.insert(
            std::make_pair(rpt_ref_desc->handle, nullptr));
      }
    }
  }
}
void RemodoX::on_gatt_read_finished(GATTReadData *data) {
  std::map<uint16_t, GATTReadData *>::iterator itr;
  itr = this->handles_to_read.find(data->handle_);
  // Guard: if this handle is not in the tracked setup-read map (e.g. a
  // keepalive read that fired after fully CONFIGURED), discard the data and
  // return without touching the state machine.
  if (itr == this->handles_to_read.end()) {
    LOGD("on_gatt_read_finished: handle=0x%04X len=%d — "
             "not a setup read (keepalive or unknown); discarding",
             data->handle_, data->value_len_);
    delete data;
    return;
  }
  LOGD("on_gatt_read_finished: handle=0x%04X len=%d — "
           "setup read completed (%zu/%zu total)",
           data->handle_, data->value_len_,
           std::count_if(this->handles_to_read.begin(),
                         this->handles_to_read.end(),
                         [](const std::pair<uint16_t, GATTReadData *> &kv) {
                           return kv.second != nullptr;
                         }) + 1,
           this->handles_to_read.size());
  itr->second = data;
  // check if all handles have been read:
  for (auto const &element : this->handles_to_read) {
    if (element.second == nullptr) {
      return;
    }
  }
  LOGD("on_gatt_read_finished: all %zu setup reads done → READ_CHARS",
           this->handles_to_read.size());
  this->hid_state = HIDState::READ_CHARS;
}

void RemodoX::gattc_event_handler(esp_gattc_cb_event_t event,
                                       esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  esp_ble_gattc_cb_param_t *p_data = param;
  switch (event) {
    case ESP_GATTC_CONNECT_EVT: {
      uint32_t now = millis();
      this->connection_count_++;
      LOGI("[gattc] CONNECT_EVT #%u (%d s after the last disconnect, cached report map: %s) — initiating encryption",
           (unsigned) this->connection_count_, age_s(now, this->disconnect_ms_),
           this->hid_report_map != nullptr ? "yes" : "no");
      this->connect_ms_ = now;
      this->notify_count_ = 0;
      this->key_count_ = 0;
      this->keepalive_count_ = 0;
      this->last_notify_ms_ = 0;
      this->last_key_ms_ = 0;
      auto ret = esp_ble_set_encryption(param->connect.remote_bda,
                                        ESP_BLE_SEC_ENCRYPT);
      if (ret) {
        LOGE("[gattc] esp_ble_set_encryption error, status=%d", ret);
      }
      esp_gap_conn_params_t params;
      ret = esp_ble_get_current_conn_params(
          this->parent()->get_remote_bda(), &params);
      if (ret != ESP_OK) {
        LOGW("[gattc] CONNECT_EVT: could not read current conn params");
      } else {
        LOGI("[gattc] CONNECT_EVT: initial conn params "
                 "interval=%u (%.1f ms) latency=%u timeout=%u (%.1f ms)",
                 params.interval, params.interval * 1.25f,
                 params.latency, params.timeout, params.timeout * 10.0f);
      }
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      {
        uint32_t now = millis();
        int idle_s = age_s(now, this->last_key_ms_ != 0 ? this->last_key_ms_ : this->connect_ms_);
        LOGW("[gattc] DISCONNECT_EVT: reason=0x%02X (%s) — connected %d s, %u notifies, %u keys, "
             "last key %d s ago, state=%s",
             (unsigned) param->disconnect.reason, hci_reason_name(param->disconnect.reason),
             age_s(now, this->connect_ms_), (unsigned) this->notify_count_, (unsigned) this->key_count_,
             age_s(now, this->last_key_ms_), hid_state_name(this->hid_state));
        if (param->disconnect.reason == 0x13 && idle_s >= 290) {
          LOGW("[gattc] ... %d s without a key: looks like the remote's 5 min idle timeout, "
               "it now resets and sleeps (no HID service, advertises Find_Remodo_X_*)", idle_s);
        }
        this->disconnect_ms_ = now;
      }
      this->status_set_warning("Disconnected");
      // Reset all HID state so reconnection starts cleanly.
      HIDState prev = this->hid_state;
      this->hid_state = HIDState::INIT;
      this->handles_waiting_for_notify_registration = 0;
      this->hid_control_point_handle_ = 0;
      this->ias_alert_level_handle_ = 0;
      this->wake_attempted_ = false;
      this->last_keepalive_ms_ = 0;
      for (auto &kv : this->handles_to_read) {
        delete kv.second;
      }
      this->handles_to_read.clear();
      this->handles_registered_for_notify.clear();
      // Keep hid_report_map and handle_report_id across disconnects so that
      // reconnects can skip all GATT reads.  Every GATT read (HID_REPORT_MAP,
      // RPT_REF_DESCR, GAP_PREF_CONN_PARAM) requires an ATT Read Response from
      // the remote.  If the connection drops while a read is in-flight the
      // remote's GATT stack is left with a queued-but-never-delivered response.
      // When a new connection arrives and new GATT operations start the remote
      // hits this inconsistent state and crashes — requiring a battery pull.
      // The report map and handle mappings are static properties of the device
      // and never change, so caching them is correct.
      if (this->hid_report_map != nullptr) {
        LOGI("[gattc] DISCONNECT_EVT: keeping cached report map + "
                 "%zu handle→report_id mappings for reconnect (no GATT reads on reconnect)",
                 this->handle_report_id.size());
      }
      LOGD("[gattc] DISCONNECT_EVT: was in state=%s, reset to INIT",
               hid_state_name(prev));
      break;
    }
    case ESP_GATTC_SEARCH_RES_EVT: {
      if (p_data->search_res.srvc_id.uuid.uuid.uuid16 ==
          ESP_GATT_UUID_HID_SVC) {
        this->hid_state = HIDState::HID_SERVICE_FOUND;
        LOGI("[gattc] SEARCH_RES_EVT: HID service (0x1812) found");
      } else {
        LOGI("[gattc] SEARCH_RES_EVT: service UUID=0x%04X",
                 p_data->search_res.srvc_id.uuid.uuid.uuid16);
      }
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (this->hid_state != HIDState::HID_SERVICE_FOUND) {
        LOGW("[gattc] SEARCH_CMPL_EVT: no HID service (%d s after connect) — the remote is "
             "asleep or in Setup Mode", age_s(millis(), this->connect_ms_));
        this->hid_state = HIDState::NO_HID_SERVICE;
        this->status_set_warning("Invalid device config");
        break;
      }
      LOGD("[gattc] SEARCH_CMPL_EVT: status=%d → BLE_CONNECTED",
               p_data->search_cmpl.status);
      this->hid_state = HIDState::BLE_CONNECTED;
      esp_gap_conn_params_t params;
      esp_err_t ret = esp_ble_get_current_conn_params(
          this->parent()->get_remote_bda(), &params);
      if (ret != ESP_OK) {
        LOGW("[gattc] SEARCH_CMPL_EVT: could not read current conn params");
      } else {
        LOGI("[gattc] SEARCH_CMPL_EVT: conn params after service discovery "
                 "interval=%u (%.1f ms) latency=%u timeout=%u (%.1f ms)",
                 params.interval, params.interval * 1.25f,
                 params.latency, params.timeout, params.timeout * 10.0f);
      }
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT:
    case ESP_GATTC_READ_DESCR_EVT: {
      if (param->read.conn_id != this->parent()->get_conn_id()) break;
      if (param->read.status != ESP_OK) {
        LOGW("[gattc] READ_%s_EVT: handle=0x%04X FAILED status=0x%02X",
                 event == ESP_GATTC_READ_CHAR_EVT ? "CHAR" : "DESCR",
                 param->read.handle, param->read.status);
        // If this was a pending setup read, erase it from the map so the state
        // machine is not permanently stuck in READING_CHARS.  A failed read
        // means we simply won't have that data (e.g. conn params), which is
        // handled gracefully by the configure step.
        auto it = this->handles_to_read.find(param->read.handle);
        if (it != this->handles_to_read.end()) {
          delete it->second;  // it->second may be nullptr if read was never fulfilled; delete nullptr is safe in C++
          this->handles_to_read.erase(it);
          // Check whether all remaining setup reads are now accounted for.
          bool any_pending = false;
          for (auto const &kv : this->handles_to_read) {
            if (kv.second == nullptr) { any_pending = true; break; }
          }
          if (!any_pending) {
            LOGW("[gattc] READ_EVT: all setup reads accounted for "
                     "(some failed) → READ_CHARS");
            this->hid_state = HIDState::READ_CHARS;
          }
        }
        break;
      }
      LOGD("[gattc] READ_%s_EVT: handle=0x%04X len=%d OK",
               event == ESP_GATTC_READ_CHAR_EVT ? "CHAR" : "DESCR",
               param->read.handle, param->read.value_len);
      GATTReadData *data = new GATTReadData(
          param->read.handle, param->read.value, param->read.value_len);
      this->on_gatt_read_finished(data);
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.conn_id != this->parent()->get_conn_id()) break;
      if (param->write.status != ESP_GATT_OK) {
        LOGW("[gattc] WRITE_CHAR_EVT: handle=0x%04X FAILED status=0x%02X",
                 param->write.handle, param->write.status);
      } else {
        LOGD("[gattc] WRITE_CHAR_EVT: handle=0x%04X OK",
                 param->write.handle);
      }
      break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.conn_id != this->parent()->get_conn_id()) break;
      LOGI("[gattc] WRITE_DESCR_EVT (CCCD): handle=0x%04X status=0x%02X %s",
           param->write.handle, (unsigned) param->write.status,
           param->write.status == ESP_GATT_OK ? "OK, notifications enabled" : "FAILED, key presses will be ignored by the remote");
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->parent()->get_conn_id()) break;
      {
        uint32_t now = millis();
        this->notify_count_++;
        LOGI("[notify] #%u handle=0x%04X len=%d state=%s (%d s since the previous notify, %d s after connect)",
             (unsigned) this->notify_count_, p_data->notify.handle, p_data->notify.value_len,
             hid_state_name(this->hid_state), age_s(now, this->last_notify_ms_),
             age_s(now, this->connect_ms_));
        this->last_notify_ms_ = now;
      }
      this->send_input_report_event(p_data);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        LOGW("[gattc] REG_FOR_NOTIFY_EVT: handle=0x%04X FAILED status=0x%02X",
                 param->reg_for_notify.handle, param->reg_for_notify.status);
      } else {
        LOGD("[gattc] REG_FOR_NOTIFY_EVT: handle=0x%04X OK",
                 param->reg_for_notify.handle);
      }
      // Guard against uint8_t underflow caused by stale callbacks firing after
      // a disconnect (when the counter has already been reset to 0).
      if (this->handles_waiting_for_notify_registration == 0) {
        LOGW("[gattc] REG_FOR_NOTIFY_EVT: spurious event "
                 "(counter already 0, state=%s) — ignoring",
                 hid_state_name(this->hid_state));
        break;
      }
      this->handles_waiting_for_notify_registration--;
      LOGD("[gattc] REG_FOR_NOTIFY_EVT: handle=0x%04X remaining=%d",
               param->reg_for_notify.handle,
               this->handles_waiting_for_notify_registration);
      // Only advance to NOTIFICATIONS_REGISTERED from the correct state.
      if (this->handles_waiting_for_notify_registration == 0 &&
          this->hid_state == HIDState::NOTIFICATIONS_REGISTERING) {
        LOGD("[gattc] REG_FOR_NOTIFY_EVT: all notifications registered → "
                 "NOTIFICATIONS_REGISTERED");
        this->hid_state = HIDState::NOTIFICATIONS_REGISTERED;
      }
      break;
    }
    default: {
      break;
    }
  }
}

void RemodoX::send_input_report_event(esp_ble_gattc_cb_param_t *p_data) {
  LOGD("[report] notify arrived: handle=0x%04X len=%d state=%s",
           p_data->notify.handle, p_data->notify.value_len,
           hid_state_name(this->hid_state));

  if (this->hid_report_map == nullptr) {
    LOGW("[report] report map not ready (state=%s) — dropping notify "
             "handle=0x%04X len=%d",
             hid_state_name(this->hid_state),
             p_data->notify.handle, p_data->notify.value_len);
    return;
  }
  // Determine report_id from our mapping (avoid default-entry creation via [])
  uint8_t report_id = 0;
  if (this->handle_report_id.count(p_data->notify.handle) > 0) {
    report_id = this->handle_report_id.at(p_data->notify.handle);
    LOGD("[report] handle=0x%04X → report_id=%d",
             p_data->notify.handle, report_id);
  } else {
    LOGW("[report] no report_id mapping for handle=0x%04X "
             "(known mappings: %zu) — defaulting to 0",
             p_data->notify.handle, this->handle_report_id.size());
  }

  // Log raw notify payload in hex for diagnostics
  {
    std::string hex;
    hex.reserve(p_data->notify.value_len * 3);
    char byte_str[5];
    for (uint16_t i = 0; i < p_data->notify.value_len; i++) {
      snprintf(byte_str, sizeof(byte_str), "%02X ", p_data->notify.value[i]);
      hex += byte_str;
    }
    LOGD("[report] raw: handle=0x%04X report_id=%d len=%d data=[%s]",
             p_data->notify.handle, report_id,
             p_data->notify.value_len, hex.c_str());
  }

  uint8_t *data = new uint8_t[p_data->notify.value_len + 1];
  memcpy(data + 1, p_data->notify.value, p_data->notify.value_len);
  data[0] = report_id;
  std::vector<HIDReportItemValue> hid_report_values =
      this->hid_report_map->parse(data);
  if (hid_report_values.size() == 0) {
    LOGD("[report] parser returned 0 values for report_id=%d "
             "(all-zeros / no change report)", report_id);
    delete[] data;
    return;
  }
  LOGD("[report] parser returned %zu value(s) for report_id=%d",
           hid_report_values.size(), report_id);
  for (HIDReportItemValue value : hid_report_values) {
    std::string usage;
    if (USAGE_PAGES.count(value.usage.page) > 0 &&
        USAGE_PAGES.at(value.usage.page).usages_.count(value.usage.usage) > 0) {
      usage = USAGE_PAGES.at(value.usage.page).usages_.at(value.usage.usage);
      LOGD("[report] HID event: page=0x%02X usage=0x%04X value=%d name='%s'",
               value.usage.page, value.usage.usage, (int) value.value, usage.c_str());
    } else {
      usage = std::to_string(value.usage.page) + "_" +
              std::to_string(value.usage.usage);
      LOGD("[report] HID event: page=0x%02X usage=0x%04X value=%d "
               "(no name, fallback='%s')",
               value.usage.page, value.usage.usage, (int) value.value, usage.c_str());
    }
    {
      uint32_t now = millis();
      this->key_count_++;
      LOGI("[report] >>> KEY #%u usage='%s' code=%u_%u value=%d (%d s after connect, %d s since the previous key)",
           (unsigned) this->key_count_, usage.c_str(), (unsigned) value.usage.page,
           (unsigned) value.usage.usage, (int) value.value, age_s(now, this->connect_ms_),
           age_s(now, this->last_key_ms_));
      this->last_key_ms_ = now;
    }
    #ifdef USE_API_HOMEASSISTANT_SERVICES
    this->fire_homeassistant_event("esphome.hid_events",
        {{"usage", usage}, {"value", std::to_string(value.value)},
         {"address", this->parent()->address_str()}});
    #endif
    // Publish value first: on_value of the code sensor reads last_event_value.state,
    // which must already belong to this event.
    if (this->last_event_value_sensor != nullptr) {
      this->last_event_value_sensor->publish_state(value.value);
    }
    if (this->last_event_usage_text_sensor != nullptr) {
      this->last_event_usage_text_sensor->publish_state(usage);
    }
    if (this->last_event_code_text_sensor != nullptr) {
      std::string event_code = std::to_string(value.usage.page) + "_" +
                               std::to_string(value.usage.usage);
      this->last_event_code_text_sensor->publish_state(event_code);
    }
  }

  delete[] data;
}

void RemodoX::register_last_event_value_sensor(
    sensor::Sensor *last_event_value_sensor) {
  this->last_event_value_sensor = last_event_value_sensor;
}

void RemodoX::register_last_event_usage_text_sensor(
    text_sensor::TextSensor *last_event_usage_text_sensor) {
  this->last_event_usage_text_sensor = last_event_usage_text_sensor;
}

void RemodoX::register_last_event_code_text_sensor(
    text_sensor::TextSensor *last_event_code_text_sensor) {
  this->last_event_code_text_sensor = last_event_code_text_sensor;
}

void RemodoX::schedule_read_char(
    ble_client::BLECharacteristic *characteristic, const char *name) {
  if (characteristic == nullptr) {
    LOGW("Characteristic '%s': not found on device (nullptr)", name);
    return;
  }
  if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_READ) == 0) {
    ESP_LOGW(TAG,
             "Characteristic '%s' (handle=0x%04X): exists but READ property "
             "not set (properties=0x%02X)",
             name, characteristic->handle, characteristic->properties);
    return;
  }
  if (esp_ble_gattc_read_char(
          this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
          characteristic->handle, ESP_GATT_AUTH_REQ_NO_MITM) != ESP_OK) {
    LOGW("read_char failed for '%s' (handle=0x%04X)",
             name, characteristic->handle);
  }
  this->handles_to_read.insert(
      std::make_pair(characteristic->handle, nullptr));
}

uint8_t *RemodoX::parse_characteristic_data(
    ble_client::BLEService *service, uint16_t uuid) {
  using namespace ble_client;
  BLECharacteristic *characteristic = service->get_characteristic(uuid);
  if (characteristic == nullptr) {
    LOGD("No characteristic with uuid %#X found on device", uuid);
    return nullptr;
  }
  if (handles_to_read.count(characteristic->handle) >= 1 &&
      handles_to_read.at(characteristic->handle) != nullptr) {
    ESP_LOGD(
        TAG,
        "Characteristic parsed for uuid %#X and handle %#X starts with %#X",
        uuid, characteristic->handle,
        *(handles_to_read.at(characteristic->handle)->value_));
    return handles_to_read.at(characteristic->handle)->value_;
  }
  ESP_LOGD(TAG,
           "Characteristic with uuid %#X and handle %#X not stored in "
           "handles_to_read",
           uuid, characteristic->handle);
  return nullptr;
}

void RemodoX::configure_hid_client() {
  using namespace ble_client;
  LOGD("[configure] configure_hid_client() start");
  BLEService *hid_service = this->parent()->get_service(ESP_GATT_UUID_HID_SVC);
  BLEService *generic_access_service = this->parent()->get_service(0x1800);
  int notify_chars = 0;  // count of notify-capable HID_REPORT handles found

  if (hid_service != nullptr) {
    BLECharacteristic *hid_report_map_char =
        hid_service->get_characteristic(ESP_GATT_UUID_HID_REPORT_MAP);
    if (hid_report_map_char != nullptr &&
        this->handles_to_read.count(hid_report_map_char->handle) > 0 &&
        this->handles_to_read.at(hid_report_map_char->handle) != nullptr) {
      LOGD("[configure] parsing HID Report Map");
      HIDReportMap::esp_logd_report_map(
          this->handles_to_read.at(hid_report_map_char->handle)->value_,
          this->handles_to_read.at(hid_report_map_char->handle)->value_len_);
      delete this->hid_report_map;
      this->hid_report_map = HIDReportMap::parse_report_map_data(
          this->handles_to_read.at(hid_report_map_char->handle)->value_,
          this->handles_to_read.at(hid_report_map_char->handle)->value_len_);
      LOGD("[configure] HID Report Map parsed OK");
    } else if (this->hid_report_map != nullptr) {
      // Reconnect: report map cached from previous connection — no GATT read needed.
      LOGI("[configure] Using cached HID report map (reconnect)");
    } else {
      LOGW("[configure] HID_REPORT_MAP not available and no cached map — "
               "button presses will be dropped");
    }
    std::vector<BLECharacteristic *> chars = hid_service->characteristics;
    LOGD("[configure] HID service has %zu characteristics", chars.size());
    // Count notify-capable HID_REPORT chars before registering so the log shows
    // expected vs actual registrations.
    for (BLECharacteristic *c : chars) {
      if (c->uuid.get_uuid().uuid.uuid16 == ESP_GATT_UUID_HID_REPORT &&
          (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0)
        notify_chars++;
    }
    LOGI("[configure] %d HID_REPORT notify handle(s) to register "
             "(reconnect=%s — CCCDs must be re-applied per connection even "
             "when report map is cached, because CCCD values are per-connection "
             "link-layer state that resets to disabled when the peripheral disconnects)",
             notify_chars, this->hid_report_map != nullptr ? "yes" : "no");
    for (BLECharacteristic *hid_char : chars) {
      if (hid_char->uuid.get_uuid().uuid.uuid16 == ESP_GATT_UUID_HID_REPORT) {
        if (hid_char->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
          LOGD("[configure] HID_REPORT handle=0x%04X: registering for notify",
                   hid_char->handle);
          auto status = esp_ble_gattc_register_for_notify(
              this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
              hid_char->handle);
          if (status != ESP_OK) {
            LOGW("[configure] register for notify failed "
                     "handle=0x%04X status=%d", hid_char->handle, status);
          } else {
            this->handles_waiting_for_notify_registration++;
            LOGD("[configure] registered for HID notify "
                     "handle=0x%04X (pending=%d)",
                     hid_char->handle,
                     this->handles_waiting_for_notify_registration);
          }
        }
        BLEDescriptor *rpt_ref_desc =
            hid_char->get_descriptor(ESP_GATT_UUID_RPT_REF_DESCR);
        if (rpt_ref_desc != nullptr &&
            this->handles_to_read.count(rpt_ref_desc->handle) > 0 &&
            this->handles_to_read.at(rpt_ref_desc->handle) != nullptr) {
          uint8_t report_id = this->handles_to_read.at(rpt_ref_desc->handle)->value_[0];
          uint8_t report_type = this->handles_to_read.at(rpt_ref_desc->handle)->value_len_ > 1
                                    ? this->handles_to_read.at(rpt_ref_desc->handle)->value_[1]
                                    : 0;
          handle_report_id.insert(std::make_pair(hid_char->handle, report_id));
          LOGD("[configure] HID_REPORT handle=0x%04X: "
                   "report_id=%d report_type=%d",
                   hid_char->handle, report_id, report_type);
        } else if (this->handle_report_id.count(hid_char->handle) > 0) {
          // Reconnect: use cached report_id mapping — no GATT descriptor read needed.
          LOGD("[configure] HID_REPORT handle=0x%04X: "
                   "using cached report_id=%d",
                   hid_char->handle, this->handle_report_id.at(hid_char->handle));
        } else {
          LOGD("[configure] HID_REPORT handle=0x%04X: "
                   "no RPT_REF_DESCR data — report_id defaults to 0",
                   hid_char->handle);
        }
      }
    }
    // Look for HID Control Point — PRIMARY keepalive target.
    // Write Without Response (no ATT acknowledgement needed from remote), so
    // if the connection drops while a keepalive write is in flight there is NO
    // pending response state left in the remote's GATT stack.  This is the key
    // advantage over GATT reads: a pending GATT read-response that is never
    // sent (because the connection dropped) leaves the remote in a confused
    // state and can cause it to crash on reconnect.
    for (BLECharacteristic *hid_char : chars) {
      if (hid_char->uuid.get_uuid().uuid.uuid16 == ESP_GATT_UUID_HID_CONTROL_POINT &&
          (hid_char->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
        this->hid_control_point_handle_ = hid_char->handle;
        LOGI("[configure] HID Control Point handle=0x%04X found "
                 "(primary keepalive: Exit Suspend + Suspend cycle every %u ms)",
                 this->hid_control_point_handle_, (unsigned) KEEPALIVE_INTERVAL_MS);
        break;
      }
    }
    if (this->hid_control_point_handle_ == 0) {
      LOGW("[configure] HID Control Point not found — "
               "keepalive will fall back to conn param update (idle timeout may still occur)");
    }
  }
  this->find_ias_handle_();
  if (this->ias_alert_level_handle_ != 0) {
    LOGI("[configure] Immediate Alert Level handle=0x%04X (keepalive / wake)",
             this->ias_alert_level_handle_);
  } else {
    LOGW("[configure] Immediate Alert Level not found — IAS keepalive disabled");
  }
  if (this->hid_control_point_handle_ == 0 && this->ias_alert_level_handle_ == 0) {
    LOGW("[configure] no keepalive mechanism found — "
             "device may self-disconnect after idle timeout");
  } else {
    LOGI("[configure] keepalive every %u ms: IAS handle=0x%04X, HID CP handle=0x%04X",
             (unsigned) KEEPALIVE_INTERVAL_MS, this->ias_alert_level_handle_,
             this->hid_control_point_handle_);
  }
  if (generic_access_service != nullptr) {
    uint8_t *t_conn_params = this->parse_characteristic_data(
        generic_access_service, ESP_GATT_UUID_GAP_PREF_CONN_PARAM);
    if (t_conn_params != nullptr) {
      this->preferred_conn_params.min_int = t_conn_params[0] | (t_conn_params[1] << 8);
      this->preferred_conn_params.max_int = t_conn_params[2] | (t_conn_params[3] << 8);
      this->preferred_conn_params.latency = t_conn_params[4] | (t_conn_params[5] << 8);
      this->preferred_conn_params.timeout = t_conn_params[6] | (t_conn_params[7] << 8);
      memcpy(this->preferred_conn_params.bda, this->parent()->get_remote_bda(), 6);
      LOGI("[configure] preferred conn params: "
               "interval=%.2f-%.2f ms latency=%u timeout=%.1f ms",
               preferred_conn_params.min_int * 1.25f,
               preferred_conn_params.max_int * 1.25f,
               preferred_conn_params.latency,
               preferred_conn_params.timeout * 10.0f);
    }
  }
  LOGI("[configure] configure_hid_client() done: "
           "notify_pending=%d/%d report_map=%s hid_cp=0x%04X",
           this->handles_waiting_for_notify_registration,
           notify_chars,
           this->hid_report_map != nullptr ? "OK" : "MISSING",
           this->hid_control_point_handle_);
  // delete read data:
  for (auto &kv : this->handles_to_read) {
    delete kv.second;
  }
  this->handles_to_read.clear();
}

}  // namespace remodox
}  // namespace esphome
#endif