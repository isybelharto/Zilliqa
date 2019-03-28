/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <array>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <boost/multiprecision/cpp_int.hpp>
#pragma GCC diagnostic pop
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libNetwork/Guard.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"
#include "libUtils/TimestampVerifier.h"

using namespace std;
using namespace boost::multiprecision;

bool Node::VerifyVCBlockCoSignature(const VCBlock& vcblock) {
  LOG_MARKER();

  unsigned int index = 0;
  unsigned int count = 0;

  const vector<bool>& B2 = vcblock.GetB2();
  if (m_mediator.m_DSCommittee->size() != B2.size()) {
    LOG_GENERAL(WARNING, "Mismatch: DS committee size = "
                             << m_mediator.m_DSCommittee->size()
                             << ", co-sig bitmap size = " << B2.size());
    return false;
  }

  // Generate the aggregated key
  vector<PubKey> keys;

  for (auto const& kv : *m_mediator.m_DSCommittee) {
    if (B2.at(index)) {
      keys.emplace_back(kv.first);
      count++;
    }
    index++;
  }

  if (count != ConsensusCommon::NumForConsensus(B2.size())) {
    LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
    return false;
  }

  shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
  if (aggregatedKey == nullptr) {
    LOG_GENERAL(WARNING, "Aggregated key generation failed");
    return false;
  }

  // Verify the collective signature
  bytes message;
  if (!vcblock.GetHeader().Serialize(message, 0)) {
    LOG_GENERAL(WARNING, "VCBlockHeader serialization failed");
    return false;
  }
  vcblock.GetCS1().Serialize(message, message.size());
  BitVector::SetBitVector(message, message.size(), vcblock.GetB1());
  if (!MultiSig::GetInstance().MultiSigVerify(
          message, 0, message.size(), vcblock.GetCS2(), *aggregatedKey)) {
    LOG_GENERAL(WARNING, "Cosig verification failed. Pubkeys");
    for (auto& kv : keys) {
      LOG_GENERAL(WARNING, kv);
    }
    return false;
  }

  return true;
}

bool Node::ProcessVCBlock(const bytes& message, unsigned int cur_offset,
                          [[gnu::unused]] const Peer& from) {
  LOG_MARKER();

  VCBlock vcblock;

  if (!Messenger::GetNodeVCBlock(message, cur_offset, vcblock)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetNodeVCBlock failed.");
    return false;
  }

  if (vcblock.GetHeader().GetVersion() != VCBLOCK_VERSION) {
    LOG_CHECK_FAIL("VCBlock version", vcblock.GetHeader().GetVersion(),
                   VCBLOCK_VERSION);
    return false;
  }

  // Check whether is this function called before ds block. VC block before ds
  // block should be processed seperately.
  if (m_mediator.m_ds->IsDSBlockVCState(
          vcblock.GetHeader().GetViewChangeState())) {
    LOG_GENERAL(WARNING,
                "Shard node shouldn't process vc block before ds block. It "
                "should process it together with ds block. cur epoch: "
                    << m_mediator.m_currentEpochNum << "vc epoch: "
                    << vcblock.GetHeader().GetViewChangeEpochNo());
    return false;
  }

  if (!ProcessVCBlockCore(vcblock)) {
    return false;
  }

  if (!LOOKUP_NODE_MODE && BROADCAST_TREEBASED_CLUSTER_MODE) {
    // Avoid using the original message for broadcasting in case it contains
    // excess data beyond the VCBlock
    bytes message2 = {MessageType::NODE, NodeInstructionType::VCBLOCK};
    if (!Messenger::SetNodeVCBlock(message2, MessageOffset::BODY, vcblock)) {
      LOG_GENERAL(WARNING, "Messenger::SetNodeVCBlock failed");
    } else {
      SendVCBlockToOtherShardNodes(message2);
    }
  }

  LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
            "I am a node and my view of leader is successfully changed.");
  return true;
}

// VC Core function to process one vc block
bool Node::ProcessVCBlockCore(const VCBlock& vcblock) {
  LOG_MARKER();

  if (vcblock.GetHeader().GetViewChangeEpochNo() !=
      m_mediator.m_currentEpochNum) {
    LOG_GENERAL(WARNING,
                "Node should received individual vc block for ds block ");
    return false;
  }

  // TODO State machine check

  // Check is block latest
  if (!m_mediator.CheckWhetherBlockIsLatest(
          vcblock.GetHeader().GetViewChangeDSEpochNo(),
          vcblock.GetHeader().GetViewChangeEpochNo())) {
    LOG_GENERAL(WARNING, "ProcessVCBlockCore CheckWhetherBlockIsLatest failed");
    return false;
  }

  // Verify the Block Hash
  BlockHash temp_blockHash = vcblock.GetHeader().GetMyHash();
  if (temp_blockHash != vcblock.GetBlockHash()) {
    LOG_GENERAL(WARNING,
                "Block Hash in Newly received VC Block doesn't match. "
                "Calculated: "
                    << temp_blockHash
                    << " Received: " << vcblock.GetBlockHash().hex());
    return false;
  }

  // Check for duplicated vc block
  VCBlockSharedPtr VCBlockptr;
  if (BlockStorage::GetBlockStorage().GetVCBlock(temp_blockHash, VCBlockptr)) {
    LOG_GENERAL(WARNING,
                "Duplicated vc block detected. 0x" << temp_blockHash.hex());
    return false;
  }

  // Check timestamp
  if (!VerifyTimestamp(vcblock.GetTimestamp(),
                       CONSENSUS_OBJECT_TIMEOUT + VIEWCHANGE_TIME +
                           VIEWCHANGE_PRECHECK_TIME + VIEWCHANGE_EXTRA_TIME)) {
    return false;
  }

  lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);

  // Verify the CommitteeHash member of the BlockHeaderBase
  CommitteeHash committeeHash;
  if (!Messenger::GetDSCommitteeHash(*m_mediator.m_DSCommittee,
                                     committeeHash)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetDSCommitteeHash failed.");
    return false;
  }
  if (committeeHash != vcblock.GetHeader().GetCommitteeHash()) {
    LOG_GENERAL(WARNING,
                "DS committee hash in newly received VC Block doesn't match. "
                "Calculated: "
                    << committeeHash
                    << " Received: " << vcblock.GetHeader().GetCommitteeHash());
    return false;
  }

  // Check the signature of this VC block
  if (!VerifyVCBlockCoSignature(vcblock)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "VCBlock co-sig verification failed");
    return false;
  }

  uint64_t latestInd = m_mediator.m_blocklinkchain.GetLatestIndex() + 1;
  m_mediator.m_blocklinkchain.AddBlockLink(
      latestInd, vcblock.GetHeader().GetViewChangeDSEpochNo(), BlockType::VC,
      vcblock.GetBlockHash());

  bytes dst;
  vcblock.Serialize(dst, 0);

  if (!BlockStorage::GetBlockStorage().PutVCBlock(vcblock.GetBlockHash(),
                                                  dst)) {
    LOG_GENERAL(WARNING, "Failed to store VC Block");
    return false;
  }

  UpdateDSCommiteeCompositionAfterVC(vcblock, *m_mediator.m_DSCommittee);

  if (LOOKUP_NODE_MODE) {
    LOG_STATE("[VCBLK] DS = " << vcblock.GetHeader().GetViewChangeDSEpochNo()
                              << " Tx = "
                              << vcblock.GetHeader().GetViewChangeEpochNo());
    LOG_STATE("[VCBLK] Leader = "
              << vcblock.GetHeader().GetCandidateLeaderNetworkInfo());
    for (const auto& faulty : vcblock.GetHeader().GetFaultyLeaders()) {
      LOG_STATE("[VCBLK] Faulty = " << faulty.second);
    }
  }

  return true;
}

/// This function asssume ddsComm to indicate 0.0.0.0 for current node
/// If you change this function remember to change
/// UpdateRetrieveDSCommiteeCompositionAfterVC()
void Node::UpdateDSCommiteeCompositionAfterVC(const VCBlock& vcblock,
                                              DequeOfNode& dsComm) {
  if (GUARD_MODE) {
    LOG_GENERAL(INFO, "In guard mode. No updating of DS composition requried");
    return;
  }

  for (const auto& faultyLeader : vcblock.GetHeader().GetFaultyLeaders()) {
    DequeOfNode::iterator it;

    // If faulty leader is current node, look for 0.0.0.0 is ds committee
    if (faultyLeader.first == m_mediator.m_selfKey.second &&
        faultyLeader.second == Peer()) {
      PairOfNode selfNode = make_pair(faultyLeader.first, Peer());
      it = find(dsComm.begin(), dsComm.end(), selfNode);
    } else {
      it = find(dsComm.begin(), dsComm.end(), faultyLeader);
    }

    // Remove faulty leader from the current
    if (it != dsComm.end()) {
      dsComm.erase(it);
    } else {
      LOG_GENERAL(WARNING, "FATAL Cannot find the ds leader to eject");
    }

    dsComm.emplace_back(faultyLeader);
  }
}

// Only compares the pubkeys to kickout
void Node::UpdateRetrieveDSCommiteeCompositionAfterVC(const VCBlock& vcblock,
                                                      DequeOfNode& dsComm) {
  if (GUARD_MODE) {
    LOG_GENERAL(INFO, "In guard mode. No updating of DS composition requried");
    return;
  }
  for (const auto& faultyLeader : vcblock.GetHeader().GetFaultyLeaders()) {
    auto it = find_if(dsComm.begin(), dsComm.end(),
                      [faultyLeader](const PairOfNode& p) {
                        return p.first == faultyLeader.first;
                      });

    // Remove faulty leader from the current
    if (it != dsComm.end()) {
      dsComm.erase(it);
    } else {
      LOG_GENERAL(WARNING, "FATAL Cannot find the ds leader to eject");
    }
    dsComm.emplace_back(faultyLeader);
  }
}

void Node::SendVCBlockToOtherShardNodes(const bytes& vcblock_message) {
  LOG_MARKER();
  unsigned int cluster_size = NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD;
  if (cluster_size <= NUM_DS_ELECTION) {
    LOG_GENERAL(
        WARNING,
        "Adjusting NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD to be greater than "
        "NUM_DS_ELECTION. Why not correct the constant.xml next time.");
    cluster_size = NUM_DS_ELECTION + 1;
  }

  LOG_GENERAL(INFO,
              "Primary CLUSTER SIZE used is "
              "(NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD):"
                  << cluster_size);
  SendBlockToOtherShardNodes(vcblock_message, cluster_size,
                             NUM_OF_TREEBASED_CHILD_CLUSTERS);
}
