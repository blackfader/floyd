// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "floyd/src/floyd_primary_thread.h"

#include <stdlib.h>
#include <time.h>
#include <google/protobuf/text_format.h>

#include <climits>
#include <algorithm>
#include <vector>

#include "slash/include/env.h"
#include "slash/include/slash_mutex.h"

#include "floyd/src/floyd_peer_thread.h"
#include "floyd/src/floyd_apply.h"
#include "floyd/src/floyd_context.h"
#include "floyd/src/floyd_client_pool.h"
#include "floyd/src/raft_log.h"
#include "floyd/src/floyd.pb.h"
#include "floyd/src/logger.h"
#include "floyd/include/floyd_options.h"

namespace floyd {

FloydPrimary::FloydPrimary(FloydContext* context, FloydApply* apply, const Options& options, Logger* info_log)
  : context_(context),
    apply_(apply),
    options_(options),
    info_log_(info_log) {
}

int FloydPrimary::Start() {
  bg_thread_.set_thread_name("FloydPrimary");
  return bg_thread_.StartThread();
}

FloydPrimary::~FloydPrimary() {
  LOGV(INFO_LEVEL, info_log_, "FloydPrimary exit!!!");
}

void FloydPrimary::set_peers(PeersSet* peers) {
  LOGV(DEBUG_LEVEL, info_log_, "FloydPrimary::set_peers peers "
       "has %d pairs", peers->size());
  peers_ = peers;
}

// TODO(anan) We keep 2 Primary Cron in total.
//    1. one short live Cron for LeaderHeartbeat, which is available as a leader;
//    2. another long live Cron for ElectLeaderCheck, which is started when
//    creating Primary;
void FloydPrimary::AddTask(TaskType type, bool is_delay, void* arg) {
  switch (type) {
  case kHeartBeat: {
    LOGV(DEBUG_LEVEL, info_log_, "FloydPrimary::AddTask HeartBeat");
    uint64_t timeout = options_.heartbeat_us;
    bg_thread_.DelaySchedule(timeout / 1000LL, LaunchHeartBeatWrapper, this);
    break;
  }
  case kCheckLeader: {
    LOGV(DEBUG_LEVEL, info_log_, "FloydPrimary::AddTask CheckLeader");
    uint64_t timeout = options_.check_leader_us;
    bg_thread_.DelaySchedule(timeout / 1000LL, LaunchCheckLeaderWrapper, this);
    break;
  }
  case kNewCommand: {
    LOGV(DEBUG_LEVEL, info_log_, "FloydPrimary::AddTask NewCommand");
    bg_thread_.Schedule(LaunchNewCommandWrapper, this);
    break;
  }
  default: {
    LOGV(WARN_LEVEL, info_log_, "FloydPrimary:: unknown task type %d", type);
    break;
  }
  }
}

void FloydPrimary::LaunchHeartBeatWrapper(void *arg) {
  reinterpret_cast<FloydPrimary *>(arg)->LaunchHeartBeat();
}

void FloydPrimary::LaunchHeartBeat() {
  slash::MutexLock l(&context_->commit_mu);
  if (context_->role == Role::kLeader) {
    NoticePeerTask(kHeartBeat);
    AddTask(kHeartBeat);
  }
}

void FloydPrimary::LaunchCheckLeaderWrapper(void *arg) {
  reinterpret_cast<FloydPrimary *>(arg)->LaunchCheckLeader();
}
void FloydPrimary::LaunchCheckLeader() {
  slash::MutexLock l(&context_->commit_mu);
  if (context_->role == Role::kFollower || context_->role == Role::kCandidate) {
    if (options_.single_mode) {
      context_->BecomeLeader();
    } else if (context_->last_op_time + options_.check_leader_us < slash::NowMicros()) {
      context_->BecomeCandidate();
      NoticePeerTask(kHeartBeat);
    }
  }
  AddTask(kCheckLeader);
}

void FloydPrimary::LaunchNewCommandWrapper(void *arg) {
  reinterpret_cast<FloydPrimary *>(arg)->LaunchNewCommand();
}
void FloydPrimary::LaunchNewCommand() {
  LOGV(DEBUG_LEVEL, info_log_, "FloydPrimary::LaunchNewCommand");
  if (context_->role != Role::kLeader) {
    LOGV(WARN_LEVEL, info_log_, "FloydPrimary::LaunchNewCommand, Not leader yet");
    return;
  }
  NoticePeerTask(kNewCommand);
}

// when adding task to peer thread, we can consider that this job have been in the network
// even it is still in the peer thread's queue
void FloydPrimary::NoticePeerTask(TaskType type) {
  for (auto& peer : *peers_) {
    switch (type) {
    case kHeartBeat:
      peer.second->AddRequestVoteTask();
      break;
    case kNewCommand:
      peer.second->AddAppendEntriesTask();
      break;
    default:
      LOGV(WARN_LEVEL, info_log_, "Error TaskType to notice peer");
    }
  }
}

} // namespace floyd
