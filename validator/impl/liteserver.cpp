/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "liteserver.hpp"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "adnl/utils.hpp"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"
#include "vm/boc.h"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "shard.hpp"
#include "fabric.h"
#include <ctime>

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

td::int32 get_tl_tag(td::Slice slice) {
  return slice.size() >= 4 ? td::as<td::int32>(slice.data()) : -1;
}

void LiteQuery::run_query(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                          td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<LiteQuery>("litequery", std::move(data), std::move(manager), std::move(promise)).release();
}

LiteQuery::LiteQuery(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                     td::Promise<td::BufferSlice> promise)
    : query_(std::move(data)), manager_(std::move(manager)), promise_(std::move(promise)) {
  timeout_ = td::Timestamp::in(default_timeout_seconds);
}

void LiteQuery::abort_query(td::Status reason) {
  LOG(INFO) << "aborted liteserver query: " << reason.to_string();
  if (promise_) {
    promise_.set_error(std::move(reason));
  }
  stop();
}

bool LiteQuery::fatal_error(td::Status error) {
  abort_query(std::move(error));
  return false;
}

bool LiteQuery::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, std::move(err_msg)));
}

bool LiteQuery::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

void LiteQuery::alarm() {
  fatal_error(-503, "timeout");
}

bool LiteQuery::finish_query(td::BufferSlice result) {
  if (promise_) {
    promise_.set_result(std::move(result));
    stop();
    return true;
  } else {
    stop();
    return false;
  }
}

void LiteQuery::start_up() {
  alarm_timestamp() = timeout_;

  auto F = fetch_tl_object<ton::lite_api::Function>(std::move(query_), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  lite_api::downcast_call(
      *F.move_as_ok().get(),
      td::overloaded(
          [&](lite_api::liteServer_getTime& q) { this->perform_getTime(); },
          [&](lite_api::liteServer_getVersion& q) { this->perform_getVersion(); },
          [&](lite_api::liteServer_getMasterchainInfo& q) { this->perform_getMasterchainInfo(); },
          [&](lite_api::liteServer_getBlock& q) { this->perform_getBlock(ton::create_block_id(q.id_)); },
          [&](lite_api::liteServer_getBlockHeader& q) {
            this->perform_getBlockHeader(ton::create_block_id(q.id_), q.mode_);
          },
          [&](lite_api::liteServer_getState& q) { this->perform_getState(ton::create_block_id(q.id_)); },
          [&](lite_api::liteServer_getAccountState& q) {
            this->perform_getAccountState(ton::create_block_id(q.id_), static_cast<WorkchainId>(q.account_->workchain_),
                                          q.account_->id_);
          },
          [&](lite_api::liteServer_getOneTransaction& q) {
            this->perform_getOneTransaction(ton::create_block_id(q.id_),
                                            static_cast<WorkchainId>(q.account_->workchain_), q.account_->id_,
                                            static_cast<LogicalTime>(q.lt_));
          },
          [&](lite_api::liteServer_getTransactions& q) {
            this->perform_getTransactions(static_cast<WorkchainId>(q.account_->workchain_), q.account_->id_,
                                          static_cast<LogicalTime>(q.lt_), q.hash_, static_cast<unsigned>(q.count_));
          },
          [&](lite_api::liteServer_sendMessage& q) { this->perform_sendMessage(std::move(q.body_)); },
          [&](lite_api::liteServer_getShardInfo& q) {
            this->perform_getShardInfo(ton::create_block_id(q.id_),
                                       ShardIdFull{q.workchain_, static_cast<ShardId>(q.shard_)}, q.exact_);
          },
          [&](lite_api::liteServer_getAllShardsInfo& q) {
            this->perform_getAllShardsInfo(ton::create_block_id(q.id_));
          },
          [&](lite_api::liteServer_lookupBlock& q) {
            this->perform_lookupBlock(ton::create_block_id_simple(q.id_), q.mode_, q.lt_, q.utime_);
          },
          [&](lite_api::liteServer_listBlockTransactions& q) {
            this->perform_listBlockTransactions(ton::create_block_id(q.id_), q.mode_, q.count_,
                                                (q.mode_ & 128) ? q.after_->account_ : td::Bits256::zero(),
                                                static_cast<LogicalTime>((q.mode_ & 128) ? (q.after_->lt_) : 0));
          },
          [&](lite_api::liteServer_getConfigParams& q) {
            this->perform_getConfigParams(ton::create_block_id(q.id_), (q.mode_ & 0xfff) | 0x1000, q.param_list_);
          },
          [&](lite_api::liteServer_getConfigAll& q) {
            this->perform_getConfigParams(ton::create_block_id(q.id_), (q.mode_ & 0xfff) | 0x2000);
          },
          [&](lite_api::liteServer_getBlockProof& q) {
            this->perform_getBlockProof(ton::create_block_id(q.known_block_),
                                        q.mode_ & 1 ? ton::create_block_id(q.target_block_) : ton::BlockIdExt{},
                                        q.mode_);
          },
          [&](auto& obj) { this->abort_query(td::Status::Error(ErrorCode::protoviolation, "unknown query")); }));
}

void LiteQuery::perform_getTime() {
  LOG(INFO) << "started a getTime() liteserver query";
  td::int32 now = static_cast<td::int32>(std::time(nullptr));
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_currentTime>(now);
  finish_query(std::move(b));
}

void LiteQuery::perform_getVersion() {
  LOG(INFO) << "started a getVersion() liteserver query";
  td::int32 now = static_cast<td::int32>(std::time(nullptr));
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_version>(0, 0x100, 0, now);
  finish_query(std::move(b));
}

void LiteQuery::perform_getMasterchainInfo() {
  LOG(INFO) << "started a getMasterchainInfo() liteserver query";
  td::actor::send_closure_later(
      manager_, &ton::validator::ValidatorManager::get_top_masterchain_state_block,
      [Self = actor_id(this)](td::Result<std::pair<Ref<ton::validator::MasterchainState>, BlockIdExt>> res)->void {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          auto pair = res.move_as_ok();
          td::actor::send_closure_later(Self, &LiteQuery::continue_getMasterchainInfo, std::move(pair.first),
                                        pair.second);
        }
      });
}

void LiteQuery::continue_getMasterchainInfo(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid) {
  LOG(INFO) << "obtained data for getMasterchainInfo() : last block = " << blkid.to_str();
  auto mc_state_q = Ref<ton::validator::MasterchainStateQ>(std::move(mc_state));
  if (mc_state_q.is_null()) {
    fatal_error("cannot obtain a valid masterchain state");
    return;
  }
  auto zerostate_id = mc_state_q->get_zerostate_id();
  auto zs_tl = create_tl_object<lite_api::tonNode_zeroStateIdExt>(zerostate_id.workchain, zerostate_id.root_hash,
                                                                  zerostate_id.file_hash);
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_masterchainInfo>(
      ton::create_tl_lite_block_id(blkid), mc_state_q->root_hash(), std::move(zs_tl));
  finish_query(std::move(b));
}

void LiteQuery::perform_getBlock(BlockIdExt blkid) {
  LOG(INFO) << "started a getBlock(" << blkid.to_str() << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  td::actor::send_closure_later(manager_, &ValidatorManager::get_block_data_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<ton::validator::BlockData>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::continue_getBlock, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
}

void LiteQuery::continue_getBlock(BlockIdExt blkid, Ref<ton::validator::BlockData> block) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ")";
  CHECK(block.not_null());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockData>(ton::create_tl_lite_block_id(blkid),
                                                                                block->data());

  finish_query(std::move(b));
}

void LiteQuery::perform_getBlockHeader(BlockIdExt blkid, int mode) {
  LOG(INFO) << "started a getBlockHeader(" << blkid.to_str() << ", " << mode << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  td::actor::send_closure_later(manager_, &ValidatorManager::get_block_data_from_db_short, blkid,
                                [ Self = actor_id(this), blkid, mode ](td::Result<Ref<ton::validator::BlockData>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockHeader, blkid,
                                                                  mode, res.move_as_ok());
                                  }
                                });
}

static bool visit(Ref<vm::Cell> cell);

static bool visit(const vm::CellSlice& cs) {
  auto cnt = cs.size_refs();
  bool res = true;
  for (unsigned i = 0; i < cnt; i++) {
    res &= visit(cs.prefetch_ref(i));
  }
  return res;
}

static bool visit(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return true;
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  return visit(cs);
}

static bool visit(Ref<vm::CellSlice> cs_ref) {
  return cs_ref.is_null() || visit(*cs_ref);
}

void LiteQuery::continue_getBlockHeader(BlockIdExt blkid, int mode, Ref<ton::validator::BlockData> block) {
  LOG(INFO) << "obtained data for getBlockHeader(" << blkid.to_str() << ", " << mode << ")";
  CHECK(block.not_null());
  CHECK(block->block_id() == blkid);
  auto block_root = block->root_cell();
  if (block_root.is_null()) {
    fatal_error("block has no valid root cell");
    return;
  }
  // create block header proof
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == blkid.root_hash);
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(blk.info, info))) {
    fatal_error("cannot unpack block header");
    return;
  }
  if (mode & 1) {
    // with state_update
    vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
    if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
          && upd_cs.size_ext() == 0x20228)) {
      fatal_error("invalid Merkle update in block");
      return;
    }
  }
  visit(info.master_ref);
  visit(info.prev_ref);
  visit(info.prev_vert_ref);
  if (mode & 2) {
    // with value flow
    visit(blk.value_flow);
  }
  if (mode & 16) {
    // with extra
    block::gen::BlockExtra::Record extra;
    if (!tlb::unpack_cell(blk.extra, extra)) {
      fatal_error("cannot unpack BlockExtra in block");
      return;
    }
    if (blkid.is_masterchain()) {
      auto mc_extra_root = extra.custom->prefetch_ref();
      block::gen::McBlockExtra::Record mc_extra;
      if (!(mc_extra_root.not_null() && tlb::unpack_cell(std::move(mc_extra_root), mc_extra))) {
        fatal_error("cannot unpack McBlockExtra in block");
        return;
      }
      if (mode & 32) {
        // with ShardHashes
        visit(mc_extra.shard_hashes);
      }
      if (mode & 64) {
        // with PrevBlkSignatures
        visit(mc_extra.r1.prev_blk_signatures);
      }
    }
  }
  auto proof = vm::MerkleProof::generate(std::move(block_root), usage_tree.get());
  if (proof.is_null()) {
    fatal_error("cannot create Merkle proof for block header");
    return;
  }
  auto proof_data = vm::std_boc_serialize(std::move(proof));
  if (proof_data.is_error()) {
    fatal_error(proof_data.move_as_error());
    return;
  }
  // send answer
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockHeader>(ton::create_tl_lite_block_id(blkid),
                                                                                  mode, proof_data.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getState(BlockIdExt blkid) {
  LOG(INFO) << "started a getState(" << blkid.to_str() << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  if (blkid.is_masterchain() && blkid.id.seqno > 1000) {
    fatal_error("cannot request total state: possibly too large");
    return;
  }
  td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<ton::validator::ShardState>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::continue_getState, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
}

void LiteQuery::continue_getState(BlockIdExt blkid, Ref<ton::validator::ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ")";
  CHECK(state.not_null());
  auto res = state->serialize();
  if (res.is_error()) {
    abort_query(res.move_as_error());
    return;
  }
  auto data = res.move_as_ok();
  FileHash file_hash;
  td::sha256(data, file_hash.as_slice());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockState>(
      ton::create_tl_lite_block_id(blkid), state->root_hash(), file_hash, std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::perform_sendMessage(td::BufferSlice data) {
  LOG(INFO) << "started a sendMessage(<" << data.size() << " bytes>) liteserver query";
  auto res = ton::validator::create_ext_message(std::move(data));
  if (res.is_error()) {
    abort_query(res.move_as_error());
    return;
  }
  LOG(INFO) << "sending an external message to validator manager";
  td::actor::send_closure_later(manager_, &ValidatorManager::send_external_message, res.move_as_ok());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_sendMsgStatus>(1);
  finish_query(std::move(b));
}

bool LiteQuery::request_mc_block_data(BlockIdExt blkid) {
  if (!blkid.is_masterchain() || !blkid.is_valid_full()) {
    return fatal_error("reference block must belong to the masterchain");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  base_blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(manager_, &ValidatorManager::get_block_data_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<BlockData>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_data, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
  return true;
}

bool LiteQuery::request_mc_block_state(BlockIdExt blkid) {
  if (!blkid.is_masterchain() || !blkid.is_valid_full()) {
    return fatal_error("reference block must belong to the masterchain");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  base_blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<ShardState>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_state, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
  return true;
}

bool LiteQuery::request_mc_block_data_state(BlockIdExt blkid) {
  return request_mc_block_data(blkid) && request_mc_block_state(blkid);
}

bool LiteQuery::request_block_data_state(BlockIdExt blkid) {
  LOG(INFO) << "requesting state for block (" << blkid.to_str() << ")";
  return request_block_data(blkid) && request_block_state(blkid);
}

bool LiteQuery::request_block_state(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<ShardState>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::got_block_state, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
  return true;
}

bool LiteQuery::request_block_data(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(manager_, &ValidatorManager::get_block_data_from_db_short, blkid,
                                [ Self = actor_id(this), blkid ](td::Result<Ref<BlockData>> res) {
                                  if (res.is_error()) {
                                    td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                  } else {
                                    td::actor::send_closure_later(Self, &LiteQuery::got_block_data, blkid,
                                                                  res.move_as_ok());
                                  }
                                });
  return true;
}

void LiteQuery::perform_getAccountState(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr) {
  LOG(INFO) << "started a getAccountState(" << blkid.to_str() << ", " << workchain << ", " << addr.to_hex()
            << ") liteserver query";
  if (blkid.id.workchain != masterchainId && blkid.id.workchain != workchain) {
    fatal_error("reference block for a getAccountState() must belong to the masterchain");
    return;
  }
  //if (workchain != masterchainId && workchain != basechainId) {
  //  fatal_error("cannot get account states from specified workchain");
  //  return;
  //}
  if (!blkid.is_valid()) {
    fatal_error("reference block id for a getAccountState() is invalid");
    return;
  }
  if (workchain == blkid.id.workchain &&
      !ton::shard_contains(blkid.shard_full(), extract_addr_prefix(workchain, addr))) {
    fatal_error("requested account id is not contained in the shard of the reference block");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  if (blkid.id.workchain != masterchainId) {
    base_blk_id_ = blkid;
    set_continuation([&]() -> void { finish_getAccountState({}); });
    request_block_data_state(blkid);
  } else if (blkid.id.seqno != ~0U) {
    set_continuation([&]() -> void { continue_getAccountState(); });
    request_mc_block_data_state(blkid);
  } else {
    LOG(INFO) << "sending a get_top_masterchain_state_block query to manager";
    td::actor::send_closure_later(
        manager_, &ton::validator::ValidatorManager::get_top_masterchain_state_block,
        [Self = actor_id(this)](td::Result<std::pair<Ref<ton::validator::MasterchainState>, BlockIdExt>> res)->void {
          if (res.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          } else {
            auto pair = res.move_as_ok();
            td::actor::send_closure_later(Self, &LiteQuery::continue_getAccountState_0, std::move(pair.first),
                                          pair.second);
          }
        });
  }
}

void LiteQuery::continue_getAccountState_0(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid) {
  LOG(INFO) << "obtained last masterchain block = " << blkid.to_str();
  base_blk_id_ = blkid;
  CHECK(mc_state.not_null());
  mc_state_ = Ref<MasterchainStateQ>(std::move(mc_state));
  CHECK(mc_state_.not_null());
  set_continuation([&]() -> void { continue_getAccountState(); });
  request_mc_block_data(blkid);
}

void LiteQuery::perform_getOneTransaction(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, LogicalTime lt) {
  LOG(INFO) << "started a getOneTransaction(" << blkid.to_str() << ", " << workchain << ", " << addr.to_hex() << ","
            << lt << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("block id in getOneTransaction() is invalid");
    return;
  }
  if (!ton::shard_contains(blkid.shard_full(), extract_addr_prefix(workchain, addr))) {
    fatal_error("requested account id is not contained in the shard of the specified block");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  trans_lt_ = lt;
  set_continuation([&]() -> void { continue_getOneTransaction(); });
  request_block_data(blkid);
}

void LiteQuery::got_block_state(BlockIdExt blkid, Ref<ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(state.not_null());
  state_ = Ref<ShardStateQ>(std::move(state));
  CHECK(state_.not_null());
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::got_mc_block_state(BlockIdExt blkid, Ref<ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(state.not_null());
  mc_state_ = Ref<MasterchainStateQ>(std::move(state));
  CHECK(mc_state_.not_null());
  CHECK(blkid == base_blk_id_);
  dec_pending();
}

void LiteQuery::got_block_data(BlockIdExt blkid, Ref<BlockData> data) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(data.not_null());
  block_ = std::move(data);
  CHECK(block_.not_null());
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::got_mc_block_data(BlockIdExt blkid, Ref<BlockData> data) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(data.not_null());
  mc_block_ = std::move(data);
  CHECK(mc_block_.not_null());
  CHECK(blkid == base_blk_id_);
  dec_pending();
}

void LiteQuery::check_pending() {
  CHECK(pending_ >= 0);
  if (!pending_) {
    if (!cont_set_) {
      fatal_error("no continuation set for completion of data loading process");
    } else {
      cont_set_ = false;
      std::move(continuation_)();
    }
  }
}

bool LiteQuery::set_continuation(std::function<void()>&& cont) {
  if (cont_set_) {
    return fatal_error("continuation already set");
  } else {
    continuation_ = std::move(cont);
    return cont_set_ = true;
  }
}

bool LiteQuery::make_mc_state_root_proof(Ref<vm::Cell>& proof) {
  return make_state_root_proof(proof, mc_state_, mc_block_, base_blk_id_);
}

bool LiteQuery::make_state_root_proof(Ref<vm::Cell>& proof) {
  return make_state_root_proof(proof, state_, block_, blk_id_);
}

bool LiteQuery::make_state_root_proof(Ref<vm::Cell>& proof, Ref<ShardStateQ> state, Ref<BlockData> block,
                                      const BlockIdExt& blkid) {
  CHECK(block.not_null() && state.not_null());
  auto block_root = block->root_cell();
  auto state_root = state->root_cell();
  CHECK(block_root.not_null() && state_root.not_null());
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == blkid.root_hash);
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(blk.info, info))) {
    return fatal_error("cannot unpack block header");
  }
  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  auto upd_hash = upd_cs.prefetch_ref(1)->get_hash(0).bits();
  auto state_hash = state_root->get_hash().bits();
  if (upd_hash.compare(state_hash, 256)) {
    return fatal_error("cannot construct Merkle proof for given masterchain state because of hash mismatch");
  }
  proof = vm::MerkleProof::generate(block_root, usage_tree.get());
  if (proof.is_null()) {
    return fatal_error("unknown error creating Merkle proof");
  }
  return true;
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, vm::CellSlice& cs, ShardIdFull shard,
                                      ShardIdFull& true_shard, Ref<vm::Cell>& leaf, bool& found, bool exact) {
  auto state_root = mc_state_->root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(state_root, usage_tree->root_ptr());
  block::gen::ShardStateUnsplit::Record sstate;
  if (!(tlb::unpack_cell(usage_cell, sstate))) {
    return fatal_error("cannot unpack state header");
  }
  auto shards_dict = block::ShardConfig::extract_shard_hashes_dict(usage_cell);
  if (!shards_dict) {
    return fatal_error("cannot extract ShardHashes from last mc state");
  }
  found = block::ShardConfig::get_shard_hash_raw_from(*shards_dict, cs, shard, true_shard, exact, &leaf);
  proof = vm::MerkleProof::generate(state_root, usage_tree.get());
  if (proof.is_null()) {
    return fatal_error("unknown error creating Merkle proof");
  }
  return true;
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, ShardIdFull shard,
                                      bool exact) {
  Ref<vm::Cell> leaf;
  vm::CellSlice cs;
  ShardIdFull true_shard;
  bool found;
  if (!make_shard_info_proof(proof, cs, shard, true_shard, leaf, found, exact)) {
    return false;
  }
  if (found) {
    info = block::McShardHash::unpack(cs, true_shard);
    if (info.is_null()) {
      return fatal_error("cannot unpack a leaf entry from ShardHashes");
    }
  } else {
    info.clear();
  }
  return true;
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, AccountIdPrefixFull prefix) {
  return make_shard_info_proof(proof, info, prefix.as_leaf_shard(), false);
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, BlockIdExt& blkid, AccountIdPrefixFull prefix) {
  Ref<block::McShardHash> info;
  if (!make_shard_info_proof(proof, info, prefix)) {
    return false;
  }
  if (info.not_null()) {
    blkid = info->top_block_id();
  } else {
    blkid.invalidate();
  }
  return true;
}

void LiteQuery::continue_getAccountState() {
  LOG(INFO) << "continue getAccountState() query";
  if (acc_workchain_ == masterchainId) {
    blk_id_ = base_blk_id_;
    block_ = mc_block_;
    state_ = mc_state_;
    finish_getAccountState({});
    return;
  }
  Ref<vm::Cell> proof3, proof4;
  ton::BlockIdExt blkid;
  if (!(make_mc_state_root_proof(proof3) &&
        make_shard_info_proof(proof4, blkid, extract_addr_prefix(acc_workchain_, acc_addr_)))) {
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof3), std::move(proof4)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  if (!blkid.is_valid()) {
    // no shard with requested address found
    LOG(INFO) << "getAccountState(" << acc_workchain_ << ":" << acc_addr_.to_hex()
              << ") query completed (unknown workchain/shard)";
    auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_accountState>(
        ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blkid), proof.move_as_ok(),
        td::BufferSlice{}, td::BufferSlice{});
    finish_query(std::move(b));
  } else {
    shard_proof_ = proof.move_as_ok();
    set_continuation([this]() -> void { finish_getAccountState(std::move(shard_proof_)); });
    request_block_data_state(blkid);
  }
}

void LiteQuery::finish_getAccountState(td::BufferSlice shard_proof) {
  LOG(INFO) << "completing getAccountState() query";
  Ref<vm::Cell> proof1;
  if (!make_state_root_proof(proof1)) {
    return;
  }
  auto state_root = state_->root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(state_root, usage_tree->root_ptr());
  block::gen::ShardStateUnsplit::Record sstate;
  if (!(tlb::unpack_cell(usage_cell, sstate))) {
    fatal_error("cannot unpack state header");
    return;
  }
  vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(sstate.accounts), 256, block::tlb::aug_ShardAccounts};
  auto acc_csr = accounts_dict.lookup(acc_addr_);
  Ref<vm::Cell> acc_root;
  if (acc_csr.not_null()) {
    acc_root = acc_csr->prefetch_ref();
  }
  auto proof2 = vm::MerkleProof::generate(state_root, usage_tree.get());
  usage_tree.reset();
  usage_cell.clear();
  auto proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  td::BufferSlice data;
  if (acc_root.not_null()) {
    auto res = vm::std_boc_serialize(std::move(acc_root));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  }
  LOG(INFO) << "getAccountState(" << acc_workchain_ << ":" << acc_addr_.to_hex() << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_accountState>(
      ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blk_id_), std::move(shard_proof),
      proof.move_as_ok(), std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::continue_getOneTransaction() {
  LOG(INFO) << "completing getOneTransaction() query";
  CHECK(block_.not_null());
  auto block_root = block_->root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  auto trans_res = block::get_block_transaction(block_root, acc_workchain_, acc_addr_, trans_lt_);
  if (trans_res.is_error()) {
    fatal_error(trans_res.move_as_error());
    return;
  }
  auto trans_root = trans_res.move_as_ok();
  auto proof = vm::MerkleProof::generate(block_root, usage_tree.get());
  auto proof_boc = vm::std_boc_serialize(std::move(proof));
  if (proof_boc.is_error()) {
    fatal_error(proof_boc.move_as_error());
    return;
  }
  usage_tree.reset();
  usage_cell.clear();
  td::BufferSlice data;
  if (trans_root.not_null()) {
    auto res = vm::std_boc_serialize(std::move(trans_root));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  }
  LOG(INFO) << "getOneTransaction(" << acc_workchain_ << ":" << acc_addr_.to_hex() << "," << trans_lt_
            << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_transactionInfo>(
      ton::create_tl_lite_block_id(blk_id_), proof_boc.move_as_ok(), std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::perform_getTransactions(WorkchainId workchain, StdSmcAddress addr, LogicalTime lt, Bits256 hash,
                                        unsigned count) {
  LOG(INFO) << "started a getTransactions(" << workchain << ", " << addr.to_hex() << ", " << lt << ", " << hash.to_hex()
            << ", " << count << ") liteserver query";
  if (count > 10) {
    fatal_error("cannot fetch more than 10 preceding transactions at one time");
    return;
  }
  if (workchain == ton::workchainInvalid) {
    fatal_error("invalid workchain specified");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  trans_lt_ = lt;
  trans_hash_ = hash;
  continue_getTransactions(count, false);
}

void LiteQuery::continue_getTransactions(unsigned remaining, bool exact) {
  LOG(INFO) << "continue getTransactions() : " << remaining << " remaining";
  bool redo = true;
  while (remaining && redo && trans_lt_ && block_.not_null()) {
    redo = false;
    if (!ton::shard_contains(block_->block_id().shard_full(), ton::extract_addr_prefix(acc_workchain_, acc_addr_))) {
      fatal_error("obtained a block that cannot contain specified account");
      return;
    }
    auto res = block::get_block_transaction_try(block_->root_cell(), acc_workchain_, acc_addr_, trans_lt_);
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    auto root = res.move_as_ok();
    if (root.not_null()) {
      // transaction found
      if (trans_hash_ != root->get_hash().bits()) {
        if (!roots_.empty()) {
          LOG(ERROR) << "transaction hash mismatch: prev_trans_lt/hash invalid for " << acc_workchain_ << ":"
                     << acc_addr_.to_hex() << " lt=" << trans_lt_ << " hash=" << trans_hash_.to_hex()
                     << " found hash=" << root->get_hash().bits().to_hex(256);
        }
        fatal_error("transaction hash mismatch");
        return;
      }
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(root, trans)) {
        fatal_error("cannot unpack transaction");
        return;
      }
      if (trans.prev_trans_lt >= trans_lt_) {
        fatal_error("previous transaction time is not less than the current one");
        return;
      }
      roots_.push_back(std::move(root));
      aux_objs_.push_back(block_);
      blk_ids_.push_back(block_->block_id());
      LOG(DEBUG) << "going to previous transaction with lt=" << trans.prev_trans_lt << " from current lt=" << trans_lt_;
      trans_lt_ = trans.prev_trans_lt;
      trans_hash_ = trans.prev_trans_hash;
      redo = (trans_lt_ > 0);
      exact = false;
      --remaining;
      continue;
    } else if (exact) {
      LOG(DEBUG) << "could not find transaction " << trans_lt_ << " of " << acc_workchain_ << ':' << acc_addr_.to_hex()
                 << " in block " << block_->block_id().to_str();
      if (roots_.empty()) {
        fatal_error("cannot locate transaction in block with specified logical time");
        return;
      }
      finish_getTransactions();
      return;
    }
  }
  if (!remaining || !trans_lt_) {
    finish_getTransactions();
    return;
  }
  ++pending_;
  LOG(DEBUG) << "sending get_block_by_lt_from_db() query to manager for " << acc_workchain_ << ":" << acc_addr_.to_hex()
             << " " << trans_lt_;
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_by_lt_from_db, ton::extract_addr_prefix(acc_workchain_, acc_addr_),
      trans_lt_, [ Self = actor_id(this), remaining, manager = manager_ ](td::Result<BlockIdExt> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_getTransactions, res.move_as_error(), ton::BlockIdExt{});
        } else {
          auto blkid = res.move_as_ok();
          LOG(DEBUG) << "requesting data for block " << blkid.to_str();
          td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db_short, blkid,
                                        [Self, blkid, remaining](td::Result<Ref<BlockData>> res) {
                                          if (res.is_error()) {
                                            td::actor::send_closure(Self, &LiteQuery::abort_getTransactions,
                                                                    res.move_as_error(), blkid);
                                          } else {
                                            td::actor::send_closure_later(Self, &LiteQuery::continue_getTransactions_2,
                                                                          blkid, res.move_as_ok(), remaining);
                                          }
                                        });
        }
      });
}

void LiteQuery::continue_getTransactions_2(BlockIdExt blkid, Ref<BlockData> block, unsigned remaining) {
  LOG(INFO) << "getTransactions() : loaded block " << blkid.to_str();
  --pending_;
  CHECK(!pending_);
  CHECK(block.not_null());
  block_ = block;
  blk_id_ = blkid;
  continue_getTransactions(remaining, true);
}

void LiteQuery::abort_getTransactions(td::Status error, ton::BlockIdExt blkid) {
  LOG(INFO) << "getTransactions() : got error " << error.message() << " from manager";
  if (roots_.empty()) {
    if (blkid.is_valid()) {
      fatal_error(PSTRING() << "cannot load block " << blkid.to_str()
                            << " with specified transaction: " << error.message());
    } else {
      fatal_error(PSTRING() << "cannot compute block with specified transaction: " << error.message());
    }
  } else {
    pending_ = 0;
    finish_getTransactions();
  }
}

void LiteQuery::finish_getTransactions() {
  LOG(INFO) << "completing getTransactions() liteserver query";
  auto res = vm::std_boc_serialize_multi(std::move(roots_));
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  std::vector<ton::tl_object_ptr<ton::lite_api::tonNode_blockIdExt>> a;
  for (const auto& id : blk_ids_) {
    a.push_back(ton::create_tl_lite_block_id(id));
  }
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_transactionList>(std::move(a), res.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getShardInfo(BlockIdExt blkid, ShardIdFull shard, bool exact) {
  LOG(INFO) << "started a getShardInfo(" << blkid.to_str() << ", " << shard.to_str() << ", " << exact
            << ") liteserver query";
  if (!shard.is_valid()) {
    fatal_error("requested shard is invalid");
    return;
  }
  set_continuation([this, shard, exact]() -> void { continue_getShardInfo(shard, exact); });
  request_mc_block_data_state(blkid);
}

void LiteQuery::perform_getConfigParams(BlockIdExt blkid, int mode, std::vector<int> param_list) {
  LOG(INFO) << "started a getConfigParams(" << blkid.to_str() << ", " << mode << ", <list of " << param_list.size()
            << " parameters>) liteserver query";
  set_continuation([ this, mode, param_list = std::move(param_list) ]() mutable {
    continue_getConfigParams(mode, std::move(param_list));
  });
  request_mc_block_data_state(blkid);
}

void LiteQuery::continue_getConfigParams(int mode, std::vector<int> param_list) {
  LOG(INFO) << "completing getConfigParams(" << base_blk_id_.to_str() << ", " << mode << ", <list of "
            << param_list.size() << " parameters>) liteserver query";
  Ref<vm::Cell> proof1;
  if (!make_mc_state_root_proof(proof1)) {
    return;
  }
  auto state_root = mc_state_->root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(state_root, usage_tree->root_ptr());
  auto res = block::Config::extract_from_state(usage_cell, mode);
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto cfg = res.move_as_ok();
  if (!cfg) {
    fatal_error("cannot extract configuration from last mc state");
    return;
  }
  try {
    if (mode & 0x2000) {
      visit(cfg->get_root_cell());
    } else if (mode & 0x1000) {
      for (int i : param_list) {
        visit(cfg->get_config_param(i));
      }
    }
  } catch (vm::VmError& err) {
    fatal_error("error while traversing required configuration parameters: "s + err.get_msg());
    return;
  }
  auto proof2 = vm::MerkleProof::generate(state_root, usage_tree.get());
  usage_tree.reset();
  usage_cell.clear();
  auto res1 = vm::std_boc_serialize(std::move(proof1));
  if (res1.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res1.move_as_error().to_string());
    return;
  }
  auto res2 = vm::std_boc_serialize(std::move(proof2));
  if (res2.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res2.move_as_error().to_string());
    return;
  }
  LOG(INFO) << "getConfigParams() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_configInfo>(
      mode & 0xfff, ton::create_tl_lite_block_id(base_blk_id_), res1.move_as_ok(), res2.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getAllShardsInfo(BlockIdExt blkid) {
  LOG(INFO) << "started a getAllShardsInfo(" << blkid.to_str() << ") liteserver query";
  set_continuation([&]() -> void { continue_getAllShardsInfo(); });
  request_mc_block_data_state(blkid);
}

void LiteQuery::continue_getShardInfo(ShardIdFull shard, bool exact) {
  LOG(INFO) << "completing getShardInfo(" << shard.to_str() << ") query";
  Ref<vm::Cell> proof1, proof2, leaf;
  if (!make_mc_state_root_proof(proof1)) {
    return;
  }
  vm::CellSlice cs;
  ShardIdFull true_shard;
  bool found;
  if (!make_shard_info_proof(proof2, cs, shard, true_shard, leaf, found, exact)) {
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  BlockIdExt true_id;
  td::BufferSlice data;
  if (found) {
    auto shard_info = block::McShardHash::unpack(cs, true_shard);
    if (shard_info.is_null()) {
      fatal_error("cannot unpack a leaf entry from ShardHashes");
      return;
    }
    true_id = shard_info->top_block_id();
    auto res = vm::std_boc_serialize(std::move(leaf));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  } else {
    true_id.invalidate_clear();
  }
  LOG(INFO) << "getShardInfo() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_shardInfo>(
      ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(true_id), proof.move_as_ok(),
      std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::continue_getAllShardsInfo() {
  LOG(INFO) << "completing getAllShardsInfo() query";
  Ref<vm::Cell> proof1;
  if (!make_mc_state_root_proof(proof1)) {
    return;
  }
  auto state_root = mc_state_->root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(state_root, usage_tree->root_ptr());
  auto shards_dict = block::ShardConfig::extract_shard_hashes_dict(usage_cell);
  if (!shards_dict) {
    fatal_error("cannot extract ShardHashes from last mc state");
    return;
  }
  auto proof2 = vm::MerkleProof::generate(state_root, usage_tree.get());
  usage_tree.reset();
  usage_cell.clear();
  shards_dict = block::ShardConfig::extract_shard_hashes_dict(state_root);
  vm::CellBuilder cb;
  Ref<vm::Cell> cell;
  if (!(std::move(shards_dict)->append_dict_to_bool(cb) && cb.finalize_to(cell))) {
    fatal_error("cannot store ShardHashes from last mc state into a new cell");
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  auto data = vm::std_boc_serialize(std::move(cell));
  if (data.is_error()) {
    fatal_error(data.move_as_error());
    return;
  }
  LOG(INFO) << "getAllShardInfo() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_allShardsInfo>(
      ton::create_tl_lite_block_id(base_blk_id_), proof.move_as_ok(), data.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_lookupBlock(BlockId blkid, int mode, LogicalTime lt, UnixTime utime) {
  if (!((1 << (mode & 7)) & 0x16)) {
    fatal_error("exactly one of mode.0, mode.1 and mode.2 bits must be set");
    return;
  }
  if (!(mode & 2)) {
    lt = 0;
  }
  if (!(mode & 4)) {
    utime = 0;
  }
  LOG(INFO) << "performing a lookupBlock(" << blkid.to_str() << ", " << mode << ", " << lt << ", " << utime
            << ") query";
  auto P = td::PromiseCreator::lambda(
      [ Self = actor_id(this), manager = manager_, mode = (mode >> 4) ](td::Result<BlockIdExt> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          auto blkid = res.move_as_ok();
          LOG(DEBUG) << "requesting data for block " << blkid.to_str();
          td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db_short, blkid,
                                        [Self, blkid, mode](td::Result<Ref<BlockData>> res) {
                                          if (res.is_error()) {
                                            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                          } else {
                                            td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockHeader,
                                                                          blkid, mode, res.move_as_ok());
                                          }
                                        });
        }
      });

  ton::AccountIdPrefixFull pfx{blkid.workchain, blkid.shard};
  if (mode & 2) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_lt_from_db, pfx, lt, std::move(P));
  } else if (mode & 4) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_unix_time_from_db, pfx, utime,
                                  std::move(P));
  } else {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_seqno_from_db, pfx, blkid.seqno,
                                  std::move(P));
  }
}

void LiteQuery::perform_listBlockTransactions(BlockIdExt blkid, int mode, int count, Bits256 account, LogicalTime lt) {
  LOG(INFO) << "started a listBlockTransactions(" << blkid.to_str() << ", " << mode << ", " << count << ", "
            << account.to_hex() << ", " << lt << ") liteserver query";
  base_blk_id_ = blkid;
  acc_addr_ = account;
  trans_lt_ = lt;
  set_continuation([this, mode, count]() -> void { finish_listBlockTransactions(mode, count); });
  request_block_data(blkid);
}

void LiteQuery::finish_listBlockTransactions(int mode, int req_count) {
  LOG(INFO) << "completing a listBlockTransactions(" << base_blk_id_.to_str() << ", " << mode << ", " << req_count
            << ", " << acc_addr_.to_hex() << ", " << trans_lt_ << ") liteserver query";
  constexpr int max_answer_transactions = 256;
  CHECK(block_.not_null());
  auto block_root = block_->root_cell();
  CHECK(block_root.not_null());
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == base_blk_id_.root_hash);
  Ref<vm::Cell> usage_cell;
  std::shared_ptr<vm::CellUsageTree> usage_tree;
  if (mode & 32) {
    // proof requested
    usage_tree = std::make_shared<vm::CellUsageTree>();
    usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  } else {
    usage_cell = block_root;
  }
  if ((mode & 192) == 64) {  // reverse order, no starting point
    acc_addr_.set_ones();
    trans_lt_ = ~0ULL;
  }
  std::vector<tl_object_ptr<lite_api::liteServer_transactionId>> result;
  bool eof = false;
  ton::LogicalTime reverse = (mode & 64) ? ~0ULL : 0;
  try {
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(std::move(blk.extra), extra))) {
      fatal_error("cannot find account transaction data in block "s + base_blk_id_.to_str());
      return;
    }
    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    int count = 0;
    bool allow_same = true;
    td::Bits256 cur_addr = acc_addr_;
    while (!eof && count < req_count && count < max_answer_transactions) {
      Ref<vm::CellSlice> value;
      try {
        value = acc_dict.extract_value(
            acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, !reverse, allow_same));
      } catch (vm::VmError err) {
        fatal_error("error while traversing account block dictionary: "s + err.get_msg());
        return;
      }
      if (value.is_null()) {
        eof = true;
        break;
      }
      allow_same = false;
      if (cur_addr != acc_addr_) {
        trans_lt_ = reverse;
      }
      block::gen::AccountBlock::Record acc_blk;
      if (!(tlb::csr_unpack(std::move(value), acc_blk) && acc_blk.account_addr == cur_addr)) {
        fatal_error("invalid AccountBlock for account "s + cur_addr.to_hex());
        return;
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
      td::BitArray<64> cur_trans{(long long)trans_lt_};
      while (count < req_count && count < max_answer_transactions) {
        Ref<vm::Cell> tvalue;
        try {
          tvalue = trans_dict.extract_value_ref(
              trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, !reverse));
        } catch (vm::VmError err) {
          fatal_error("error while traversing transaction dictionary of an AccountBlock: "s + err.get_msg());
          return;
        }
        if (tvalue.is_null()) {
          trans_lt_ = reverse;
          break;
        }
        result.push_back(create_tl_object<lite_api::liteServer_transactionId>(mode, cur_addr, cur_trans.to_long(),
                                                                              tvalue->get_hash().bits()));
        ++count;
      }
    }
  } catch (vm::VmError err) {
    fatal_error("error while parsing AccountBlocks of block "s + base_blk_id_.to_str() + " : " + err.get_msg());
    return;
  }
  td::BufferSlice proof_data;
  if (mode & 32) {
    // create proof
    auto proof = vm::MerkleProof::generate(block_root, usage_tree.get());
    auto proof_boc = vm::std_boc_serialize(std::move(proof));
    if (proof_boc.is_error()) {
      fatal_error(proof_boc.move_as_error());
      return;
    }
    proof_data = proof_boc.move_as_ok();
  }

  LOG(INFO) << "listBlockTransactions() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockTransactions>(
      ton::create_tl_lite_block_id(base_blk_id_), req_count, !eof, std::move(result), std::move(proof_data));
  finish_query(std::move(b));
}

void LiteQuery::perform_getBlockProof(ton::BlockIdExt from, ton::BlockIdExt to, int mode) {
  if (!(mode & 1)) {
    to.invalidate_clear();
  }
  LOG(INFO) << "performing a getBlockProof(" << mode << ", " << from.to_str() << ", " << to.to_str() << ") query";
  if (!from.is_masterchain_ext()) {
    fatal_error("source block "s + from.to_str() + " is not a valid masterchain block id");
    return;
  }
  if ((mode & 1) && !to.is_masterchain_ext()) {
    fatal_error("destination block "s + to.to_str() + " is not a valid masterchain block id");
    return;
  }
  if (!(mode & 1)) {
    td::actor::send_closure_later(
        manager_, &ton::validator::ValidatorManager::get_top_masterchain_state_block,
        [ Self = actor_id(this), from, mode ](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res)->void {
          if (res.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          } else {
            auto pair = res.move_as_ok();
            td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockProof, from, pair.second, mode,
                                          Ref<MasterchainStateQ>(std::move(pair.first)));
          }
        });
  } else {
    base_blk_id_ = (from.seqno() > to.seqno()) ? from : to;
    td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, base_blk_id_,
                                  [ Self = actor_id(this), from, to, mode ](td::Result<Ref<ShardState>> res) {
                                    if (res.is_error()) {
                                      td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                    } else {
                                      td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockProof, from, to,
                                                                    mode, Ref<MasterchainStateQ>(res.move_as_ok()));
                                    }
                                  });
  }
}

void LiteQuery::continue_getBlockProof(ton::BlockIdExt from, ton::BlockIdExt to, int mode,
                                       Ref<MasterchainStateQ> state) {
  if (mode & 1) {
    base_blk_id_ = to;
    if (!to.is_masterchain_ext()) {
      fatal_error("last masterchain block id "s + to.to_str() + " is invalid");
      return;
    }
    if (from.seqno() > to.seqno()) {
      fatal_error("client knows block "s + from.to_str() + " newer than the latest masterchain block " + to.to_str());
      return;
    }
  }
  if (state.is_null()) {
    fatal_error("obtained no valid masterchain state for block "s + base_blk_id_.to_str());
    return;
  }
  mc_state0_ = Ref<MasterchainStateQ>(state);
  if (base_blk_id_ != state->get_block_id()) {
    fatal_error("the state for "s + base_blk_id_.to_str() + " is in fact a state for different block " +
                state->get_block_id().to_str());
    return;
  }
  LOG(INFO) << "continuing getBlockProof(" << mode << ", " << from.to_str() << ", " << to.to_str()
            << ") query with a state for " << base_blk_id_.to_str();
  if (!state->check_old_mc_block_id(from)) {
    fatal_error("source masterchain block "s + from.to_str() + " is unknown from the perspective of newer block " +
                base_blk_id_.to_str());
    return;
  }
  if (!state->check_old_mc_block_id(to)) {
    fatal_error("destination masterchain block "s + to.to_str() + " is unknown from the perspective of newer block " +
                base_blk_id_.to_str());
    return;
  }
  chain_ = std::make_unique<block::BlkProofChain>(from, to, mode);
  blk_id_ = from;
  construct_proof_chain(from);
}

bool LiteQuery::construct_proof_chain(ton::BlockIdExt id) {
  CHECK(chain_);
  if (chain_->link_count() >= 16 || id == chain_->to) {
    return finish_proof_chain(std::move(id));
  }
  if (chain_->to.seqno() == id.seqno()) {
    return fatal_error("cannot have two different masterchain blocks "s + chain_->to.to_str() + " and " + id.to_str() +
                       " of the same height");
  }
  if (chain_->to.seqno() < id.seqno()) {
    return construct_proof_link_back(id, chain_->to);
  }
  auto prev_key_blk = mc_state0_->prev_key_block_id(id.seqno());
  if (!prev_key_blk.is_masterchain_ext()) {
    return fatal_error("cannot compute previous key block for "s + id.to_str());
  }
  if (prev_key_blk.seqno() > id.seqno() || (prev_key_blk.seqno() == id.seqno() && prev_key_blk != id)) {
    return fatal_error("block "s + prev_key_blk.to_str() + " cannot be the previous key block for " + id.to_str());
  }
  if (prev_key_blk.seqno() != id.seqno()) {
    return construct_proof_link_back(id, prev_key_blk);
  }
  auto next_key_blk = mc_state0_->next_key_block_id(id.seqno() + 1);
  if (next_key_blk.is_valid()) {
    if (!(next_key_blk.is_masterchain_ext() && next_key_blk.seqno() > id.seqno())) {
      return fatal_error("block "s + next_key_blk.to_str() + " cannot be the next key block after " + id.to_str());
    }
    return construct_proof_link_forward(id, next_key_blk);
  } else {
    return construct_proof_link_forward(id, chain_->to);
  }
}

bool LiteQuery::construct_proof_link_forward(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "constructing a forward proof link from " << cur.to_str() << " to " << next.to_str();
  if (!(cur.is_masterchain_ext() && next.is_masterchain_ext() && mc_state0_->check_old_mc_block_id(cur) &&
        mc_state0_->check_old_mc_block_id(next))) {
    return fatal_error("cannot construct forward proof link from "s + cur.to_str() + " to " + next.to_str() +
                       " because these are not known masterchain block ids");
  }
  if (cur.seqno() >= next.seqno()) {
    return fatal_error("cannot construct forward proof link from "s + cur.to_str() + " to " + next.to_str());
  }
  set_continuation([this, cur, next]() { construct_proof_link_back_cont(cur, next); });
  return request_mc_block_data(cur);
}

bool LiteQuery::construct_proof_link_forward_cont(ton::BlockIdExt cur, ton::BlockIdExt next) {
  // ...
  return fatal_error("construction of forward proof links not implemented yet");
}

bool LiteQuery::construct_proof_link_back(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "constructing a backward proof link from " << cur.to_str() << " to " << next.to_str();
  if (!(cur.is_masterchain_ext() && next.is_masterchain_ext() && mc_state0_->check_old_mc_block_id(cur) &&
        mc_state0_->check_old_mc_block_id(next))) {
    return fatal_error("cannot construct backward proof link from "s + cur.to_str() + " to " + next.to_str() +
                       " because these are not known masterchain block ids");
  }
  if (cur.seqno() <= next.seqno()) {
    return fatal_error("cannot construct backward proof link from "s + cur.to_str() + " to " + next.to_str());
  }
  set_continuation([this, cur, next]() { construct_proof_link_back_cont(cur, next); });
  return request_mc_block_data_state(cur);
}

bool LiteQuery::construct_proof_link_back_cont(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "continue constructing a backward proof link from " << cur.to_str() << " to " << next.to_str();
  CHECK(mc_state_.not_null() && mc_block_.not_null() && mc_state_->get_block_id() == cur &&
        mc_block_->block_id() == cur);
  // ...
  return fatal_error("construction of backward proof links not implemented yet");
}

bool LiteQuery::finish_proof_chain(ton::BlockIdExt id) {
  CHECK(chain_);
  LOG(INFO) << "finish constructing block proof chain from " << chain_->from.to_str() << " to " << chain_->to.to_str()
            << " (constructed " << chain_->link_count() << " up to " << id.to_str() << ")";
  // bool complete = (id == chain_->to);
  chain_->to = id;
  // serialize answer
  // ...
  return fatal_error("cannot serialize PartialBlockProof from "s + chain_->from.to_str() + " to " + id.to_str());
}

}  // namespace validator
}  // namespace ton