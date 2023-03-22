#include "TrumaiNetBoxApp.h"
#include "TrumaStatusFrame.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "helpers.h"

namespace esphome {
namespace truma_inetbox {

static const char *const TAG = "truma_inetbox.TrumaiNetBoxApp";

void TrumaiNetBoxApp::update() {
  // Call listeners in after method 'lin_multiframe_recieved' call.
  // Because 'lin_multiframe_recieved' is time critical an all these sensors can take some time.

  // Run through callbacks
  heater_.update();
  timer_.update();
  clock_.update();
  config_.update();

  LinBusProtocol::update();

#ifdef USE_TIME
  // Update time of CP Plus automatically when
  // - Time component configured
  // - Update was not done
  // - 30 seconds after init data recieved
  if (this->time_ != nullptr && !this->update_status_clock_done && this->init_recieved_ > 0) {
    if (micros() > ((30 * 1000 * 1000) + this->init_recieved_ /* 30 seconds after init recieved */)) {
      this->update_status_clock_done = true;
      this->action_write_time();
    }
  }
#endif  // USE_TIME
}

template<typename T> void TrumaStausFrameStorage<T>::update() {
  if (this->data_updated_) {
    this->state_callback_.call(&this->data_);
  }
  this->data_updated_ = false;
}

const std::array<uint8_t, 4> TrumaiNetBoxApp::lin_identifier() {
  // Supplier Id: 0x4617 - Truma (Phone: +49 (0)89 4617-0)
  // Unknown:
  // 17.46.01.03 - old Combi model
  // 17.46.10.03 - Unknown more comms required for init.
  // 17.46.20.03 - Unknown more comms required for init.
  // Heater:
  // 17.46.40.03 - H2.00.01 - 0340.xx Combi 4/6
  // Aircon:
  // 17.46.00.0C - A23.70.0 - 0C00.xx (with light option: OFF/1..5)
  // 17.46.01.0C - A23.70.0 - 0C01.xx
  // 17.46.02.0C
  // 17.46.03.0C
  // 17.46.04.0C - A23.70.0 - 0C04.xx (with light option: OFF/1..5)
  // 17.46.05.0C - A23.70.0 - 0C05.xx
  // 17.46.06.0C - A23.70.0 - 0C06.xx (with light option: OFF/1..5)
  // 17.46.07.0C - A23.70.0 - 0C07.xx (with light option: OFF/1..5)
  // iNet Box:
  // 17.46.00.1F - T23.70.0 - 1F00.xx iNet Box
  return {0x17 /*Supplied Id*/, 0x46 /*Supplied Id*/, 0x00 /*Function Id*/, 0x1F /*Function Id*/};
}

void TrumaiNetBoxApp::lin_heartbeat() { this->device_registered_ = micros(); }

void TrumaiNetBoxApp::lin_reset_device() {
  LinBusProtocol::lin_reset_device();
  this->device_registered_ = micros();
  this->init_recieved_ = 0;

  this->heater_.reset();
  this->timer_.reset();
  this->airconManual_.reset();
  this->clock_.reset();
  this->config_.reset();

  this->update_time_ = 0;
}

template<typename T> void TrumaStausFrameStorage<T>::reset() {
  this->data_valid_ = false;
  this->data_updated_ = false;
}

template<typename T, typename TResponse> void TrumaStausFrameResponseStorage<T, TResponse>::reset() {
  TrumaStausFrameStorage<T>::reset();
  this->update_status_prepared_ = false;
  this->update_status_unsubmitted_ = false;
  this->update_status_stale_ = false;
}

bool TrumaiNetBoxApp::answer_lin_order_(const u_int8_t pid) {
  // Alive message
  if (pid == LIN_PID_TRUMA_INET_BOX) {
    std::array<u_int8_t, 8> response = this->lin_empty_response_;

    if (this->updates_to_send_.empty() && !this->has_update_to_submit_()) {
      response[0] = 0xFE;
    }
    this->write_lin_answer_(response.data(), (u_int8_t) sizeof(response));
    return true;
  }
  return LinBusProtocol::answer_lin_order_(pid);
}

bool TrumaiNetBoxApp::lin_read_field_by_identifier_(u_int8_t identifier, std::array<u_int8_t, 5> *response) {
  if (identifier == 0x00 /* LIN Product Identification */) {
    auto lin_identifier = this->lin_identifier();
    (*response)[0] = lin_identifier[0];
    (*response)[1] = lin_identifier[1];
    (*response)[2] = lin_identifier[2];
    (*response)[3] = lin_identifier[3];
    (*response)[4] = 0x01;  // Variant
    return true;
  } else if (identifier == 0x20 /* Product details to display in CP plus */) {
    auto lin_identifier = this->lin_identifier();
    // Only the first three parts are displayed.
    (*response)[0] = lin_identifier[0];
    (*response)[1] = lin_identifier[1];
    (*response)[2] = lin_identifier[2];
    // (*response)[3] = // unknown
    // (*response)[4] = // unknown
    return true;
  } else if (identifier == 0x22 /* unknown usage */) {
    // Init is failing if missing
    // Data can be anything?
    return true;
  }
  return false;
}

const u_int8_t *TrumaiNetBoxApp::lin_multiframe_recieved(const u_int8_t *message, const u_int8_t message_len,
                                                         u_int8_t *return_len) {
  static u_int8_t response[48] = {};
  // Validate message prefix.
  if (message_len < truma_message_header.size()) {
    return nullptr;
  }
  for (u_int8_t i = 1; i < truma_message_header.size(); i++) {
    if (message[i] != truma_message_header[i]) {
      return nullptr;
    }
  }

  if (message[0] == LIN_SID_READ_STATE_BUFFER) {
    // Example: BA.00.1F.00.1E.00.00.22.FF.FF.FF (11)
    memset(response, 0, sizeof(response));
    auto response_frame = reinterpret_cast<StatusFrame *>(response);

    // The order must match with the method 'has_update_to_submit_'.
    if (this->init_recieved_ == 0) {
      ESP_LOGD(TAG, "Requested read: Sending init");
      status_frame_create_init(response_frame, return_len, this->message_counter++);
      return response;
    } else if (this->heater_.update_status_unsubmitted_) {
      ESP_LOGD(TAG, "Requested read: Sending heater update");
      status_frame_create_update_heater(
          response_frame, return_len, this->message_counter++, this->heater_.update_status_.target_temp_room,
          this->heater_.update_status_.target_temp_water, this->heater_.update_status_.heating_mode,
          this->heater_.update_status_.energy_mix_a, this->heater_.update_status_.el_power_level_a);

      this->update_time_ = 0;
      this->heater_.update_status_prepared_ = false;
      this->heater_.update_status_unsubmitted_ = false;
      this->heater_.update_status_stale_ = true;
      return response;
    } else if (this->timer_.update_status_unsubmitted_) {
      ESP_LOGD(TAG, "Requested read: Sending timer update");
      status_frame_create_update_timer(
          response_frame, return_len, this->message_counter++, this->timer_.update_status_.timer_resp_active,
          this->timer_.update_status_.timer_resp_start_hours, this->timer_.update_status_.timer_resp_start_minutes,
          this->timer_.update_status_.timer_resp_stop_hours, this->timer_.update_status_.timer_resp_stop_minutes,
          this->timer_.update_status_.timer_target_temp_room, this->timer_.update_status_.timer_target_temp_water,
          this->timer_.update_status_.timer_heating_mode, this->timer_.update_status_.timer_energy_mix_a,
          this->timer_.update_status_.timer_el_power_level_a);

      this->update_time_ = 0;
      this->timer_.update_status_prepared_ = false;
      this->timer_.update_status_unsubmitted_ = false;
      this->timer_.update_status_stale_ = true;
      return response;
    } else if (this->airconManual_.update_status_unsubmitted_) {
      ESP_LOGD(TAG, "Requested read: Sending aircon update");

      status_frame_create_empty(response_frame, STATUS_FRAME_AIRCON_MANUAL_RESPONSE, sizeof(StatusFrameAirconManualResponse),
                                this->message_counter++);

      response_frame->inner.airconManualResponse.mode = this->airconManual_.update_status_.mode;
      response_frame->inner.airconManualResponse.unknown_02 = this->airconManual_.update_status_.unknown_02;
      response_frame->inner.airconManualResponse.operation = this->airconManual_.update_status_.operation;
      response_frame->inner.airconManualResponse.energy_mix = this->airconManual_.update_status_.energy_mix;
      response_frame->inner.airconManualResponse.target_temp_aircon = this->airconManual_.update_status_.target_temp_aircon;

      status_frame_calculate_checksum(response_frame);
      (*return_len) = sizeof(StatusFrameHeader) + sizeof(StatusFrameAirconManualResponse);

      this->update_time_ = 0;
      this->airconManual_.update_status_prepared_ = false;
      this->airconManual_.update_status_unsubmitted_ = false;
      this->airconManual_.update_status_stale_ = true;
      return response;
#ifdef USE_TIME
    } else if (this->update_status_clock_unsubmitted_) {
      if (this->time_ != nullptr) {
        ESP_LOGD(TAG, "Requested read: Sending clock update");
        // read time live
        auto now = this->time_->now();

        status_frame_create_update_clock(response_frame, return_len, this->message_counter++, now.hour, now.minute,
                                         now.second, this->clock_.data_.clock_mode);
      }
      this->update_status_clock_unsubmitted_ = false;
      return response;
#endif  // USE_TIME
    } else {
      ESP_LOGW(TAG, "Requested read: CP Plus asks for an update, but I have none.");
    }
  }

  if (message_len < sizeof(StatusFrame) && message[0] == LIN_SID_FIll_STATE_BUFFFER) {
    return nullptr;
  }

  auto statusFrame = reinterpret_cast<const StatusFrame *>(message);
  auto header = &statusFrame->inner.genericHeader;
  // Validate Truma frame checksum
  if (header->checksum != data_checksum(&statusFrame->raw[10], sizeof(StatusFrame) - 10, (0xFF - header->checksum)) ||
      header->header_2 != 'T' || header->header_3 != 0x01) {
    ESP_LOGE(TAG, "Truma checksum fail.");
    return nullptr;
  }

  // create acknowledge response.
  response[0] = (header->service_identifier | LIN_SID_RESPONSE);
  (*return_len) = 1;

  if (header->message_type == STATUS_FRAME_HEATER && header->message_length == sizeof(StatusFrameHeater)) {
    ESP_LOGI(TAG, "StatusFrameHeater");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|tRoom|mo|  |elecA|tWate|elecB|mi|mi|cWate|cRoom|st|err  |  |
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.14.33.00.12.00.00.00.00.00.00.00.00.00.00.01.01.CC.0B.6C.0B.00.00.00.00
    this->heater_.data_ = statusFrame->inner.heater;
    this->heater_.data_valid_ = true;
    this->heater_.data_updated_ = true;

    this->heater_.update_status_stale_ = false;
    return response;
  } else if (header->message_type == STATUS_FRAME_AIRCON_MANUAL && header->message_length == sizeof(StatusFrameAirconManual)) {
    ESP_LOGI(TAG, "StatusFrameAirconManual");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // - ac temps form 16 - 30 C in +2 steps
    // - activation and deactivation of the ac ventilating
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.AA.00.00.71.01.00.00.00.00.86.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.A5.00.00.71.01.00.00.00.00.8B.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.A5.00.00.71.01.00.00.00.00.8B.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.4B.05.00.71.01.4A.0B.00.00.8B.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.37.05.00.71.01.5E.0B.00.00.8B.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.24.05.00.71.01.72.0B.00.00.8A.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.13.05.00.71.01.86.0B.00.00.87.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.FC.05.00.71.01.9A.0B.00.00.89.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.E8.05.00.71.01.AE.0B.00.00.89.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.D5.05.00.71.01.C2.0B.00.00.88.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.C1.05.00.71.01.D6.0B.00.00.88.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.A7.00.00.71.01.00.00.00.00.89.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.C2.04.00.71.01.D6.0B.00.00.88.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.13.04.00.71.01.86.0B.00.00.88.0B.00.00.00.00.00.00.AA.0A
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.35.00.A8.00.00.71.01.00.00.00.00.88.0B.00.00.00.00.00.00.AA.0A
    this->airconManual_.data_ = statusFrame->inner.airconManual;
    this->airconManual_.data_valid_ = true;
    this->airconManual_.data_updated_ = true;

    this->airconManual_.update_status_stale_ = false;
    return response;
  } else if (header->message_type == STATUS_FRAME_AIRCON_MANUAL_INIT &&
             header->message_length == sizeof(StatusFrameAirconManualInit)) {
    ESP_LOGI(TAG, "StatusFrameAirconManualInit");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.16.3F.00.E2.00.00.71.01.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00
    return response;
  } else if (header->message_type == STATUS_FRAME_AIRCON_AUTO && header->message_length == sizeof(StatusFrameAirconAuto)) {
    ESP_LOGI(TAG, "StatusFrameAirconAuto");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.12.37.00.BF.01.00.01.00.00.00.00.00.00.00.00.00.00.00.49.0B.40.0B
    return response;
  } else if (header->message_type == STATUS_FRAME_AIRCON_AUTO_INIT &&
             header->message_length == sizeof(StatusFrameAirconAutoInit)) {
    ESP_LOGI(TAG, "StatusFrameAirconAutoInit");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.14.41.00.53.01.00.01.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00.00
    return response;
  } else if (header->message_type == STATUS_FRAME_TIMER && header->message_length == sizeof(StatusFrameTimer)) {
    ESP_LOGI(TAG, "StatusFrameTimer");
    // EXAMPLE:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|tRoom|mo|??|elecA|tWate|elecB|mi|mi|<--response-->|??|??|on|start|stop-|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.18.3D.00.1D.18.0B.01.00.00.00.00.00.00.00.01.01.00.00.00.00.00.00.00.01.00.08.00.09
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.18.3D.00.13.18.0B.0B.00.00.00.00.00.00.00.01.01.00.00.00.00.00.00.00.01.00.08.00.09
    this->timer_.data_ = statusFrame->inner.timer;
    this->timer_.data_valid_ = true;
    this->timer_.data_updated_ = true;

    this->timer_.update_status_stale_ = false;

    ESP_LOGD(TAG, "StatusFrameTimer target_temp_room: %f target_temp_water: %f %02u:%02u -> %02u:%02u %s",
             temp_code_to_decimal(this->timer_.data_.timer_target_temp_room),
             temp_code_to_decimal(this->timer_.data_.timer_target_temp_water), this->timer_.data_.timer_start_hours,
             this->timer_.data_.timer_start_minutes, this->timer_.data_.timer_stop_hours,
             this->timer_.data_.timer_stop_minutes, ((u_int8_t) this->timer_.data_.timer_active ? " ON" : " OFF"));

    return response;
  } else if (header->message_type == STATUS_FRAME_RESPONSE_ACK &&
             header->message_length == sizeof(StatusFrameResponseAck)) {
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.02.0D.01.98.02.00
    auto data = statusFrame->inner.responseAck;

    if (data.error_code != ResponseAckResult::RESPONSE_ACK_RESULT_OKAY) {
      ESP_LOGW(TAG, "StatusFrameResponseAck");
    } else {
      ESP_LOGI(TAG, "StatusFrameResponseAck");
    }
    ESP_LOGD(TAG, "StatusFrameResponseAck %02X %s %02X", statusFrame->inner.genericHeader.command_counter,
             data.error_code == ResponseAckResult::RESPONSE_ACK_RESULT_OKAY ? " OKAY " : " FAILED ",
             (u_int8_t) data.error_code);

    if (data.error_code != ResponseAckResult::RESPONSE_ACK_RESULT_OKAY) {
      // I tried to update something and it failed. Read current state again to validate and hold any updates for now.
      this->lin_reset_device();
    }

    return response;
  } else if (header->message_type == STATUS_FRAME_CLOCK && header->message_length == sizeof(StatusFrameClock)) {
    ESP_LOGI(TAG, "StatusFrameClock");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.15.00.5B.0D.20.00.01.01.00.00.01.00.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.15.00.71.16.00.00.01.01.00.00.02.00.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.15.00.2B.16.1F.28.01.01.00.00.01.00.00
    this->clock_.data_ = statusFrame->inner.clock;
    this->clock_.data_valid_ = true;
    this->clock_.data_updated_ = true;

    ESP_LOGD(TAG, "StatusFrameClock %02d:%02d:%02d", this->clock_.data_.clock_hour, this->clock_.data_.clock_minute,
             this->clock_.data_.clock_second);

    return response;
  } else if (header->message_type == STAUTS_FRAME_CONFIG && header->message_length == sizeof(StatusFrameConfig)) {
    ESP_LOGI(TAG, "StatusFrameConfig");
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.17.00.0F.06.01.B4.0A.AA.0A.00.00.00.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.17.00.41.06.01.B4.0A.78.0A.00.00.00.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0A.17.00.0F.06.01.B4.0A.AA.0A.00.00.00.00
    this->config_.data_ = statusFrame->inner.config;
    this->config_.data_valid_ = true;
    this->config_.data_updated_ = true;

    ESP_LOGD(TAG, "StatusFrameConfig Offset: %.1f", temp_code_to_decimal(this->config_.data_.temp_offset));

    return response;
  } else if (header->message_type == STATUS_FRAME_DEVICES && header->message_length == sizeof(StatusFrameDevice)) {
    ESP_LOGI(TAG, "StatusFrameDevice");
    // This message is special. I recieve one response per registered (at CP plus) device.
    // Example:
    // SID<---------PREAMBLE---------->|<---MSG_HEAD---->|count|st|??|Hardware|Software|??|??
    // Combi4
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.79.02.00.01.00.50.00.00.04.03.02.AD.10 - C4.03.02 0050.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.27.02.01.01.00.40.03.22.02.00.01.00.00 - H2.00.01 0340.22
    // VarioHeat Comfort w/o E-Kit
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.C2.02.00.01.00.51.00.00.05.01.00.66.10 - P5.01.00 0051.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.64.02.01.01.00.20.06.02.03.00.00.00.00 - H3.00.00 0620.02
    // Combi6DE + Saphir Compact AC
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.C7.03.00.01.00.50.00.00.04.03.00.60.10
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.71.03.01.01.00.10.03.02.06.00.02.00.00
    // BB.00.1F.00.1E.00.00.22.FF.FF.FF.54.01.0C.0B.00.7C.03.02.01.00.01.0C.00.01.02.01.00.00

    auto device = statusFrame->inner.device;

    this->init_recieved_ = micros();

    ESP_LOGD(TAG, "StatusFrameDevice %d/%d - %d.%02d.%02d %04X.%02X (%02X %02X)", device.device_id + 1,
             device.device_count, device.software_revision[0], device.software_revision[1], device.software_revision[2],
             device.hardware_revision_major, device.hardware_revision_minor, device.unknown_2, device.unknown_3);

    const auto truma_device = static_cast<TRUMA_DEVICE>(device.software_revision[0]);
    {
      bool found_unknown_value = false;
      if (device.unknown_1 != 0x00)
        found_unknown_value = true;
      if (truma_device != TRUMA_DEVICE::AIRCON_DEVICE && truma_device != TRUMA_DEVICE::HEATER_COMBI4 &&
          truma_device != TRUMA_DEVICE::HEATER_VARIO && truma_device != TRUMA_DEVICE::CPPLUS_COMBI &&
          truma_device != TRUMA_DEVICE::CPPLUS_VARIO && truma_device != TRUMA_DEVICE::HEATER_COMBI6D)
        found_unknown_value = true;

      if (found_unknown_value)
        ESP_LOGW(TAG, "Unknown information in StatusFrameDevice found. Please report.");
    }

    if (truma_device == TRUMA_DEVICE::HEATER_COMBI4) {
      this->heater_device_ = TRUMA_DEVICE::HEATER_COMBI4;
    } else if (truma_device == TRUMA_DEVICE::HEATER_COMBI6D) {
      this->heater_device_ = TRUMA_DEVICE::HEATER_COMBI6D;
    } else if (truma_device == TRUMA_DEVICE::HEATER_VARIO) {
      this->heater_device_ = TRUMA_DEVICE::HEATER_VARIO;
    }

    if (truma_device == TRUMA_DEVICE::AIRCON_DEVICE) {
      this->aircon_device_ = TRUMA_DEVICE::AIRCON_DEVICE;
    }

    return response;
  } else {
    ESP_LOGW(TAG, "Unknown message type %02X", header->message_type);
  }
  (*return_len) = 0;
  return nullptr;
}

bool TrumaiNetBoxApp::has_update_to_submit_() {
  // No logging in this message!
  // It is called by interrupt. Logging is a blocking operation (especially when Wifi Logging).
  // If logging is necessary use logging queue of LinBusListener class.
  if (this->init_requested_ == 0) {
    this->init_requested_ = micros();
    // ESP_LOGD(TAG, "Requesting initial data.");
    return true;
  } else if (this->init_recieved_ == 0) {
    auto init_wait_time = micros() - this->init_requested_;
    // it has been 5 seconds and i am still awaiting the init data.
    if (init_wait_time > 1000 * 1000 * 5) {
      // ESP_LOGD(TAG, "Requesting initial data again.");
      this->init_requested_ = micros();
      return true;
    }
  } else if (this->heater_.update_status_unsubmitted_ || this->timer_.update_status_unsubmitted_ ||
             this->update_status_clock_unsubmitted_ || this->airconManual_.update_status_unsubmitted_) {
    if (this->update_time_ == 0) {
      // ESP_LOGD(TAG, "Notify CP Plus I got updates.");
      this->update_time_ = micros();
      return true;
    }
    auto update_wait_time = micros() - this->update_time_;
    if (update_wait_time > 1000 * 1000 * 5) {
      // ESP_LOGD(TAG, "Notify CP Plus again I still got updates.");
      this->update_time_ = micros();
      return true;
    }
  }
  return false;
}

}  // namespace truma_inetbox
}  // namespace esphome