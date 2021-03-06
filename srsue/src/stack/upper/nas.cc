/*
 * Copyright 2013-2019 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsue/hdr/stack/upper/nas.h"
#include "srslte/common/bcd_helpers.h"
#include "srslte/common/security.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <srslte/asn1/liblte_mme.h>
#include <sstream>
#include <unistd.h>

#include "srslte/asn1/liblte_mme.h"
#include "srslte/common/bcd_helpers.h"
#include "srslte/common/security.h"
#include "srsue/hdr/stack/upper/nas.h"

using namespace srslte;

#define ProcError(fmt, ...) nas_ptr->nas_log->error("Proc \"%s\" - " fmt, name(), ##__VA_ARGS__)
#define ProcWarning(fmt, ...) nas_ptr->nas_log->warning("Proc \"%s\" - " fmt, name(), ##__VA_ARGS__)
#define ProcInfo(fmt, ...) nas_ptr->nas_log->info("Proc \"%s\" - " fmt, name(), ##__VA_ARGS__)

namespace srsue {

using srslte::proc_outcome_t;

proc_outcome_t nas::plmn_search_proc::init()
{
  // start RRC
  state = state_t::plmn_search;
  if (not nas_ptr->rrc->plmn_search()) {
    ProcError("ProcError while searching for PLMNs\n");
    return proc_outcome_t::error;
  }

  ProcInfo("Starting...\n");
  return proc_outcome_t::yield;
}

proc_outcome_t nas::plmn_search_proc::step()
{
  return proc_outcome_t::yield;
}

void nas::plmn_search_proc::then(const srslte::proc_state_t& result)
{
  ProcInfo("Completed with %s\n", result.is_success() ? "success" : "failure");

  // start T3411
  nas_ptr->nas_log->debug("Starting T3411\n");
  nas_ptr->t3411.run();

  if (result.is_error()) {
    nas_ptr->enter_emm_deregistered();
  }
}

proc_outcome_t nas::plmn_search_proc::react(const rrc_connect_proc::rrc_connect_complete_ev& t)
{
  if (state != state_t::rrc_connect) {
    // not expecting a rrc connection result
    ProcWarning("Received unexpected RRC Connection Result event\n");
    return proc_outcome_t::yield;
  }
  return t.is_success() ? proc_outcome_t::success : proc_outcome_t::error;
}

proc_outcome_t nas::plmn_search_proc::react(const plmn_search_complete_t& t)
{
  if (state != state_t::plmn_search) {
    ProcWarning("PLMN Search Complete was received but PLMN Search is not running.\n");
    return proc_outcome_t::yield; // ignore
  }

  // check whether the state hasn't changed
  if (nas_ptr->state != EMM_STATE_DEREGISTERED or nas_ptr->plmn_is_selected) {
    ProcError("ProcError while searching for PLMNs\n");
    return proc_outcome_t::error;
  }

  if (t.nof_plmns < 0) {
    ProcError("Error while searching for PLMNs\n");
    return proc_outcome_t::error;
  }
  if (t.nof_plmns == 0) {
    ProcWarning("Did not find any PLMN in the set of frequencies.\n");
    return proc_outcome_t::error;
  }

  // Save PLMNs
  nas_ptr->known_plmns.clear();
  for (int i = 0; i < t.nof_plmns; i++) {
    nas_ptr->known_plmns.push_back(t.found_plmns[i].plmn_id);
    nas_ptr->nas_log->info(
        "Found PLMN:  Id=%s, TAC=%d\n", t.found_plmns[i].plmn_id.to_string().c_str(), t.found_plmns[i].tac);
    nas_ptr->nas_log->console(
        "Found PLMN:  Id=%s, TAC=%d\n", t.found_plmns[i].plmn_id.to_string().c_str(), t.found_plmns[i].tac);
  }
  nas_ptr->select_plmn();

  // Select PLMN in request establishment of RRC connection
  if (not nas_ptr->plmn_is_selected) {
    ProcError("PLMN is not selected because no suitable PLMN was found\n");
    return proc_outcome_t::error;
  }

  nas_ptr->rrc->plmn_select(nas_ptr->current_plmn);

  state = state_t::rrc_connect;
  if (not nas_ptr->rrc_connector.launch(srslte::establishment_cause_t::mo_data, nullptr)) {
    ProcError("Unable to initiate RRC connection.\n");
    return proc_outcome_t::error;
  }
  nas_ptr->callbacks.add_proc(nas_ptr->rrc_connector);

  return proc_outcome_t::yield;
}

proc_outcome_t nas::rrc_connect_proc::init(srslte::establishment_cause_t cause_, srslte::unique_byte_buffer_t pdu)
{
  /* if (nas_ptr->rrc->is_connected()) {
    Info("Stopping. Reason: Already connected\n");
    return proc_outcome_t::success;
  } */

  if (pdu == nullptr) {
    // Generate service request or attach request message
    pdu = srslte::allocate_unique_buffer(*nas_ptr->pool, true);
    if (!pdu) {
      ProcError("Fatal Error: Couldn't allocate PDU.\n");
      return proc_outcome_t::error;
    }

    if (nas_ptr->state == EMM_STATE_REGISTERED) {
      nas_ptr->gen_service_request(pdu.get());
    } else {
      nas_ptr->gen_attach_request(pdu.get());
    }
  }

  // Provide UE-Identity to RRC if have one
  if (nas_ptr->have_guti) {
    srslte::s_tmsi_t s_tmsi;
    s_tmsi.mmec   = nas_ptr->ctxt.guti.mme_code;
    s_tmsi.m_tmsi = nas_ptr->ctxt.guti.m_tmsi;
    nas_ptr->rrc->set_ue_identity(s_tmsi);
  }

  ProcInfo("Starting...\n");
  state = state_t::conn_req;
  if (not nas_ptr->rrc->connection_request(cause_, std::move(pdu))) {
    ProcError("Failed to initiate a connection request procedure\n");
    return proc_outcome_t::error;
  }
  return proc_outcome_t::yield;
}

proc_outcome_t nas::rrc_connect_proc::step()
{
  if (state != state_t::wait_attach) {
    return proc_outcome_t::yield;
  }
  wait_timeout++;
  // Wait until attachment. If doing a service request is already attached
  if (wait_timeout < 5000 and nas_ptr->state != EMM_STATE_REGISTERED and nas_ptr->running and
      nas_ptr->rrc->is_connected()) {
    return proc_outcome_t::yield;
  }
  if (nas_ptr->state == EMM_STATE_REGISTERED) {
    ProcInfo("Success: EMM Registered correctly.\n");
    return proc_outcome_t::success;
  } else if (nas_ptr->state == EMM_STATE_DEREGISTERED) {
    ProcError("Timeout or received attach reject while trying to attach\n");
    nas_ptr->nas_log->console("Failed to Attach\n");
  } else if (!nas_ptr->rrc->is_connected()) {
    ProcError("Was disconnected while attaching\n");
  } else {
    ProcError("Timed out while trying to attach\n");
  }
  return proc_outcome_t::error;
}

void nas::rrc_connect_proc::then(const srslte::proc_state_t& result)
{
  nas_ptr->plmn_searcher.trigger(result);
}

proc_outcome_t nas::rrc_connect_proc::react(nas::rrc_connect_proc::connection_request_completed_t event)
{
  if (state == state_t::conn_req and event.outcome) {
    ProcInfo("Connection established correctly. Waiting for Attach\n");
    wait_timeout = 0;
    // Wait until attachment. If doing a service request is already attached
    state = state_t::wait_attach;
    // wake up proc
    return proc_outcome_t::repeat;
  } else {
    ProcError("Could not establish RRC connection\n");
    return proc_outcome_t::error;
  }
}

/*********************************************************************
 *   NAS
 ********************************************************************/

nas::nas(srslte::log* log_, srslte::timer_handler* timers_) :
  nas_log(log_),
  pool(byte_buffer_pool::get_instance()),
  plmn_searcher(this),
  rrc_connector(this),
  timers(timers_),
  t3410(timers_->get_unique_timer()),
  t3411(timers_->get_unique_timer())
{
}

void nas::init(usim_interface_nas* usim_, rrc_interface_nas* rrc_, gw_interface_nas* gw_, const nas_args_t& cfg_) {
  init(usim_, rrc_, gw_, nullptr, cfg_);
}

void nas::init(usim_interface_nas* usim_, rrc_interface_nas* rrc_, gw_interface_nas* gw_, testbench_interface_nas* tb_, const nas_args_t& cfg_)
{
  usim = usim_;
  rrc = rrc_;
  gw = gw_;
  tb = tb_;
  state = EMM_STATE_DEREGISTERED;

  if (!usim->get_home_plmn_id(&home_plmn)) {
    nas_log->error("Getting Home PLMN Id from USIM. Defaulting to 001-01\n");
    home_plmn.from_number(61441, 65281); // This is 001 01
  }

  // parse and sanity check EIA list
  std::vector<uint8_t> cap_list = split_string(cfg_.eia);
  if (cap_list.empty()) {
    nas_log->error("Empty EIA list. Select at least one EIA algorithm.\n");
  }
  for (std::vector<uint8_t>::const_iterator it = cap_list.begin(); it != cap_list.end(); ++it) {
    if (*it != 0 && *it < 4) {
      eia_caps[*it] = true;
    } else {
      nas_log->error("EIA%d is not a valid EIA algorithm.\n", *it);
    }
  }

  // parse and sanity check EEA list
  cap_list = split_string(cfg_.eea);
  if (cap_list.empty()) {
    nas_log->error("Empty EEA list. Select at least one EEA algorithm.\n");
  }
  for (std::vector<uint8_t>::const_iterator it = cap_list.begin(); it != cap_list.end(); ++it) {
    if (*it < 4) {
      eea_caps[*it] = true;
    } else {
      nas_log->error("EEA%d is not a valid EEA algorithm.\n", *it);
    }
  }

  cfg = cfg_;

  if ((read_ctxt_file(&ctxt))) {
    usim->generate_nas_keys(ctxt.k_asme, k_nas_enc, k_nas_int, ctxt.cipher_algo, ctxt.integ_algo);
    nas_log->debug_hex(k_nas_enc, 32, "NAS encryption key - k_nas_enc");
    nas_log->debug_hex(k_nas_int, 32, "NAS integrity key - k_nas_int");
    have_guti = true;
    have_ctxt = true;
  }

  // Configure T3410 and T3411
  t3410.set(t3410_duration_ms, [this](uint32_t tid) { timer_expired(tid); });
  t3411.set(t3411_duration_ms, [this](uint32_t tid) { timer_expired(tid); });

  running = true;
}

void nas::stop()
{
  running = false;
  write_ctxt_file(ctxt);
}

void nas::get_metrics(nas_metrics_t* m)
{
  nas_metrics_t metrics         = {};
  metrics.state                 = state;
  metrics.nof_active_eps_bearer = eps_bearer.size();
  *m                            = metrics;
}

emm_state_t nas::get_state()
{
  return state;
}

void nas::run_tti(uint32_t tti)
{
  callbacks.run();
}

void nas::timer_expired(uint32_t timeout_id)
{
  if (timeout_id == t3410.id()) {
    nas_log->info("Timer T3410 expired: starting T3411\n");
    t3411.run();
  } else if (timeout_id == t3411.id()) {
    nas_log->info("Timer T3411 expired: trying to attach again\n");
    start_attach_request(nullptr, srslte::establishment_cause_t::mo_sig);
  } else {
    nas_log->error("Timeout from unknown timer id %d\n", timeout_id);
  }
}

/*******************************************************************************
 * UE interface
 ******************************************************************************/

/** Blocking function to Attach to the network and establish RRC connection if not established.
 * The function returns true if the UE could attach correctly or false in case of error or timeout during attachment.
 *
 */
void nas::start_attach_request(srslte::proc_state_t* result, srslte::establishment_cause_t cause_)
{
  nas_log->info("Attach Request\n");
  switch (state) {
    case EMM_STATE_DEREGISTERED:

      // start T3410
      nas_log->debug("Starting T3410\n");
      t3410.run();

      // stop T3411
      if (t3411.is_running()) {
        t3411.stop();
      }

      // Todo: stop T3402

      // Search PLMN is not selected
      if (!plmn_is_selected) {
        nas_log->info("No PLMN selected. Starting PLMN Search...\n");
        if (not plmn_searcher.launch()) {
          if (result != nullptr) {
            result->set_error();
          }
          return;
        }
        plmn_searcher.then([this, result](const proc_state_t& res) {
          nas_log->info("Attach Request from PLMN Search %s\n", res.is_success() ? "finished successfully" : "failed");
          if (result != nullptr) {
            *result = res;
          }
        });
      } else {
        nas_log->error("PLMN selected in state %s\n", emm_state_text[state]);
        if (result != nullptr) {
          result->set_error();
        }
      }
      break;
    case EMM_STATE_REGISTERED:
      if (rrc->is_connected()) {
        nas_log->info("NAS is already registered and RRC connected\n");
        if (result != nullptr) {
          result->set_val();
        }
      } else {
        nas_log->info("NAS is already registered but RRC disconnected. Connecting now...\n");
        if (not rrc_connector.launch(cause_, nullptr)) {
          nas_log->error("Cannot initiate concurrent rrc connection procedures\n");
          if (result != nullptr) {
            result->set_error();
          }
          return;
        }
        rrc_connector.then([this, result](const proc_state_t& res) {
          if (res.is_success()) {
            nas_log->info("NAS attached successfully\n");
          } else {
            nas_log->error("Could not attach from attach_request\n");
          }
          if (result != nullptr) {
            *result = res;
          }
        });
        callbacks.add_proc(rrc_connector);
      }
      break;
    default:
      nas_log->info("Attach request ignored. State = %s\n", emm_state_text[state]);
      if (result != nullptr) {
        result->set_error();
      }
  }
}

void nas::plmn_search_completed(const rrc_interface_nas::found_plmn_t found_plmns[rrc_interface_nas::MAX_FOUND_PLMNS],
                                int                                   nof_plmns)
{
  plmn_searcher.trigger(plmn_search_proc::plmn_search_complete_t(found_plmns, nof_plmns));
}

bool nas::detach_request(const bool switch_off)
{
  switch (state) {
    case EMM_STATE_DEREGISTERED:
      // do nothing ..
      break;
    case EMM_STATE_REGISTERED:
      // send detach request
      send_detach_request(switch_off);
      break;
    case EMM_STATE_DEREGISTERED_INITIATED:
      // do nothing ..
      break;
    default:
      break;
  }
  return false;
}

void nas::enter_emm_deregistered()
{
  // Deactivate EPS bearer according to Sec. 5.5.2.2.2
  nas_log->debug("Clearing EPS bearer context\n");

  eps_bearer.clear();

  plmn_is_selected = false;
  state            = EMM_STATE_DEREGISTERED;
}

void nas::left_rrc_connected() {}

bool nas::is_attached()
{
  return state == EMM_STATE_REGISTERED;
}

bool nas::paging(s_tmsi_t* ue_identity)
{
  if (state == EMM_STATE_REGISTERED) {
    nas_log->info("Received paging: requesting RRC connection establishment\n");
    if (not rrc_connector.launch(srslte::establishment_cause_t::mt_access, nullptr)) {
      nas_log->error("Could not launch RRC Connect()\n");
      return false;
    }
    // once completed, call paging complete
    rrc_connector.then([this](proc_state_t outcome) {
      rrc->paging_completed(outcome.is_success());
      return proc_outcome_t::success;
    });
    callbacks.add_proc(rrc_connector);
  } else {
    nas_log->warning("Received paging while in state %s\n", emm_state_text[state]);
    return false;
  }
  return true;
}

void nas::enable_sec_algo(sec_algo_type_t type, uint index, bool enable)
{
  if (index > 7) {
    nas_log->error("Security Algorithm %s%u is invalid\n", type == EIA ? "EIA" : "EEA", index);
    return;
  }

  if (type == EIA) {
    eia_caps[index] = enable;
  } else { // EEA
    eea_caps[index] = enable;
  }
}

void nas::set_barring(barring_t barring) {
  current_barring = barring;
}

// Signal from RRC that connection request proc completed
bool nas::connection_request_completed(bool outcome)
{
  rrc_connector.trigger(rrc_connect_proc::connection_request_completed_t{outcome});
  return true;
}

void nas::select_plmn()
{

  plmn_is_selected = false;

  // First find if Home PLMN is available
  for (const srslte::plmn_id_t& known_plmn : known_plmns) {
    if (known_plmn == home_plmn) {
      nas_log->info("Selecting Home PLMN Id=%s\n", known_plmn.to_string().c_str());
      plmn_is_selected = true;
      current_plmn     = known_plmn;
      return;
    }
  }

  // If not, select the first available PLMN
  if (not known_plmns.empty()) {
    nas_log->info("Could not find Home PLMN Id=%s, trying to connect to PLMN Id=%s\n",
                  home_plmn.to_string().c_str(),
                  known_plmns[0].to_string().c_str());

    nas_log->console("Could not find Home PLMN Id=%s, trying to connect to PLMN Id=%s\n",
                     home_plmn.to_string().c_str(),
                     known_plmns[0].to_string().c_str());
    plmn_is_selected = true;
    current_plmn     = known_plmns[0];
  }
}

void nas::write_pdu(uint32_t lcid, unique_byte_buffer_t pdu)
{
  uint8 pd           = 0;
  uint8 msg_type     = 0;
  uint8 sec_hdr_type = 0;

  nas_log->info_hex(pdu->msg, pdu->N_bytes, "DL %s PDU", rrc->get_rb_name(lcid).c_str());

  // Parse the message security header
  liblte_mme_parse_msg_sec_header((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &pd, &sec_hdr_type);
  switch (sec_hdr_type) {
    case LIBLTE_MME_SECURITY_HDR_TYPE_PLAIN_NAS:
    case LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_WITH_NEW_EPS_SECURITY_CONTEXT:
    case LIBLTE_MME_SECURITY_HDR_TYPE_SERVICE_REQUEST:
      break;
    case LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY:
    case LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED:
      if (!(integrity_check(pdu.get()))) {
        nas_log->error("Integrity check error\n");
      }
      if (sec_hdr_type == LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED) {
        cipher_decrypt(pdu.get());
      }
      break;
    case LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED_WITH_NEW_EPS_SECURITY_CONTEXT:
      break;
    default:
      nas_log->error("Not handling NAS message with SEC_HDR_TYPE=%02X\n", sec_hdr_type);
      return;
  }

  // Write NAS pcap
  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  // Parse the message header
  liblte_mme_parse_msg_header((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &pd, &msg_type);
  nas_log->info_hex(pdu->msg, pdu->N_bytes, "DL %s Decrypted PDU", rrc->get_rb_name(lcid).c_str());
  // TODO: Check if message type requieres specical security header type and if it isvalid

  switch (msg_type) {
    case LIBLTE_MME_MSG_TYPE_ATTACH_ACCEPT:
      parse_attach_accept(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_ATTACH_REJECT:
      parse_attach_reject(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_AUTHENTICATION_REQUEST:
      parse_authentication_request(lcid, std::move(pdu), sec_hdr_type);
      break;
    case LIBLTE_MME_MSG_TYPE_AUTHENTICATION_REJECT:
      parse_authentication_reject(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_IDENTITY_REQUEST:
      parse_identity_request(std::move(pdu), sec_hdr_type);
      break;
    case LIBLTE_MME_MSG_TYPE_SECURITY_MODE_COMMAND:
      parse_security_mode_command(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_SERVICE_REJECT:
      parse_service_reject(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_ESM_INFORMATION_REQUEST:
      parse_esm_information_request(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_EMM_INFORMATION:
      parse_emm_information(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_EMM_STATUS:
      parse_emm_status(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_DETACH_REQUEST:
      parse_detach_request(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_ACTIVATE_DEDICATED_EPS_BEARER_CONTEXT_REQUEST:
      parse_activate_dedicated_eps_bearer_context_request(lcid, std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_DEACTIVATE_EPS_BEARER_CONTEXT_REQUEST:
      parse_deactivate_eps_bearer_context_request(std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_MODIFY_EPS_BEARER_CONTEXT_REQUEST:
      parse_modify_eps_bearer_context_request(std::move(pdu));
      break;
    case LIBLTE_MME_MSG_TYPE_ACTIVATE_TEST_MODE:
      parse_activate_test_mode(lcid, std::move(pdu), sec_hdr_type);
      break;
    default:
      nas_log->error("Not handling NAS message with MSG_TYPE=%02X\n", msg_type);
      return;
  }
}

void nas::set_k_enb_count(uint32_t count)
{
  // UL count for RRC key derivation depends on UL Count of the Attach Request or Service Request.
  // On the case of an Authentication Request, the UL count used to generate K_enb must be reset to zero.
  ctxt.k_enb_count = count;
}

uint32_t nas::get_k_enb_count()
{
  return ctxt.k_enb_count;
}

bool nas::get_k_asme(uint8_t* k_asme_, uint32_t n)
{
  if (!have_ctxt) {
    nas_log->error("K_asme requested before security context established\n");
    return false;
  }
  if (nullptr == k_asme_ || n < 32) {
    nas_log->error("Invalid parameters to get_k_asme");
    return false;
  }

  memcpy(k_asme_, ctxt.k_asme, 32);
  return true;
}

uint32_t nas::get_ipv4_addr()
{
  return ip_addr;
}

bool nas::get_ipv6_addr(uint8_t* ipv6_addr)
{
  uint8_t null_addr[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (memcmp(ipv6_addr, null_addr, 8) != 0) {
    memcpy(ipv6_addr, ipv6_if_id, 8);
    return true;
  }
  return false;
}

/*******************************************************************************
  PCAP
*******************************************************************************/

void nas::start_pcap(srslte::nas_pcap* pcap_)
{
  pcap = pcap_;
}

/*******************************************************************************
 * Security
 ******************************************************************************/

void nas::integrity_generate(uint8_t* key_128,
                             uint32_t count,
                             uint8_t  direction,
                             uint8_t* msg,
                             uint32_t msg_len,
                             uint8_t* mac)
{
  switch (ctxt.integ_algo) {
    case INTEGRITY_ALGORITHM_ID_EIA0:
      bzero(mac, 4);
      break;
    case INTEGRITY_ALGORITHM_ID_128_EIA1:
      security_128_eia1(key_128,
                        count,
                        0, // Bearer always 0 for NAS
                        direction,
                        msg,
                        msg_len,
                        mac);
      break;
    case INTEGRITY_ALGORITHM_ID_128_EIA2:
      security_128_eia2(key_128,
                        count,
                        0, // Bearer always 0 for NAS
                        direction,
                        msg,
                        msg_len,
                        mac);
      break;
    case INTEGRITY_ALGORITHM_ID_128_EIA3:
      security_128_eia3(key_128,
                        count,
                        0, // Bearer always 0 for NAS
                        direction,
                        msg,
                        msg_len,
                        mac);
      break;
    default:
      bzero(mac, 4);
      break;
  }
}

// This function depends to a valid k_nas_int.
// This key is generated in the security mode command.

bool nas::integrity_check(byte_buffer_t* pdu)
{
  if (pdu == nullptr) {
    nas_log->error("Invalid PDU\n");
    return false;
  }

  if (pdu->N_bytes > 5) {
    uint8_t  exp_mac[4] = {0};
    uint8_t* mac        = &pdu->msg[1];

    // generate expected MAC
    uint32_t count_est = (ctxt.rx_count & 0x0FF0u) | pdu->msg[5];
    integrity_generate(
        &k_nas_int[16], count_est, SECURITY_DIRECTION_DOWNLINK, &pdu->msg[5], pdu->N_bytes - 5, &exp_mac[0]);

    // Check if expected mac equals the sent mac
    for (int i = 0; i < 4; i++) {
      if (exp_mac[i] != mac[i]) {
        nas_log->warning("Integrity check failure. Local: count=%d, [%02x %02x %02x %02x], "
                         "Received: count=%d, [%02x %02x %02x %02x]\n",
                         ctxt.rx_count,
                         exp_mac[0],
                         exp_mac[1],
                         exp_mac[2],
                         exp_mac[3],
                         pdu->msg[5],
                         mac[0],
                         mac[1],
                         mac[2],
                         mac[3]);
        return false;
      }
    }
    nas_log->info("Integrity check ok. Local: count=%d, Received: count=%d\n", ctxt.rx_count, pdu->msg[5]);

    // Updated local count (according to TS 24.301 Sec. 4.4.3.3)
    if (pdu->msg[5] != ctxt.rx_count) {
      nas_log->info("Update local count to received value %d\n", pdu->msg[5]);
      ctxt.rx_count = count_est;
    }
    return true;
  } else {
    nas_log->error("Invalid integrity check PDU size (%d)\n", pdu->N_bytes);
    return false;
  }
}

void nas::cipher_encrypt(byte_buffer_t* pdu)
{
  byte_buffer_t pdu_tmp;
  switch (ctxt.cipher_algo) {
    case CIPHERING_ALGORITHM_ID_EEA0:
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA1:
      security_128_eea1(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_UPLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &pdu_tmp.msg[6]);
      memcpy(&pdu->msg[6], &pdu_tmp.msg[6], pdu->N_bytes - 6);
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA2:
      security_128_eea2(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_UPLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &pdu_tmp.msg[6]);
      memcpy(&pdu->msg[6], &pdu_tmp.msg[6], pdu->N_bytes - 6);
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA3:
      security_128_eea3(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_UPLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &pdu_tmp.msg[6]);
      memcpy(&pdu->msg[6], &pdu_tmp.msg[6], pdu->N_bytes - 6);
      break;
    default:
      nas_log->error("Ciphering algorithm not known\n");
      break;
  }
}

void nas::cipher_decrypt(byte_buffer_t* pdu)
{
  byte_buffer_t tmp_pdu;
  switch (ctxt.cipher_algo) {
    case CIPHERING_ALGORITHM_ID_EEA0:
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA1:
      security_128_eea1(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_DOWNLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &tmp_pdu.msg[6]);
      memcpy(&pdu->msg[6], &tmp_pdu.msg[6], pdu->N_bytes - 6);
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA2:
      security_128_eea2(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_DOWNLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &tmp_pdu.msg[6]);
      nas_log->debug_hex(tmp_pdu.msg, pdu->N_bytes, "Decrypted");
      memcpy(&pdu->msg[6], &tmp_pdu.msg[6], pdu->N_bytes - 6);
      break;
    case CIPHERING_ALGORITHM_ID_128_EEA3:
      security_128_eea3(&k_nas_enc[16],
                        pdu->msg[5],
                        0, // Bearer always 0 for NAS
                        SECURITY_DIRECTION_DOWNLINK,
                        &pdu->msg[6],
                        pdu->N_bytes - 6,
                        &tmp_pdu.msg[6]);
      nas_log->debug_hex(tmp_pdu.msg, pdu->N_bytes, "Decrypted");
      memcpy(&pdu->msg[6], &tmp_pdu.msg[6], pdu->N_bytes - 6);
      break;
    default:
      nas_log->error("Ciphering algorithms not known\n");
      break;
  }
}

bool nas::check_cap_replay(LIBLTE_MME_UE_SECURITY_CAPABILITIES_STRUCT* caps)
{
  for (uint32_t i = 0; i < 8; i++) {
    if (caps->eea[i] != eea_caps[i] || caps->eia[i] != eia_caps[i]) {
      return false;
    }
  }
  return true;
}

/*******************************************************************************
 * Parsers
 ******************************************************************************/

void nas::parse_attach_accept(uint32_t lcid, unique_byte_buffer_t pdu)
{
  if (!pdu) {
    nas_log->error("Invalid PDU\n");
    return;
  }

  if (pdu->N_bytes <= 5) {
    nas_log->error("Invalid attach accept PDU size (%d)\n", pdu->N_bytes);
    return;
  }

  nas_log->info("Received Attach Accept\n");

  // stop T3410
  if (t3410.is_running()) {
    t3410.stop();
  }

  LIBLTE_MME_ATTACH_ACCEPT_MSG_STRUCT attach_accept = {};
  liblte_mme_unpack_attach_accept_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &attach_accept);

  tb->report_attach_accept();

  if (attach_accept.eps_attach_result == LIBLTE_MME_EPS_ATTACH_RESULT_EPS_ONLY) {
    // TODO: Handle t3412.unit
    // TODO: Handle tai_list
    if (attach_accept.guti_present) {
      memcpy(&ctxt.guti, &attach_accept.guti.guti, sizeof(LIBLTE_MME_EPS_MOBILE_ID_GUTI_STRUCT));
      have_guti = true;
      // Update RRC UE-Idenity
      s_tmsi_t s_tmsi;
      s_tmsi.mmec   = ctxt.guti.mme_code;
      s_tmsi.m_tmsi = ctxt.guti.m_tmsi;
      rrc->set_ue_identity(s_tmsi);
    }
    if (attach_accept.lai_present) {
    }
    if (attach_accept.ms_id_present) {
    }
    if (attach_accept.emm_cause_present) {
    }
    if (attach_accept.t3402_present) {
    }
    if (attach_accept.t3412_ext_present) {
    }
    if (attach_accept.t3423_present) {
    }
    if (attach_accept.equivalent_plmns_present) {
    }
    if (attach_accept.emerg_num_list_present) {
    }
    if (attach_accept.eps_network_feature_support_present) {
    }
    if (attach_accept.additional_update_result_present) {
    }

    LIBLTE_MME_ACTIVATE_DEFAULT_EPS_BEARER_CONTEXT_REQUEST_MSG_STRUCT act_def_eps_bearer_context_req = {};
    liblte_mme_unpack_activate_default_eps_bearer_context_request_msg(&attach_accept.esm_msg,
                                                                      &act_def_eps_bearer_context_req);

    if ((cfg.apn_protocol == "ipv4" && LIBLTE_MME_PDN_TYPE_IPV6 == act_def_eps_bearer_context_req.pdn_addr.pdn_type) ||
        (cfg.apn_protocol == "ipv6" && LIBLTE_MME_PDN_TYPE_IPV4 == act_def_eps_bearer_context_req.pdn_addr.pdn_type)) {
      nas_log->error("Failed to attach -- Mismatch between PDN protocol and PDN type in attach accept.\n");
      return;
    }
    if (("ipv4v6" == cfg.apn_protocol &&
         LIBLTE_MME_PDN_TYPE_IPV4 == act_def_eps_bearer_context_req.pdn_addr.pdn_type) ||
        ("ipv4v6" == cfg.apn_protocol &&
         LIBLTE_MME_PDN_TYPE_IPV6 == act_def_eps_bearer_context_req.pdn_addr.pdn_type)) {
      nas_log->warning("Requested IPv4v6, but only received a single PDN address.\n");
      nas_log->warning("EMM Cause: %d\n", attach_accept.emm_cause);
    }
    if (LIBLTE_MME_PDN_TYPE_IPV4 == act_def_eps_bearer_context_req.pdn_addr.pdn_type) {
      ip_addr = 0;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[0] << 24u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[1] << 16u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[2] << 8u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[3];

      nas_log->info("Network attach successful. APN: %s, IP: %u.%u.%u.%u\n",
                    act_def_eps_bearer_context_req.apn.apn,
                    act_def_eps_bearer_context_req.pdn_addr.addr[0],
                    act_def_eps_bearer_context_req.pdn_addr.addr[1],
                    act_def_eps_bearer_context_req.pdn_addr.addr[2],
                    act_def_eps_bearer_context_req.pdn_addr.addr[3]);

      nas_log->console("Network attach successful. IP: %u.%u.%u.%u\n",
                       act_def_eps_bearer_context_req.pdn_addr.addr[0],
                       act_def_eps_bearer_context_req.pdn_addr.addr[1],
                       act_def_eps_bearer_context_req.pdn_addr.addr[2],
                       act_def_eps_bearer_context_req.pdn_addr.addr[3]);

      // Setup GW
      char* err_str = nullptr;
      if (gw->setup_if_addr(rrc->get_lcid_for_eps_bearer(act_def_eps_bearer_context_req.eps_bearer_id),
                            LIBLTE_MME_PDN_TYPE_IPV4,
                            ip_addr,
                            nullptr,
                            err_str)) {
        nas_log->error("Failed to set gateway address - %s\n", err_str);
      }
    } else if (LIBLTE_MME_PDN_TYPE_IPV6 == act_def_eps_bearer_context_req.pdn_addr.pdn_type) {
      memcpy(ipv6_if_id, act_def_eps_bearer_context_req.pdn_addr.addr, 8);
      nas_log->info("Network attach successful. APN: %s, IPv6 interface id: %02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                    act_def_eps_bearer_context_req.apn.apn,
                    act_def_eps_bearer_context_req.pdn_addr.addr[0],
                    act_def_eps_bearer_context_req.pdn_addr.addr[1],
                    act_def_eps_bearer_context_req.pdn_addr.addr[2],
                    act_def_eps_bearer_context_req.pdn_addr.addr[3],
                    act_def_eps_bearer_context_req.pdn_addr.addr[4],
                    act_def_eps_bearer_context_req.pdn_addr.addr[5],
                    act_def_eps_bearer_context_req.pdn_addr.addr[6],
                    act_def_eps_bearer_context_req.pdn_addr.addr[7]);

      nas_log->console("Network attach successful. IPv6 interface Id: %02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                       act_def_eps_bearer_context_req.pdn_addr.addr[0],
                       act_def_eps_bearer_context_req.pdn_addr.addr[1],
                       act_def_eps_bearer_context_req.pdn_addr.addr[2],
                       act_def_eps_bearer_context_req.pdn_addr.addr[3],
                       act_def_eps_bearer_context_req.pdn_addr.addr[4],
                       act_def_eps_bearer_context_req.pdn_addr.addr[5],
                       act_def_eps_bearer_context_req.pdn_addr.addr[6],
                       act_def_eps_bearer_context_req.pdn_addr.addr[7]);
      // Setup GW
      char* err_str = nullptr;
      if (gw->setup_if_addr(rrc->get_lcid_for_eps_bearer(act_def_eps_bearer_context_req.eps_bearer_id),
                            LIBLTE_MME_PDN_TYPE_IPV6,
                            0,
                            ipv6_if_id,
                            err_str)) {
        nas_log->error("Failed to set gateway address - %s\n", err_str);
      }
    } else if (LIBLTE_MME_PDN_TYPE_IPV4V6 == act_def_eps_bearer_context_req.pdn_addr.pdn_type) {
      memcpy(ipv6_if_id, act_def_eps_bearer_context_req.pdn_addr.addr, 8);
      // IPv6
      nas_log->info("Network attach successful. APN: %s, IPv6 interface id: %02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                    act_def_eps_bearer_context_req.apn.apn,
                    act_def_eps_bearer_context_req.pdn_addr.addr[0],
                    act_def_eps_bearer_context_req.pdn_addr.addr[1],
                    act_def_eps_bearer_context_req.pdn_addr.addr[2],
                    act_def_eps_bearer_context_req.pdn_addr.addr[3],
                    act_def_eps_bearer_context_req.pdn_addr.addr[4],
                    act_def_eps_bearer_context_req.pdn_addr.addr[5],
                    act_def_eps_bearer_context_req.pdn_addr.addr[6],
                    act_def_eps_bearer_context_req.pdn_addr.addr[7]);
      nas_log->console("Network attach successful. IPv6 interface Id: %02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                       act_def_eps_bearer_context_req.pdn_addr.addr[0],
                       act_def_eps_bearer_context_req.pdn_addr.addr[1],
                       act_def_eps_bearer_context_req.pdn_addr.addr[2],
                       act_def_eps_bearer_context_req.pdn_addr.addr[3],
                       act_def_eps_bearer_context_req.pdn_addr.addr[4],
                       act_def_eps_bearer_context_req.pdn_addr.addr[5],
                       act_def_eps_bearer_context_req.pdn_addr.addr[6],
                       act_def_eps_bearer_context_req.pdn_addr.addr[7]);
      // IPv4
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[8] << 24u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[9] << 16u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[10] << 8u;
      ip_addr |= act_def_eps_bearer_context_req.pdn_addr.addr[11];

      nas_log->info("Network attach successful. APN: %s, IP: %u.%u.%u.%u\n",
                    act_def_eps_bearer_context_req.apn.apn,
                    act_def_eps_bearer_context_req.pdn_addr.addr[8],
                    act_def_eps_bearer_context_req.pdn_addr.addr[9],
                    act_def_eps_bearer_context_req.pdn_addr.addr[10],
                    act_def_eps_bearer_context_req.pdn_addr.addr[11]);

      nas_log->console("Network attach successful. IP: %u.%u.%u.%u\n",
                       act_def_eps_bearer_context_req.pdn_addr.addr[8],
                       act_def_eps_bearer_context_req.pdn_addr.addr[9],
                       act_def_eps_bearer_context_req.pdn_addr.addr[10],
                       act_def_eps_bearer_context_req.pdn_addr.addr[11]);

      char* err_str = nullptr;
      if (gw->setup_if_addr(rrc->get_lcid_for_eps_bearer(act_def_eps_bearer_context_req.eps_bearer_id),
                            LIBLTE_MME_PDN_TYPE_IPV4V6,
                            ip_addr,
                            ipv6_if_id,
                            err_str)) {
        nas_log->error("Failed to set gateway address - %s\n", err_str);
      }
    } else {
      nas_log->error("PDN type not IPv4, IPv6 nor IPv4v6\n");
      return;
    }

    if (act_def_eps_bearer_context_req.transaction_id_present) {
      transaction_id = act_def_eps_bearer_context_req.proc_transaction_id;
    }

    // Search for DNS entry in protocol config options
    if (act_def_eps_bearer_context_req.protocol_cnfg_opts_present) {
      for (uint32_t i = 0; i < act_def_eps_bearer_context_req.protocol_cnfg_opts.N_opts; i++) {
        if (act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].id ==
            LIBLTE_MME_ADDITIONAL_PARAMETERS_DL_DNS_SERVER_IPV4_ADDRESS) {
          uint32_t dns_addr = 0;
          dns_addr |= act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[0] << 24u;
          dns_addr |= act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[1] << 16u;
          dns_addr |= act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[2] << 8u;
          dns_addr |= act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[3];
          nas_log->info("DNS: %u.%u.%u.%u\n",
                        act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[0],
                        act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[1],
                        act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[2],
                        act_def_eps_bearer_context_req.protocol_cnfg_opts.opt[i].contents[3]);
        }
      }
    }

    // TODO: Handle the following parameters
    //    act_def_eps_bearer_context_req.eps_qos.qci
    //    act_def_eps_bearer_context_req.eps_qos.br_present
    //    act_def_eps_bearer_context_req.eps_qos.br_ext_present
    //    act_def_eps_bearer_context_req.apn.apn
    //    act_def_eps_bearer_context_req.negotiated_qos_present
    //    act_def_eps_bearer_context_req.llc_sapi_present
    //    act_def_eps_bearer_context_req.radio_prio_present
    //    act_def_eps_bearer_context_req.packet_flow_id_present
    //    act_def_eps_bearer_context_req.apn_ambr_present
    //    act_def_eps_bearer_context_req.protocol_cnfg_opts_present
    //    act_def_eps_bearer_context_req.connectivity_type_present

    // TODO: Setup the default EPS bearer context

    eps_bearer_t bearer  = {};
    bearer.type          = DEFAULT_EPS_BEARER;
    bearer.eps_bearer_id = act_def_eps_bearer_context_req.eps_bearer_id;
    if (eps_bearer.insert(eps_bearer_map_pair_t(bearer.eps_bearer_id, bearer)).second) {
      // bearer added successfully
      state = EMM_STATE_REGISTERED;

      // send attach complete
      send_attach_complete(transaction_id, bearer.eps_bearer_id);
    } else {
      // bearer already exists (perhaps the attach complete got lost and this is a retx?)
      // TODO: what are we supposed to do in this case?
      nas_log->error("Error adding EPS bearer.\n");
    }

  } else {
    nas_log->info("Not handling attach type %u\n", attach_accept.eps_attach_result);
    enter_emm_deregistered();
  }

  ctxt.rx_count++;
}

void nas::parse_attach_reject(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_ATTACH_REJECT_MSG_STRUCT attach_rej;
  ZERO_OBJECT(attach_rej);

  liblte_mme_unpack_attach_reject_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &attach_rej);
  nas_log->warning("Received Attach Reject. Cause= %02X\n", attach_rej.emm_cause);
  nas_log->console("Received Attach Reject. Cause= %02X\n", attach_rej.emm_cause);

  tb->report_attach_reject(attach_rej.emm_cause);

  enter_emm_deregistered();
  // TODO: Command RRC to release?
}

void nas::parse_authentication_request(uint32_t lcid, unique_byte_buffer_t pdu, const uint8_t sec_hdr_type)
{
  LIBLTE_MME_AUTHENTICATION_REQUEST_MSG_STRUCT auth_req;
  bzero(&auth_req, sizeof(LIBLTE_MME_AUTHENTICATION_REQUEST_MSG_STRUCT));

  nas_log->info("Received Authentication Request\n");
  liblte_mme_unpack_authentication_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &auth_req);

  ctxt.rx_count++;

  // Generate authentication response using RAND, AUTN & KSI-ASME
  uint16 mcc, mnc;
  mcc = rrc->get_mcc();
  mnc = rrc->get_mnc();

  nas_log->info("MCC=%d, MNC=%d\n", mcc, mnc);

  uint8_t res[16];
  int     res_len = 0;
  nas_log->debug_hex(auth_req.rand, 16, "Authentication request RAND\n");
  nas_log->debug_hex(auth_req.autn, 16, "Authentication request AUTN\n");
  auth_result_t auth_result =
      usim->generate_authentication_response(auth_req.rand, auth_req.autn, mcc, mnc, res, &res_len, ctxt.k_asme);
  if (LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE == auth_req.nas_ksi.tsc_flag) {
    ctxt.ksi = auth_req.nas_ksi.nas_ksi;
  } else {
    nas_log->error("NAS mapped security context not currently supported\n");
    nas_log->console("Warning: NAS mapped security context not currently supported\n");
  }

  if (auth_result == AUTH_OK) {
    nas_log->info("Network authentication successful\n");
    send_authentication_response(res, res_len, sec_hdr_type);
    nas_log->info_hex(ctxt.k_asme, 32, "Generated k_asme:\n");
    set_k_enb_count(0);
    auth_request = true;
  } else if (auth_result == AUTH_SYNCH_FAILURE) {
    nas_log->error("Network authentication synchronization failure.\n");
    send_authentication_failure(LIBLTE_MME_EMM_CAUSE_SYNCH_FAILURE, res);
  } else {
    nas_log->warning("Network authentication failure\n");
    nas_log->console("Warning: Network authentication failure\n");
    send_authentication_failure(LIBLTE_MME_EMM_CAUSE_MAC_FAILURE, nullptr);
  }
}

void nas::parse_authentication_reject(uint32_t lcid, unique_byte_buffer_t pdu)
{
  nas_log->warning("Received Authentication Reject\n");
/*   report_attach_result(false, LIBLTE_MME_MSG_TYPE_AUTHENTICATION_REJECT);
 */  enter_emm_deregistered();
  // FIXME: Command RRC to release?
}

void nas::parse_identity_request(unique_byte_buffer_t pdu, const uint8_t sec_hdr_type)
{
  LIBLTE_MME_ID_REQUEST_MSG_STRUCT id_req = {};
  liblte_mme_unpack_identity_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &id_req);

  nas_log->info("Received Identity Request. ID type: %d\n", id_req.id_type);
  ctxt.rx_count++;

  // do not respond if request is not protected (TS 24.301 Sec. 4.4.4.2)
  if (sec_hdr_type >= LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY ||
      (sec_hdr_type == LIBLTE_MME_SECURITY_HDR_TYPE_PLAIN_NAS && id_req.id_type == LIBLTE_MME_MOBILE_ID_TYPE_IMSI)) {
    send_identity_response(id_req.id_type, sec_hdr_type);
  } else {
    nas_log->info("Not sending identity response due to missing integrity protection.\n");
  }
}

void nas::parse_security_mode_command(uint32_t lcid, unique_byte_buffer_t pdu)
{
  if (!pdu) {
    nas_log->error("Invalid PDU\n");
    return;
  }

  if (pdu->N_bytes <= 5) {
    nas_log->error("Invalid security mode command PDU size (%d)\n", pdu->N_bytes);
    return;
  }

  LIBLTE_MME_SECURITY_MODE_COMMAND_MSG_STRUCT sec_mode_cmd;
  bzero(&sec_mode_cmd, sizeof(LIBLTE_MME_SECURITY_MODE_COMMAND_MSG_STRUCT));
  LIBLTE_MME_SECURITY_MODE_COMPLETE_MSG_STRUCT sec_mode_comp;
  bzero(&sec_mode_comp, sizeof(LIBLTE_MME_SECURITY_MODE_COMPLETE_MSG_STRUCT));

  LIBLTE_ERROR_ENUM ret = liblte_mme_unpack_security_mode_command_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &sec_mode_cmd);
  if (ret != LIBLTE_SUCCESS) {
    nas_log->error("Error decoding Security Mode Command\n");
    return;
  }
  nas_log->info("Received Security Mode Command ksi: %d, eea: %s, eia: %s\n",
                sec_mode_cmd.nas_ksi.nas_ksi,
                ciphering_algorithm_id_text[sec_mode_cmd.selected_nas_sec_algs.type_of_eea],
                integrity_algorithm_id_text[sec_mode_cmd.selected_nas_sec_algs.type_of_eia]);

  if (sec_mode_cmd.nas_ksi.tsc_flag != LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE) {
    nas_log->error("Mapped security context not supported\n");
    return;
  }

  if (have_ctxt) {
    if (sec_mode_cmd.nas_ksi.nas_ksi != ctxt.ksi) {
      nas_log->warning("Sending Security Mode Reject due to key set ID mismatch\n");
      send_security_mode_reject(LIBLTE_MME_EMM_CAUSE_SECURITY_MODE_REJECTED_UNSPECIFIED);
      // TODO:merlin how to deal with this case? does MME change behaviour in next attach?
      return;
    }
  }

  // MME is setting up security context

  // TODO: check nonce (not sent by Amari)

  // Check capabilities replay
  if (!check_cap_replay(&sec_mode_cmd.ue_security_cap)) {
    nas_log->console("Security Capabilities replay mismatch\n");
    nas_log->error("Security Capabilities replay mismatch\n");
    // send_security_mode_reject(LIBLTE_MME_EMM_CAUSE_UE_SECURITY_CAPABILITIES_MISMATCH);
    //return;
  }

  // Reset counters (as per 24.301 5.4.3.2), only needed for initial security mode command
  if (auth_request) {
    ctxt.rx_count = 0;
    ctxt.tx_count = 0;
    auth_request  = false;
  }

  ctxt.cipher_algo = (CIPHERING_ALGORITHM_ID_ENUM)sec_mode_cmd.selected_nas_sec_algs.type_of_eea;
  ctxt.integ_algo  = (INTEGRITY_ALGORITHM_ID_ENUM)sec_mode_cmd.selected_nas_sec_algs.type_of_eia;

  // Check capabilities
  if(!eea_caps[ctxt.cipher_algo] || !eia_caps[ctxt.integ_algo]) {
    nas_log->console("Security capabilities mismatch\n");
    nas_log->error("Security capabilities mismatch\n");
    // send_security_mode_reject(LIBLTE_MME_EMM_CAUSE_UE_SECURITY_CAPABILITIES_MISMATCH);
    return;
  }

  // Generate NAS keys
  usim->generate_nas_keys(ctxt.k_asme, k_nas_enc, k_nas_int, ctxt.cipher_algo, ctxt.integ_algo);
  nas_log->info_hex(k_nas_enc, 32, "NAS encryption key - k_nas_enc");
  nas_log->info_hex(k_nas_int, 32, "NAS integrity key - k_nas_int");

  nas_log->debug(
      "Generating integrity check. integ_algo:%d, count_dl:%d, lcid:%d\n", ctxt.integ_algo, ctxt.rx_count, lcid);

  if (not integrity_check(pdu.get())) {
    nas_log->error("Integrity Check error in Security Mode Command\n");
    nas_log->warning("Integrity Check error in Security Mode Command\n");
  }

  tb->report_nas_security_mode_command(ctxt.integ_algo, ctxt.cipher_algo);

  ctxt.rx_count++;

  // Take security context into use
  have_ctxt = true;

  if (sec_mode_cmd.imeisv_req_present && LIBLTE_MME_IMEISV_REQUESTED == sec_mode_cmd.imeisv_req) {
    sec_mode_comp.imeisv_present    = true;
    sec_mode_comp.imeisv.type_of_id = LIBLTE_MME_MOBILE_ID_TYPE_IMEISV;
    usim->get_imei_vec(sec_mode_comp.imeisv.imeisv, 15);
    sec_mode_comp.imeisv.imeisv[14] = 5;
    sec_mode_comp.imeisv.imeisv[15] = 3;
  } else {
    sec_mode_comp.imeisv_present = false;
  }

  // Send response
  pdu->clear();
  liblte_mme_pack_security_mode_complete_msg(
      &sec_mode_comp,
      LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED_WITH_NEW_EPS_SECURITY_CONTEXT,
      ctxt.tx_count,
      (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());
  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }
  cipher_encrypt(pdu.get());
  integrity_generate(
      &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  nas_log->info("Sending Security Mode Complete nas_current_ctxt.tx_count=%d, RB=%s\n",
                ctxt.tx_count,
                rrc->get_rb_name(lcid).c_str());
  rrc->write_sdu(std::move(pdu));
  ctxt.tx_count++;
}

void nas::parse_service_reject(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_SERVICE_REJECT_MSG_STRUCT service_reject;
  if (liblte_mme_unpack_service_reject_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &service_reject)) {
    nas_log->error("Error unpacking service reject.\n");
    return;
  }

/*   report_attach_result(false, LIBLTE_MME_MSG_TYPE_SERVICE_REJECT);
 */
  nas_log->console("Received service reject with EMM cause=0x%x.\n", service_reject.emm_cause);
  if (service_reject.t3446_present) {
    nas_log->info(
        "Received service reject with EMM cause=0x%x and t3446=%d\n", service_reject.emm_cause, service_reject.t3446);
  }

  // TODO: handle NAS backoff-timers correctly

  enter_emm_deregistered();

  // Reset security context
  ctxt      = {};
  have_ctxt = false;
  have_guti = false;

  // Send attach request after receiving service reject 
  pdu->clear(); 
  /* gen_attach_request(pdu.get());
  rrc->write_sdu(std::move(pdu)); */
}

void nas::parse_esm_information_request(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_ESM_INFORMATION_REQUEST_MSG_STRUCT esm_info_req;
  liblte_mme_unpack_esm_information_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &esm_info_req);

  nas_log->info("ESM information request received for beaser=%d, transaction_id=%d\n",
                esm_info_req.eps_bearer_id,
                esm_info_req.proc_transaction_id);
  ctxt.rx_count++;

  // send response
  send_esm_information_response(esm_info_req.proc_transaction_id);
}

void nas::parse_emm_information(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_EMM_INFORMATION_MSG_STRUCT emm_info = {};
  liblte_mme_unpack_emm_information_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &emm_info);
  std::string str = emm_info_str(&emm_info);
  nas_log->info("Received EMM Information: %s\n", str.c_str());
  nas_log->console("%s\n", str.c_str());
  ctxt.rx_count++;
}

void nas::parse_detach_request(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_DETACH_REQUEST_MSG_STRUCT detach_request;
  liblte_mme_unpack_detach_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &detach_request);
  ctxt.rx_count++;

/*   report_attach_result(false, LIBLTE_MME_MSG_TYPE_DETACH_REQUEST);
 */
  switch (state) {
    case EMM_STATE_DEREGISTERED_INITIATED:
      nas_log->info("Received detach from network while performing UE initiated detach. Aborting UE detach.\n");
    case EMM_STATE_REGISTERED:
      nas_log->info("Received detach request (type=%d)\n", detach_request.detach_type.type_of_detach);

      // send accept and leave state
      send_detach_accept();
      enter_emm_deregistered();
      break;
    default:
      nas_log->warning("Received detach request in invalid state (%s)\n", emm_state_text[state]);
      break;
  }
}

void nas::parse_activate_dedicated_eps_bearer_context_request(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_ACTIVATE_DEDICATED_EPS_BEARER_CONTEXT_REQUEST_MSG_STRUCT request;
  liblte_mme_unpack_activate_dedicated_eps_bearer_context_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &request);

  nas_log->info(
      "Received Activate Dedicated EPS bearer context request (eps_bearer_id=%d, linked_bearer_id=%d, proc_id=%d)\n",
      request.eps_bearer_id,
      request.linked_eps_bearer_id,
      request.proc_transaction_id);

  ctxt.rx_count++;
  LIBLTE_MME_TRAFFIC_FLOW_TEMPLATE_STRUCT* tft = &request.tft;
  nas_log->info("Traffic Flow Template: TFT OP code 0x%x, Filter list size %d, Parameter list size %d\n",
                tft->tft_op_code,
                tft->packet_filter_list_size,
                tft->parameter_list_size);

  // check the a linked default bearer exists
  if (eps_bearer.find(request.linked_eps_bearer_id) == eps_bearer.end()) {
    nas_log->error("No linked default EPS bearer found (%d).\n", request.linked_eps_bearer_id);
    // TODO: send reject according to 24.301 Sec 6.4.2.5 paragraph c
    return;
  }

  // check if the dedicated EPS bearer already exists
  if (eps_bearer.find(request.eps_bearer_id) != eps_bearer.end()) {
    // according to 24.301 Sec 6.4.2.5 paragraph b) the existing bearer shall be deactived before proceeding
    nas_log->error("EPS bearer already exists (%d). Removing it.\n", request.eps_bearer_id);

    // remove bearer
    eps_bearer_map_t::iterator it = eps_bearer.find(request.eps_bearer_id);
    eps_bearer.erase(it);
  }

  // create new bearer
  eps_bearer_t bearer         = {};
  bearer.type                 = DEDICATED_EPS_BEARER;
  bearer.eps_bearer_id        = request.eps_bearer_id;
  bearer.linked_eps_bearer_id = request.linked_eps_bearer_id;
  if (not eps_bearer.insert(eps_bearer_map_pair_t(bearer.eps_bearer_id, bearer)).second) {
    nas_log->error("Error adding EPS bearer.\n");
    return;
  }

  // apply packet filters to GW
  gw->apply_traffic_flow_template(request.eps_bearer_id, rrc->get_lcid_for_eps_bearer(request.eps_bearer_id), tft);

  send_activate_dedicated_eps_bearer_context_accept(request.proc_transaction_id, request.eps_bearer_id);
}

void nas::parse_deactivate_eps_bearer_context_request(unique_byte_buffer_t pdu)
{
  LIBLTE_MME_DEACTIVATE_EPS_BEARER_CONTEXT_REQUEST_MSG_STRUCT request;

  liblte_mme_unpack_deactivate_eps_bearer_context_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &request);

  nas_log->info("Received Deactivate EPS bearer context request (eps_bearer_id=%d, proc_id=%d, cause=0x%X)\n",
                request.eps_bearer_id,
                request.proc_transaction_id,
                request.esm_cause);

  ctxt.rx_count++;

  // check if bearer exists
  if (eps_bearer.find(request.eps_bearer_id) == eps_bearer.end()) {
    nas_log->error("EPS bearer doesn't exist (eps_bearer_id=%d)\n", request.eps_bearer_id);
    // fixme: send proper response
    return;
  }

  // remove bearer
  eps_bearer_map_t::iterator it = eps_bearer.find(request.eps_bearer_id);
  eps_bearer.erase(it);

  nas_log->info("Removed EPS bearer context (eps_bearer_id=%d)\n", request.eps_bearer_id);

  send_deactivate_eps_bearer_context_accept(request.proc_transaction_id, request.eps_bearer_id);
}

void nas::parse_modify_eps_bearer_context_request(srslte::unique_byte_buffer_t pdu)
{
  LIBLTE_MME_MODIFY_EPS_BEARER_CONTEXT_REQUEST_MSG_STRUCT request;

  liblte_mme_unpack_modify_eps_bearer_context_request_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &request);

  nas_log->info("Received Modify EPS bearer context request (eps_bearer_id=%d, proc_id=%d)\n",
                request.eps_bearer_id,
                request.proc_transaction_id);

  ctxt.rx_count++;

  // check if bearer exists
  if (eps_bearer.find(request.eps_bearer_id) == eps_bearer.end()) {
    nas_log->error("EPS bearer doesn't exist (eps_bearer_id=%d)\n", request.eps_bearer_id);
    // fixme: send proper response
    return;
  }

  // fixme: carry out modification
  nas_log->info("Modified EPS bearer context (eps_bearer_id=%d)\n", request.eps_bearer_id);

  send_modify_eps_bearer_context_accept(request.proc_transaction_id, request.eps_bearer_id);
}

void nas::parse_activate_test_mode(uint32_t lcid, unique_byte_buffer_t pdu, const uint8_t sec_hdr_type)
{
  nas_log->info("Received Activate test mode\n");

  ctxt.rx_count++;

  send_activate_test_mode_complete(sec_hdr_type);
}

void nas::parse_emm_status(uint32_t lcid, unique_byte_buffer_t pdu)
{
  LIBLTE_MME_EMM_STATUS_MSG_STRUCT emm_status;
  liblte_mme_unpack_emm_status_msg((LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), &emm_status);
  ctxt.rx_count++;

  switch (emm_status.emm_cause) {
    case LIBLTE_MME_ESM_CAUSE_INVALID_EPS_BEARER_IDENTITY:
      nas_log->info("Received EMM status: Invalid EPS bearer identity\n");
      // TODO: abort any ongoing procedure (see Sec. 6.7 in TS 24.301)
      break;
    case LIBLTE_MME_ESM_CAUSE_INVALID_PTI_VALUE:
      nas_log->info("Received EMM status: Invalid PTI value\n");
      // TODO: abort any ongoing procedure (see Sec. 6.7 in TS 24.301)
      break;
    case LIBLTE_MME_ESM_CAUSE_MESSAGE_TYPE_NON_EXISTENT_OR_NOT_IMPLEMENTED:
      nas_log->info("Received EMM status: Invalid PTI value\n");
      // TODO: see Sec. 6.7 in TS 24.301
      break;
    default:
      nas_log->info("Received unknown EMM status (cause=%d)\n", emm_status.emm_cause);
      break;
  }
}

/*******************************************************************************
 * Senders
 ******************************************************************************/

void nas::gen_attach_request(byte_buffer_t* msg)
{
  if (msg == nullptr) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in gen_attach_request().\n");
    return;
  }
  LIBLTE_MME_ATTACH_REQUEST_MSG_STRUCT attach_req;
  bzero(&attach_req, sizeof(LIBLTE_MME_ATTACH_REQUEST_MSG_STRUCT));

  nas_log->info("Generating attach request\n");

  attach_req.eps_attach_type = LIBLTE_MME_EPS_ATTACH_TYPE_EPS_ATTACH;

  for (u_int32_t i = 0; i < 8; i++) {
    attach_req.ue_network_cap.eea[i] = eea_caps[i];
    attach_req.ue_network_cap.eia[i] = eia_caps[i];
  }

  attach_req.ue_network_cap.uea_present                     = false; // UMTS encryption algos
  attach_req.ue_network_cap.uia_present                     = false; // UMTS integrity algos
  attach_req.ue_network_cap.ucs2_present                    = false;
  attach_req.ms_network_cap_present                         = false; // A/Gb mode (2G) or Iu mode (3G)
  attach_req.ue_network_cap.lpp_present                     = false;
  attach_req.ue_network_cap.lcs_present                     = false;
  attach_req.ue_network_cap.onexsrvcc_present               = false;
  attach_req.ue_network_cap.nf_present                      = false;
  attach_req.old_p_tmsi_signature_present                   = false;
  attach_req.additional_guti_present                        = false;
  attach_req.last_visited_registered_tai_present            = false;
  attach_req.drx_param_present                              = false;
  attach_req.old_lai_present                                = false;
  attach_req.tmsi_status_present                            = false;
  attach_req.ms_cm2_present                                 = false;
  attach_req.ms_cm3_present                                 = false;
  attach_req.supported_codecs_present                       = false;
  attach_req.additional_update_type_present                 = false;
  attach_req.voice_domain_pref_and_ue_usage_setting_present = false;
  attach_req.device_properties_present                      = false;
  attach_req.old_guti_type_present                          = false;

  // ESM message (PDN connectivity request) for first default bearer
  gen_pdn_connectivity_request(&attach_req.esm_msg);

  // GUTI or IMSI attach
  if (have_guti && have_ctxt && !cfg.force_imsi_attach) {
    attach_req.tmsi_status_present      = true;
    attach_req.tmsi_status              = LIBLTE_MME_TMSI_STATUS_VALID_TMSI;
    attach_req.eps_mobile_id.type_of_id = LIBLTE_MME_EPS_MOBILE_ID_TYPE_GUTI;
    memcpy(&attach_req.eps_mobile_id.guti, &ctxt.guti, sizeof(LIBLTE_MME_EPS_MOBILE_ID_GUTI_STRUCT));
    attach_req.old_guti_type         = LIBLTE_MME_GUTI_TYPE_NATIVE;
    attach_req.old_guti_type_present = true;
    attach_req.nas_ksi.tsc_flag      = LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE;
    attach_req.nas_ksi.nas_ksi       = ctxt.ksi;
    nas_log->info("Requesting GUTI attach. "
                  "m_tmsi: %x, mcc: %x, mnc: %x, mme_group_id: %x, mme_code: %x\n",
                  ctxt.guti.m_tmsi,
                  ctxt.guti.mcc,
                  ctxt.guti.mnc,
                  ctxt.guti.mme_group_id,
                  ctxt.guti.mme_code);
    liblte_mme_pack_attach_request_msg(
        &attach_req, LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY, ctxt.tx_count, (LIBLTE_BYTE_MSG_STRUCT*)msg);

    // Add MAC
    if (msg->N_bytes > 5) {
      integrity_generate(
          &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &msg->msg[5], msg->N_bytes - 5, &msg->msg[1]);
    } else {
      nas_log->error("Invalid PDU size %d\n", msg->N_bytes);
    }
  } else {
    attach_req.eps_mobile_id.type_of_id = LIBLTE_MME_EPS_MOBILE_ID_TYPE_IMSI;
    attach_req.nas_ksi.tsc_flag         = LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE;
    attach_req.nas_ksi.nas_ksi          = 0;
    usim->get_imsi_vec(attach_req.eps_mobile_id.imsi, 15);
    nas_log->info("Requesting IMSI attach (IMSI=%s)\n", usim->get_imsi_str().c_str());
    liblte_mme_pack_attach_request_msg(&attach_req, (LIBLTE_BYTE_MSG_STRUCT*)msg);
  }

  if (pcap != nullptr) {
    pcap->write_nas(msg->msg, msg->N_bytes);
  }

  if (have_ctxt) {
    set_k_enb_count(ctxt.tx_count);
    ctxt.tx_count++;
  }

  // stop T3411
  if (t3411.is_running()) {
    nas_log->debug("Stopping T3411\n");
    t3411.stop();
  }
}

void nas::gen_service_request(byte_buffer_t* msg)
{
  if (msg == nullptr) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in gen_service_request().\n");
    return;
  }

  nas_log->info("Generating service request\n");

  // Pack the service request message directly
  msg->msg[0] = (LIBLTE_MME_SECURITY_HDR_TYPE_SERVICE_REQUEST << 4u) | (LIBLTE_MME_PD_EPS_MOBILITY_MANAGEMENT);
  msg->N_bytes++;
  msg->msg[1] = (ctxt.ksi & 0x07u) << 5u;
  msg->msg[1] |= ctxt.tx_count & 0x1Fu;
  msg->N_bytes++;

  uint8_t mac[4];
  integrity_generate(&k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &msg->msg[0], 2, &mac[0]);
  // Set the short MAC
  msg->msg[2] = mac[2];
  msg->N_bytes++;
  msg->msg[3] = mac[3];
  msg->N_bytes++;

  if (pcap != nullptr) {
    pcap->write_nas(msg->msg, msg->N_bytes);
  }
  set_k_enb_count(ctxt.tx_count);
  ctxt.tx_count++;
}

void nas::gen_pdn_connectivity_request(LIBLTE_BYTE_MSG_STRUCT* msg)
{
  LIBLTE_MME_PDN_CONNECTIVITY_REQUEST_MSG_STRUCT pdn_con_req;
  ZERO_OBJECT(pdn_con_req);

  nas_log->info("Generating PDN Connectivity Request\n");

  // Set the PDN con req parameters
  pdn_con_req.eps_bearer_id       = 0x00; // Unassigned bearer ID
  pdn_con_req.proc_transaction_id = 0x01; // First transaction ID
  pdn_con_req.request_type        = LIBLTE_MME_REQUEST_TYPE_INITIAL_REQUEST;
  pdn_con_req.apn_present         = false;

  // Set PDN protocol type
  if (cfg.apn_protocol == "ipv4" || cfg.apn_protocol.empty()) {
    nas_log->debug("Requesting IPv4 PDN protocol\n");
    pdn_con_req.pdn_type = LIBLTE_MME_PDN_TYPE_IPV4;
  } else if (cfg.apn_protocol == "ipv6") {
    nas_log->debug("Requesting IPv6 PDN protocol\n");
    pdn_con_req.pdn_type = LIBLTE_MME_PDN_TYPE_IPV6;
  } else if (cfg.apn_protocol == "ipv4v6") {
    nas_log->debug("Requesting IPv4v6 PDN protocol\n");
    pdn_con_req.pdn_type = LIBLTE_MME_PDN_TYPE_IPV4V6;
  } else {
    nas_log->warning("Unsupported PDN prtocol. Defaulting to IPv4\n");
    nas_log->console("Unsupported PDN prtocol: %s. Defaulting to IPv4\n", cfg.apn_protocol.c_str());
    pdn_con_req.pdn_type = LIBLTE_MME_PDN_TYPE_IPV4;
  }

  // Set the optional flags
  if (cfg.apn_name.empty()) {
    pdn_con_req.esm_info_transfer_flag_present = false;
  } else {
    // request ESM info transfer is APN is specified
    pdn_con_req.esm_info_transfer_flag_present = true;
    pdn_con_req.esm_info_transfer_flag         = LIBLTE_MME_ESM_INFO_TRANSFER_FLAG_REQUIRED;
  }

  pdn_con_req.protocol_cnfg_opts_present = false;
  pdn_con_req.device_properties_present  = false;

  // Pack the message
  liblte_mme_pack_pdn_connectivity_request_msg(&pdn_con_req, msg);
}

void nas::send_security_mode_reject(uint8_t cause)
{
  unique_byte_buffer_t msg = srslte::allocate_unique_buffer(*pool, true);
  if (!msg) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_security_mode_reject().\n");
    return;
  }

  LIBLTE_MME_SECURITY_MODE_REJECT_MSG_STRUCT sec_mode_rej = {0};
  sec_mode_rej.emm_cause                                  = cause;
  liblte_mme_pack_security_mode_reject_msg(&sec_mode_rej, (LIBLTE_BYTE_MSG_STRUCT*)msg.get());
  if (pcap != nullptr) {
    pcap->write_nas(msg->msg, msg->N_bytes);
  }
  nas_log->info("Sending security mode reject\n");
  rrc->write_sdu(std::move(msg));
}

void nas::send_detach_request(bool switch_off)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  if (!pdu) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in %s().\n", __FUNCTION__);
    return;
  }

  LIBLTE_MME_DETACH_REQUEST_MSG_STRUCT detach_request;
  bzero(&detach_request, sizeof(detach_request));
  if (switch_off) {
    detach_request.detach_type.switch_off     = 1;
    detach_request.detach_type.type_of_detach = LIBLTE_MME_SO_FLAG_SWITCH_OFF;
  } else {
    detach_request.detach_type.switch_off     = 0;
    detach_request.detach_type.type_of_detach = LIBLTE_MME_TOD_UL_EPS_DETACH;
  }

  // GUTI or IMSI detach
  if (have_guti && have_ctxt) {
    detach_request.eps_mobile_id.type_of_id = LIBLTE_MME_EPS_MOBILE_ID_TYPE_GUTI;
    memcpy(&detach_request.eps_mobile_id.guti, &ctxt.guti, sizeof(LIBLTE_MME_EPS_MOBILE_ID_GUTI_STRUCT));
    detach_request.nas_ksi.tsc_flag = LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE;
    detach_request.nas_ksi.nas_ksi  = ctxt.ksi;
    nas_log->info("Sending detach request with GUTI\n"); // If sent as an Initial UE message, it cannot be chiphered
    liblte_mme_pack_detach_request_msg(&detach_request,
                                       rrc->is_connected() ? LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED
                                                           : LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY,
                                       ctxt.tx_count,
                                       (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());

    if (pcap != nullptr) {
      pcap->write_nas(pdu->msg, pdu->N_bytes);
    }

    // Add MAC
    if (pdu->N_bytes > 5) {
      if (rrc->is_connected()) {
        cipher_encrypt(pdu.get());
      }
      integrity_generate(
          &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
      ctxt.tx_count++;
    } else {
      nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
    }
  } else {
    detach_request.eps_mobile_id.type_of_id = LIBLTE_MME_EPS_MOBILE_ID_TYPE_IMSI;
    detach_request.nas_ksi.tsc_flag         = LIBLTE_MME_TYPE_OF_SECURITY_CONTEXT_FLAG_NATIVE;
    detach_request.nas_ksi.nas_ksi          = 0;
    usim->get_imsi_vec(detach_request.eps_mobile_id.imsi, 15);
    nas_log->info("Sending detach request with IMSI\n");
    liblte_mme_pack_detach_request_msg(
        &detach_request, LIBLTE_MME_SECURITY_HDR_TYPE_PLAIN_NAS, ctxt.tx_count, (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());

    if (pcap != nullptr) {
      pcap->write_nas(pdu->msg, pdu->N_bytes);
    }
  }

  if (switch_off) {
    enter_emm_deregistered();
  } else {
    // we are expecting a response from the core
    state = EMM_STATE_DEREGISTERED_INITIATED;
  }

  if (rrc->is_connected()) {
    rrc->write_sdu(std::move(pdu));
  } else {
    if (not rrc_connector.launch(establishment_cause_t::mo_sig, std::move(pdu))) {
      nas_log->error("Failed to initiate RRC Connection Request\n");
    }
    callbacks.add_proc(rrc_connector);
  }
}

void nas::send_attach_complete(const uint8_t& transaction_id, const uint8_t& eps_bearer_id)
{
  // Send EPS bearer context accept and attach complete
  LIBLTE_MME_ATTACH_COMPLETE_MSG_STRUCT                            attach_complete                   = {};
  LIBLTE_MME_ACTIVATE_DEFAULT_EPS_BEARER_CONTEXT_ACCEPT_MSG_STRUCT act_def_eps_bearer_context_accept = {};
  act_def_eps_bearer_context_accept.eps_bearer_id                                                    = eps_bearer_id;
  act_def_eps_bearer_context_accept.proc_transaction_id                                              = transaction_id;
  act_def_eps_bearer_context_accept.protocol_cnfg_opts_present                                       = false;
  liblte_mme_pack_activate_default_eps_bearer_context_accept_msg(&act_def_eps_bearer_context_accept,
                                                                 &attach_complete.esm_msg);

  // Pack entire message
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  liblte_mme_pack_attach_complete_msg(&attach_complete,
                                      LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
                                      ctxt.tx_count,
                                      (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());
  // Write NAS pcap
  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  cipher_encrypt(pdu.get());
  integrity_generate(
      &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);

  // Instruct RRC to enable capabilities
  rrc->enable_capabilities();

  nas_log->info("Sending Attach Complete\n");
  rrc->write_sdu(std::move(pdu));
  ctxt.tx_count++;
}

void nas::send_detach_accept()
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  if (!pdu) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in %s().\n", __FUNCTION__);
    return;
  }

  LIBLTE_MME_DETACH_ACCEPT_MSG_STRUCT detach_accept;
  bzero(&detach_accept, sizeof(detach_accept));
  liblte_mme_pack_detach_accept_msg(&detach_accept,
                                    LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
                                    ctxt.tx_count,
                                    (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  // Encrypt and add MAC
  if (pdu->N_bytes > 5) {
    cipher_encrypt(pdu.get());
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  } else {
    nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
  }

  nas_log->info("Sending detach accept\n");
  rrc->write_sdu(std::move(pdu));
}

void nas::send_authentication_response(const uint8_t* res, const size_t res_len, const uint8_t sec_hdr_type)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  if (!pdu) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_authentication_response().\n");
    return;
  }

  LIBLTE_MME_AUTHENTICATION_RESPONSE_MSG_STRUCT auth_res;
  bzero(&auth_res, sizeof(LIBLTE_MME_AUTHENTICATION_RESPONSE_MSG_STRUCT));

  for (uint32_t i = 0; i < res_len; i++) {
    auth_res.res[i] = res[i];
  }
  auth_res.res_len = res_len;
  liblte_mme_pack_authentication_response_msg(
      &auth_res, sec_hdr_type, ctxt.tx_count, (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  if (sec_hdr_type == LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED && pdu->N_bytes > 5) {
    cipher_encrypt(pdu.get());
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  }

  nas_log->info("Sending Authentication Response\n");
  rrc->write_sdu(std::move(pdu));
}

void nas::send_authentication_failure(const uint8_t cause, const uint8_t* auth_fail_param)
{
  unique_byte_buffer_t msg = srslte::allocate_unique_buffer(*pool, true);
  if (!msg) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_authentication_failure().\n");
    return;
  }

  LIBLTE_MME_AUTHENTICATION_FAILURE_MSG_STRUCT auth_failure;
  auth_failure.emm_cause = cause;
  if (auth_fail_param != nullptr) {
    memcpy(auth_failure.auth_fail_param, auth_fail_param, 14);
    nas_log->debug_hex(auth_failure.auth_fail_param, 14, "auth_failure.auth_fail_param\n");
    auth_failure.auth_fail_param_present = true;
  } else {
    auth_failure.auth_fail_param_present = false;
  }

  liblte_mme_pack_authentication_failure_msg(&auth_failure, (LIBLTE_BYTE_MSG_STRUCT*)msg.get());
  if (pcap != nullptr) {
    pcap->write_nas(msg->msg, msg->N_bytes);
  }
  nas_log->info("Sending authentication failure.\n");
  rrc->write_sdu(std::move(msg));
}

void nas::send_identity_response(const uint8 id_type, const uint8_t sec_hdr_type)
{
  LIBLTE_MME_ID_RESPONSE_MSG_STRUCT id_resp = {};

  switch (id_type) {
    case LIBLTE_MME_MOBILE_ID_TYPE_IMSI:
      id_resp.mobile_id.type_of_id = LIBLTE_MME_MOBILE_ID_TYPE_IMSI;
      usim->get_imsi_vec(id_resp.mobile_id.imsi, 15);
      break;
    case LIBLTE_MME_MOBILE_ID_TYPE_IMEI:
      id_resp.mobile_id.type_of_id = LIBLTE_MME_MOBILE_ID_TYPE_IMEI;
      usim->get_imei_vec(id_resp.mobile_id.imei, 15);
      break;
    default:
      nas_log->error("Unhandled ID type: %d\n", id_type);
      return;
  }

  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  if (!pdu) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_identity_response().\n");
    return;
  }

  liblte_mme_pack_identity_response_msg(&id_resp, sec_hdr_type, ctxt.tx_count, (LIBLTE_BYTE_MSG_STRUCT*)pdu.get());

  // add security if needed
  if (sec_hdr_type == LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED && pdu->N_bytes > 5) {
    cipher_encrypt(pdu.get());
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  rrc->write_sdu(std::move(pdu));
  ctxt.tx_count++;
}

void nas::send_service_request()
{
  unique_byte_buffer_t msg = srslte::allocate_unique_buffer(*pool, true);
  if (!msg) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_service_request().\n");
    return;
  }

  // Pack the service request message directly
  msg->msg[0] = (LIBLTE_MME_SECURITY_HDR_TYPE_SERVICE_REQUEST << 4u) | (LIBLTE_MME_PD_EPS_MOBILITY_MANAGEMENT);
  msg->N_bytes++;
  msg->msg[1] = (ctxt.ksi & 0x07u) << 5u;
  msg->msg[1] |= ctxt.tx_count & 0x1Fu;
  msg->N_bytes++;

  uint8_t mac[4];
  integrity_generate(&k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &msg->msg[0], 2, &mac[0]);
  // Set the short MAC
  msg->msg[2] = mac[2];
  msg->N_bytes++;
  msg->msg[3] = mac[3];
  msg->N_bytes++;

  if (pcap != nullptr) {
    pcap->write_nas(msg->msg, msg->N_bytes);
  }

  nas_log->info("Sending service request\n");
  rrc->write_sdu(std::move(msg));
  ctxt.tx_count++;
}

void nas::send_esm_information_response(const uint8 proc_transaction_id)
{
  LIBLTE_MME_ESM_INFORMATION_RESPONSE_MSG_STRUCT esm_info_resp;
  esm_info_resp.proc_transaction_id = proc_transaction_id;
  esm_info_resp.eps_bearer_id       = 0; // respone shall always have no bearer assigned

  if (cfg.apn_name == "") {
    esm_info_resp.apn_present = false;
  } else {
    nas_log->debug("Including APN %s in ESM info response\n", cfg.apn_name.c_str());
    esm_info_resp.apn_present = true;
    int len                   = std::min((int)cfg.apn_name.length(), LIBLTE_STRING_LEN - 1);
    strncpy(esm_info_resp.apn.apn, cfg.apn_name.c_str(), len);
    esm_info_resp.apn.apn[len] = '\0';
  }

  if (cfg.apn_user != "" && cfg.apn_user.length() < LIBLTE_STRING_LEN && cfg.apn_pass != "" &&
      cfg.apn_pass.length() < LIBLTE_STRING_LEN) {

    nas_log->debug("Including CHAP authentication for user %s in ESM info response\n", cfg.apn_user.c_str());

    // Generate CHAP challenge
    uint16_t len = 1 /* CHAP code */ + 1 /* ID */ + 2 /* complete length */ + 1 /* data value size */ +
                   16 /* data value */ + cfg.apn_user.length();

    uint8_t challenge[len];
    bzero(challenge, len * sizeof(uint8_t));
    challenge[0] = 0x01;    // challenge code
    challenge[1] = chap_id; // ID
    challenge[2] = (len >> 8u) & 0xffu;
    challenge[3] = len & 0xffu;
    challenge[4] = 16;

    // Append random challenge value
    for (int i = 0; i < 16; i++) {
      challenge[5 + i] = rand() & 0xFFu;
    }

    // add user as name field
    for (size_t i = 0; i < cfg.apn_user.length(); i++) {
      const char* name  = cfg.apn_user.c_str();
      challenge[21 + i] = name[i];
    }

    // Generate response
    uint8_t response[len];
    bzero(response, len * sizeof(uint8_t));
    response[0] = 0x02; // response code
    response[1] = chap_id;
    response[2] = (len >> 8u) & 0xffu;
    response[3] = len & 0xffu;
    response[4] = 16;

    // Generate response value
    uint16_t resp_val_len = 16 /* MD5 len */ + 1 /* ID */ + cfg.apn_pass.length();
    uint8_t  resp_val[resp_val_len];
    resp_val[0] = chap_id;

    // add secret
    for (size_t i = 0; i < cfg.apn_pass.length(); i++) {
      const char* pass = cfg.apn_pass.c_str();
      resp_val[1 + i]  = pass[i];
    }

    // copy original challenge behind secret
    uint8_t* chal_val = &challenge[5];
    memcpy(&resp_val[1 + cfg.apn_pass.length()], chal_val, 16);

    // Compute MD5 of resp_val and add to response
    security_md5(resp_val, resp_val_len, &response[5]);

    // add user as name field again
    for (size_t i = 0; i < cfg.apn_user.length(); i++) {
      const char* name = cfg.apn_user.c_str();
      response[21 + i] = name[i];
    }

    // Add challenge and response to ESM info response
    esm_info_resp.protocol_cnfg_opts_present   = true;
    esm_info_resp.protocol_cnfg_opts.opt[0].id = LIBLTE_MME_CONFIGURATION_PROTOCOL_OPTIONS_CHAP;
    memcpy(esm_info_resp.protocol_cnfg_opts.opt[0].contents, challenge, sizeof(challenge));
    esm_info_resp.protocol_cnfg_opts.opt[0].len = sizeof(challenge);

    esm_info_resp.protocol_cnfg_opts.opt[1].id = LIBLTE_MME_CONFIGURATION_PROTOCOL_OPTIONS_CHAP;
    memcpy(esm_info_resp.protocol_cnfg_opts.opt[1].contents, response, sizeof(response));
    esm_info_resp.protocol_cnfg_opts.opt[1].len = sizeof(response);
    esm_info_resp.protocol_cnfg_opts.N_opts     = 2;
  } else {
    esm_info_resp.protocol_cnfg_opts_present = false;
  }

  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);
  if (!pdu) {
    nas_log->error("Fatal Error: Couldn't allocate PDU in send_attach_request().\n");
    return;
  }

  if (liblte_mme_pack_esm_information_response_msg(&esm_info_resp,
                                                   LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
                                                   ctxt.tx_count,
                                                   (LIBLTE_BYTE_MSG_STRUCT*)pdu.get()) != LIBLTE_SUCCESS) {
    nas_log->error("Error packing ESM information response.\n");
    return;
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  cipher_encrypt(pdu.get());
  if (pdu->N_bytes > 5) {
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  } else {
    nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
    return;
  }

  nas_log->info_hex(pdu->msg, pdu->N_bytes, "Sending ESM information response\n");
  rrc->write_sdu(std::move(pdu));

  ctxt.tx_count++;
  chap_id++;
}

void nas::send_activate_dedicated_eps_bearer_context_accept(const uint8_t& proc_transaction_id,
                                                            const uint8_t& eps_bearer_id)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);

  LIBLTE_MME_ACTIVATE_DEDICATED_EPS_BEARER_CONTEXT_ACCEPT_MSG_STRUCT accept = {};

  accept.eps_bearer_id       = eps_bearer_id;
  accept.proc_transaction_id = proc_transaction_id;

  if (liblte_mme_pack_activate_dedicated_eps_bearer_context_accept_msg(
          &accept,
          LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
          ctxt.tx_count,
          (LIBLTE_BYTE_MSG_STRUCT*)pdu.get()) != LIBLTE_SUCCESS) {
    nas_log->error("Error packing Activate Dedicated EPS Bearer context accept.\n");
    return;
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  cipher_encrypt(pdu.get());
  if (pdu->N_bytes > 5) {
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  } else {
    nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
    return;
  }

  nas_log->info_hex(pdu->msg,
                    pdu->N_bytes,
                    "Sending Activate Dedicated EPS Bearer context accept (eps_bearer_id=%d, proc_id=%d)\n",
                    accept.eps_bearer_id,
                    accept.proc_transaction_id);
  rrc->write_sdu(std::move(pdu));

  ctxt.tx_count++;
}

void nas::send_deactivate_eps_bearer_context_accept(const uint8_t& proc_transaction_id, const uint8_t& eps_bearer_id)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);

  LIBLTE_MME_DEACTIVATE_EPS_BEARER_CONTEXT_ACCEPT_MSG_STRUCT accept = {};

  accept.eps_bearer_id       = eps_bearer_id;
  accept.proc_transaction_id = proc_transaction_id;

  if (liblte_mme_pack_deactivate_eps_bearer_context_accept_msg(&accept,
                                                               LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
                                                               ctxt.tx_count,
                                                               (LIBLTE_BYTE_MSG_STRUCT*)pdu.get()) != LIBLTE_SUCCESS) {
    nas_log->error("Error packing Aeactivate EPS Bearer context accept.\n");
    return;
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  cipher_encrypt(pdu.get());
  if (pdu->N_bytes > 5) {
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  } else {
    nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
    return;
  }

  nas_log->info_hex(pdu->msg,
                    pdu->N_bytes,
                    "Sending Deactivate EPS Bearer context accept (eps_bearer_id=%d, proc_id=%d)\n",
                    accept.eps_bearer_id,
                    accept.proc_transaction_id);
  rrc->write_sdu(std::move(pdu));

  ctxt.tx_count++;
}

void nas::send_modify_eps_bearer_context_accept(const uint8_t& proc_transaction_id, const uint8_t& eps_bearer_id)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);

  LIBLTE_MME_MODIFY_EPS_BEARER_CONTEXT_ACCEPT_MSG_STRUCT accept = {};

  accept.eps_bearer_id       = eps_bearer_id;
  accept.proc_transaction_id = proc_transaction_id;

  if (liblte_mme_pack_modify_eps_bearer_context_accept_msg(&accept,
                                                           LIBLTE_MME_SECURITY_HDR_TYPE_INTEGRITY_AND_CIPHERED,
                                                           ctxt.tx_count,
                                                           (LIBLTE_BYTE_MSG_STRUCT*)pdu.get()) != LIBLTE_SUCCESS) {
    nas_log->error("Error packing Modify EPS Bearer context accept.\n");
    return;
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  cipher_encrypt(pdu.get());
  if (pdu->N_bytes > 5) {
    integrity_generate(
        &k_nas_int[16], ctxt.tx_count, SECURITY_DIRECTION_UPLINK, &pdu->msg[5], pdu->N_bytes - 5, &pdu->msg[1]);
  } else {
    nas_log->error("Invalid PDU size %d\n", pdu->N_bytes);
    return;
  }

  nas_log->info_hex(pdu->msg,
                    pdu->N_bytes,
                    "Sending Modify EPS Bearer context accept (eps_bearer_id=%d, proc_id=%d)\n",
                    accept.eps_bearer_id,
                    accept.proc_transaction_id);
  rrc->write_sdu(std::move(pdu));

  ctxt.tx_count++;
}

void nas::send_activate_test_mode_complete(const uint8_t sec_hdr_type)
{
  unique_byte_buffer_t pdu = srslte::allocate_unique_buffer(*pool, true);

  if (liblte_mme_pack_activate_test_mode_complete_msg(
          (LIBLTE_BYTE_MSG_STRUCT*)pdu.get(), sec_hdr_type, ctxt.tx_count)) {
    nas_log->error("Error packing activate test mode complete.\n");
    return;
  }

  if (pcap != nullptr) {
    pcap->write_nas(pdu->msg, pdu->N_bytes);
  }

  nas_log->info_hex(pdu->msg, pdu->N_bytes, "Sending Activate test mode complete\n");
  rrc->write_sdu(std::move(pdu));

  ctxt.tx_count++;
}

/*******************************************************************************
 * Security context persistence file
 ******************************************************************************/

bool nas::read_ctxt_file(nas_sec_ctxt* ctxt_)
{
  std::ifstream file;
  if (ctxt_ == nullptr) {
    return false;
  }

  if (cfg.force_imsi_attach) {
    nas_log->info("Skip reading context from file.\n");
    return false;
  }

  file.open(".ctxt", std::ios::in);
  if (file.is_open()) {
    if (!readvar(file, "m_tmsi=", &ctxt_->guti.m_tmsi)) {
      return false;
    }
    if (!readvar(file, "mcc=", &ctxt_->guti.mcc)) {
      return false;
    }
    if (!readvar(file, "mnc=", &ctxt_->guti.mnc)) {
      return false;
    }
    if (!readvar(file, "mme_group_id=", &ctxt_->guti.mme_group_id)) {
      return false;
    }
    if (!readvar(file, "mme_code=", &ctxt_->guti.mme_code)) {
      return false;
    }
    if (!readvar(file, "tx_count=", &ctxt_->tx_count)) {
      return false;
    }
    if (!readvar(file, "rx_count=", &ctxt_->rx_count)) {
      return false;
    }
    if (!readvar(file, "int_alg=", &ctxt_->integ_algo)) {
      return false;
    }
    if (!readvar(file, "enc_alg=", &ctxt_->cipher_algo)) {
      return false;
    }
    if (!readvar(file, "ksi=", &ctxt_->ksi)) {
      return false;
    }

    if (!readvar(file, "k_asme=", ctxt_->k_asme, 32)) {
      return false;
    }

    file.close();
    have_guti = true;
    nas_log->info("Read GUTI from file "
                  "m_tmsi: %x, mcc: %x, mnc: %x, mme_group_id: %x, mme_code: %x\n",
                  ctxt_->guti.m_tmsi,
                  ctxt_->guti.mcc,
                  ctxt_->guti.mnc,
                  ctxt_->guti.mme_group_id,
                  ctxt_->guti.mme_code);
    have_ctxt = true;
    nas_log->info("Read security ctxt from file .ctxt. "
                  "ksi: %x, k_asme: %s, tx_count: %x, rx_count: %x, int_alg: %d, enc_alg: %d\n",
                  ctxt_->ksi,
                  hex_to_string(ctxt_->k_asme, 32).c_str(),
                  ctxt_->tx_count,
                  ctxt_->rx_count,
                  ctxt_->integ_algo,
                  ctxt_->cipher_algo);
    return true;
  }
  return false;
}

bool nas::write_ctxt_file(nas_sec_ctxt ctxt_)
{
  if (!have_guti || !have_ctxt) {
    return false;
  }
  std::ofstream file;
  file.open(".ctxt", std::ios::out | std::ios::trunc);
  if (file.is_open()) {
    file << "m_tmsi=" << (int)ctxt_.guti.m_tmsi << std::endl;
    file << "mcc=" << (int)ctxt_.guti.mcc << std::endl;
    file << "mnc=" << (int)ctxt_.guti.mnc << std::endl;
    file << "mme_group_id=" << (int)ctxt_.guti.mme_group_id << std::endl;
    file << "mme_code=" << (int)ctxt_.guti.mme_code << std::endl;
    file << "tx_count=" << (int)ctxt_.tx_count << std::endl;
    file << "rx_count=" << (int)ctxt_.rx_count << std::endl;
    file << "int_alg=" << (int)ctxt_.integ_algo << std::endl;
    file << "enc_alg=" << (int)ctxt_.cipher_algo << std::endl;
    file << "ksi=" << (int)ctxt_.ksi << std::endl;

    file << "k_asme=" << hex_to_string(ctxt_.k_asme, 32) << std::endl;

    nas_log->info("Saved GUTI to file "
                  "m_tmsi: %x, mcc: %x, mnc: %x, mme_group_id: %x, mme_code: %x\n",
                  ctxt_.guti.m_tmsi,
                  ctxt_.guti.mcc,
                  ctxt_.guti.mnc,
                  ctxt_.guti.mme_group_id,
                  ctxt_.guti.mme_code);
    nas_log->info("Saved security ctxt to file .ctxt. "
                  "ksi: %x, k_asme: %s, tx_count: %x, rx_count: %x, int_alg: %d, enc_alg: %d\n",
                  ctxt_.ksi,
                  hex_to_string(ctxt_.k_asme, 32).c_str(),
                  ctxt_.tx_count,
                  ctxt_.rx_count,
                  ctxt_.integ_algo,
                  ctxt_.cipher_algo);
    file.close();
    return true;
  }
  return false;
}

/*********************************************************************
 *   Conversion helpers
 ********************************************************************/
std::string nas::hex_to_string(uint8_t* hex, int size)
{
  std::stringstream ss;

  ss << std::hex << std::setfill('0');
  for (int i = 0; i < size; i++) {
    ss << std::setw(2) << static_cast<unsigned>(hex[i]);
  }
  return ss.str();
}

bool nas::string_to_hex(std::string hex_str, uint8_t* hex, uint32_t len)
{
  static const char* const lut     = "0123456789abcdef";
  uint32_t                 str_len = hex_str.length();
  if (str_len & 1) {
    return false; // uneven hex_str length
  }
  if (str_len > len * 2) {
    return false; // not enough space in hex buffer
  }
  for (uint32_t i = 0; i < str_len; i += 2) {
    char        a = hex_str[i];
    const char* p = std::lower_bound(lut, lut + 16, a);
    if (*p != a) {
      return false; // invalid char
    }
    char        b = hex_str[i + 1];
    const char* q = std::lower_bound(lut, lut + 16, b);
    if (*q != b) {
      return false; // invalid char
    }
    hex[i / 2] = ((p - lut) << 4u) | (q - lut);
  }
  return true;
}

std::string nas::emm_info_str(LIBLTE_MME_EMM_INFORMATION_MSG_STRUCT* info)
{
  std::stringstream ss;
  if (info->full_net_name_present) {
    ss << info->full_net_name.name;
  }
  if (info->short_net_name_present) {
    ss << " (" << info->short_net_name.name << ")";
  }
  if (info->utc_and_local_time_zone_present) {
    ss << " " << (int)info->utc_and_local_time_zone.day;
    ss << "/" << (int)info->utc_and_local_time_zone.month;
    ss << "/" << (int)info->utc_and_local_time_zone.year;
    ss << " " << (int)info->utc_and_local_time_zone.hour;
    ss << ":" << (int)info->utc_and_local_time_zone.minute;
    ss << ":" << (int)info->utc_and_local_time_zone.second;
    ss << " TZ:" << (int)info->utc_and_local_time_zone.tz;
  }
  return ss.str();
}

} // namespace srsue
