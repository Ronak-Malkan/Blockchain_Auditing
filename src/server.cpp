// src/server.cpp

#include "server.h"
#include "merkle_tree.h"    
#include "heartbeat_table.h"   
#include "election_state.h"                   // SHA256Hex, ComputeMerkleRoot
#include <google/protobuf/util/json_util.h>       // MessageToJsonString
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <iostream>
#include <chrono>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
using ordered_json = nlohmann::ordered_json;
namespace fs = std::filesystem;
using namespace std::chrono;

using google::protobuf::util::MessageToJsonString;
using google::protobuf::util::JsonStringToMessage;

static constexpr auto kGossipTimeoutMs = 200;

// Utility: Base64-decode into a byte vector
static std::vector<unsigned char> Base64Decode(const std::string& b64) {
  BIO* bmem  = BIO_new_mem_buf(b64.data(), (int)b64.size());
  BIO* b64f  = BIO_new(BIO_f_base64());
  BIO_set_flags(b64f, BIO_FLAGS_BASE64_NO_NL);
  bmem = BIO_push(b64f, bmem);
  std::vector<unsigned char> out(b64.size());
  int len = BIO_read(bmem, out.data(), (int)out.size());
  BIO_free_all(bmem);
  if (len < 0) return {};
  out.resize(len);
  return out;
}

// Utility: verify signature_b64 over data using PEM public key
static bool VerifySignature(
    const std::string& data,
    const std::string& signature_b64,
    const std::string& pubkey_pem)
{
  auto sig = Base64Decode(signature_b64);
  std::cout << "[VerifySignature] decoded signature (len="
            << sig.size() << ")\n";
  BIO* bio = BIO_new_mem_buf(pubkey_pem.data(), (int)pubkey_pem.size());
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  std::cout << "[VerifySignature] decoded public key (len="
            << pubkey_pem << ")\n";
  std::cout << "Signature: " << signature_b64 << "\n";
  std::cout << "Data: " << data << "\n";
  if (!pkey) return false;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  
  EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey);

  EVP_PKEY_CTX *pctx = EVP_MD_CTX_pkey_ctx(ctx);
  if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) <= 0) {
    std::cerr << "Failed to set RSA padding\n";
  }

  EVP_DigestVerifyUpdate(ctx, data.data(), data.size());
  int rc = EVP_DigestVerifyFinal(ctx, sig.data(), sig.size());
  std::cout << "[VerifySignature] EVP_DigestVerifyFinal rc="
            << rc << "\n";
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return rc == 1;
}

// -- FileAuditServiceImpl -------------------------------------------------

FileAuditServiceImpl::FileAuditServiceImpl(
    const std::vector<std::string>& peers,
    std::shared_ptr<MempoolManager> mempool)
  : mempool_(std::move(mempool))
{
  for (auto& addr : peers) {
    std::cout << "[FileAuditServiceImpl] gossip to peer="
              << addr << "\n";
    auto chan = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    gossip_stubs_.push_back(
      blockchain::BlockChainService::NewStub(chan));
  }
}

std::vector<std::unique_ptr<blockchain::BlockChainService::Stub>>&
FileAuditServiceImpl::getGossipStubs() {
  return gossip_stubs_;
}


grpc::Status FileAuditServiceImpl::SubmitAudit(
    grpc::ServerContext* /*ctx*/,
    const common::FileAudit* request,
    fileaudit::FileAuditResponse* response)
{

  std::cout << "[SubmitAudit] client signature="
            << request->signature() << "\n";
  std::cout << "[SubmitAudit] client public key="
            << request->public_key() << "\n";
  std::cout << "[SubmitAudit] verifying client signature\n";

  // 1) Canonical JSON payload (sorted keys)
  ordered_json j;
  j["access_type"] = request->access_type();
  j["file_info"]   = {{"file_id",   request->file_info().file_id()},
                      {"file_name", request->file_info().file_name()}};
  j["req_id"]      = request->req_id();
  j["timestamp"]   = request->timestamp();
  j["user_info"]   = {{"user_id",   request->user_info().user_id()},
                      {"user_name", request->user_info().user_name()}};
  std::string payload = j.dump();
  std::cout << "[SubmitAudit] payload=" << payload << "\n";

  if (!VerifySignature(payload,
                       request->signature(),
                       request->public_key())) {
    return grpc::Status(
      grpc::StatusCode::INVALID_ARGUMENT,
      "Invalid client signature");
  }
  std::cout << "[SubmitAudit] verified client signature\n";

  // 2) Persist to mempool
  mempool_->Append(*request);

  // 3) Gossip to peers
for (auto& stub : gossip_stubs_) {
    // create a new context and set a 200 ms deadline
    grpc::ClientContext ctx2;
    ctx2.set_deadline(
      std::chrono::system_clock::now() +
      std::chrono::milliseconds(kGossipTimeoutMs)
    );

    blockchain::WhisperResponse wr;
    auto st = stub->WhisperAuditRequest(&ctx2, *request, &wr);

    if (!st.ok()) {
      if (st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "[Gossip] to peer timed out after "
                  << kGossipTimeoutMs << "ms\n";
      } else {
        std::cerr << "[Gossip] to peer failed: "
                  << st.error_message() << "\n";
      }
    } else {
      std::cout << "[Gossip] to peer succeeded: "
                << wr.status() << "\n";
    }
  }

  // 4) Reply to client
  response->set_req_id(request->req_id());
  response->set_status("success");
  return grpc::Status::OK;
}

// -- BlockChainServiceImpl ------------------------------------------------


BlockChainServiceImpl::BlockChainServiceImpl(
    std::shared_ptr<MempoolManager> mempool,
    ChainManager& chain,
    std::shared_ptr<HeartbeatTable> hb_table,
    ElectionState& election_state,
    std::string self_addr)
  : mempool_(std::move(mempool))
  , chain_(chain)
  , hb_table_(std::move(hb_table))
  , state_(election_state)
  , self_addr_(std::move(self_addr))
{}

grpc::Status BlockChainServiceImpl::WhisperAuditRequest(
    grpc::ServerContext* /*ctx*/,
    const common::FileAudit* request,
    blockchain::WhisperResponse* response)
{
  std::cout << "[WhisperAuditRequest] audit request received\n";
  std::cout << "  req_id:    " << request->req_id() << "\n";
  std::cout << "  file_info: id=" << request->file_info().file_id()
            << ", name=" << request->file_info().file_name() << "\n";
  std::cout << "  user_info: id=" << request->user_info().user_id()
            << ", name=" << request->user_info().user_name() << "\n";
  std::cout << "  access:    " << request->access_type()
            << ", timestamp=" << request->timestamp() << "\n";
  std::cout << "  signature: (len=" << request->signature().size() << ")\n";
  std::cout << "  public_key: (len=" << request->public_key().size() << ")\n";

  ordered_json j;
  j["access_type"] = request->access_type();
  j["file_info"]   = {{"file_id",   request->file_info().file_id()},
                      {"file_name", request->file_info().file_name()}};
  j["req_id"]      = request->req_id();
  j["timestamp"]   = request->timestamp();
  j["user_info"]   = {{"user_id",   request->user_info().user_id()},
                      {"user_name", request->user_info().user_name()}};
  std::string payload2 = j.dump();
  std::cout << "[SubmitAudit] payload=" << payload2 << "\n";


  if (!VerifySignature(payload2,
                       request->signature(),
                       request->public_key()))
  {
    std::cerr << "[WhisperAuditRequest] invalid signature for req_id="
              << request->req_id() << "\n";
    return grpc::Status(
      grpc::StatusCode::INVALID_ARGUMENT,
      "Invalid signature in gossiped audit");
  } else {
    std::cout << "[WhisperAuditRequest] verified signature for req_id="
              << request->req_id() << "\n";
  }

  // 2) Persist to mempool
  mempool_->Append(*request);

  // 3) Log that it's added
  std::cout << "[WhisperAuditRequest] audit added to mempool\n";

  // 4) Ack
  response->set_status("success");
  return grpc::Status::OK;
}

grpc::Status BlockChainServiceImpl::ProposeBlock(
    grpc::ServerContext* /*ctx*/,
    const blockchain::Block* blk,
    blockchain::BlockVoteResponse* resp)
{
  // 1) Recompute Merkle root from the same JSON-hashes Python uses
  std::vector<std::string> leafs;
  for (auto& a : blk->audits()) {
    ordered_json j;
    j["access_type"] = a.access_type();
    j["file_info"]   = {
      {"file_id",   a.file_info().file_id()},
      {"file_name", a.file_info().file_name()}
    };
    j["req_id"]      = a.req_id();
    j["timestamp"]   = a.timestamp();
    j["user_info"]   = {
      {"user_id",   a.user_info().user_id()},
      {"user_name", a.user_info().user_name()}
    };
    leafs.push_back(SHA256Hex(j.dump()));
  }
  if (ComputeMerkleRoot(leafs) != blk->merkle_root()) {
    resp->set_vote(false);
    resp->set_status("failure");
    resp->set_error_message("bad merkle_root");
    return grpc::Status::OK;
  }

  // 2) prev‐hash
  if (blk->previous_hash() != chain_.getLastHash()) {
    resp->set_vote(false);
    resp->set_status("failure");
    resp->set_error_message("bad previous_hash");
    return grpc::Status::OK;
  }

  // // 3) verify block.hash matches header:
  // {
  //     // recompute same header string
  //   std::string hdr;
  //   hdr += std::to_string(blk->id());
  //   hdr += blk->previous_hash();
  //   hdr += blk->merkle_root();
  //   for (auto& a : blk->audits()) {
  //     hdr += a.SerializeAsString();
  //   }
  //   auto expected = SHA256Hex(hdr);
  //   if (expected != blk->hash()) {
  //     resp->set_vote(false);
  //     resp->set_status("failure");
  //     resp->set_error_message("block_hash mismatch");
  //     return grpc::Status::OK;
  //   }
  // }
  // // 4) verify each audit’s signature…
  // for (auto& a : blk->audits()) {
  //   common::FileAudit copy = a;
  //   copy.clear_signature();
  //   copy.clear_public_key();
  //   std::string payload;
  //   copy.SerializeToString(&payload);
  //   if (!VerifySignature(
  //         payload,
  //         a.signature(),
  //         a.public_key()))
  //   {
  //     resp->set_vote(false);
  //     resp->set_status("failure");
  //     resp->set_error_message("invalid audit signature: " + a.req_id());
  //     return grpc::Status::OK;
  //   }
  // }

  resp->set_vote(true);
  resp->set_status("success");
  return grpc::Status::OK;
}

grpc::Status BlockChainServiceImpl::CommitBlock(
    grpc::ServerContext* /*ctx*/,
    const blockchain::Block* blk,
    blockchain::BlockCommitResponse* resp) 
{
  std::cout << "[CommitBlock] received block id=" << blk->id()
            << ", merkle_root=" << blk->merkle_root() << "\n";
  // // 1) verify merkle root
  // std::vector<std::string> leafs;
  // for (auto& a : blk->audits()) {
  //   std::string json;
  //   google::protobuf::util::MessageToJsonString(a, &json);
  //   leafs.push_back(SHA256Hex(json));
  // }
  // if (ComputeMerkleRoot(leafs) != blk->merkle_root()) {
  //   resp->set_status("failure");
  //   resp->set_error_message("bad merkle_root");
  //   return grpc::Status::OK;
  // }

  // // 2) verify previous hash matches our chain head
  // if (blk->previous_block_hash() != chain_.getLastHash()) {
  //   resp->set_status("failure");
  //   resp->set_error_message("bad previous_block_hash");
  //   return grpc::Status::OK;
  // }

  // // 3) (optional) verify each audit’s signature here…

  // 4) commit into chain.json
  BlockMeta meta {
    blk->id(),
    blk->hash(),
    blk->previous_hash(),
    blk->merkle_root()
  };
  chain_.append(meta);

  // 5) prune mempool
  std::vector<std::string> ids;
  for (auto& a : blk->audits()) ids.push_back(a.req_id());
  mempool_->RemoveBatch(ids);

  // 6) write full block out to its own file
  try {
    fs::create_directories("../blocks");
    std::string block_json;
    google::protobuf::util::MessageToJsonString(*blk, &block_json);

    std::string path = "../blocks/block_" + std::to_string(blk->id()) + ".json";
    std::ofstream out(path);
    if (!out) {
      std::cerr << "[CommitBlock] ERROR writing block file: " << path << "\n";
      resp->set_status("failure");
      resp->set_error_message("could not write block file");
      return grpc::Status::OK;
    }
    out << block_json;
    out.close();
    std::cout << "[CommitBlock] wrote block file " << path << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[CommitBlock] exception writing block file: " << e.what() << "\n";
    resp->set_status("failure");
    resp->set_error_message("exception writing block file");
    return grpc::Status::OK;
  }

  resp->set_status("success");
  return grpc::Status::OK;
}

grpc::Status BlockChainServiceImpl::GetBlock(
    grpc::ServerContext* /*ctx*/,
    const blockchain::GetBlockRequest* req,
    blockchain::GetBlockResponse* resp)
{
  int64_t id = req->id();
  if (id > chain_.getLastID()) {
    resp->set_status("failure");
    resp->set_error_message("block id out of range");
    return grpc::Status::OK;
  }

  // Read the JSON file we wrote earlier
  std::string path = "../blocks/block_" + std::to_string(id) + ".json";
  std::ifstream in(path);
  if (!in) {
    resp->set_status("failure");
    resp->set_error_message("could not open block file");
    return grpc::Status::OK;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  std::string json = buf.str();

  // Parse back into a Block proto
  blockchain::Block blk;
  auto st = JsonStringToMessage(json, &blk);
  if (!st.ok()) {
    resp->set_status("failure");
    resp->set_error_message("JSON parse error: " + st.ToString());
    return grpc::Status::OK;
  }

  // Return it
  *resp->mutable_block() = std::move(blk);
  resp->set_status("success");
  return grpc::Status::OK;
}


grpc::Status BlockChainServiceImpl::SendHeartbeat(
    grpc::ServerContext* /*ctx*/,
    const blockchain::HeartbeatRequest* req,
    blockchain::HeartbeatResponse* resp)
{
  std::cout << "[SendHeartbeat] from=" << req->from_address()
            << " leader=" << req->current_leader_address()
            << " blk=" << req->latest_block_id()
            << " pool=" << req->mem_pool_size() << "\n";

  hb_table_->update(
    req->from_address(),
    req->current_leader_address(),
    req->latest_block_id(),
    req->mem_pool_size()
  );

  if (state_.getLeader().empty() && !req->current_leader_address().empty()) {
    state_.setLeader(req->current_leader_address());
    std::cout << "[SendHeartbeat] learned new leader: "
                << state_.getLeader() << "\n";
  }
  

  resp->set_status("success");
  return grpc::Status::OK;
}


// --- TriggerElection: vote yes/no and record voted_for ---
grpc::Status BlockChainServiceImpl::TriggerElection(
    grpc::ServerContext* /*ctx*/,
    const blockchain::TriggerElectionRequest* req,
    blockchain::TriggerElectionResponse* resp)
{
  const auto& candidate = req->address();

  // Look up candidate’s stats from heartbeat table
  int64_t cand_blocks = 0, cand_pool = 0;
  for (auto& e : hb_table_->all()) {
    if (e.from_address == candidate) {
      cand_blocks = e.latest_block_id;
      cand_pool   = e.mem_pool_size;
      break;
    }
  }

  // Our own stats
  int64_t my_blocks = chain_.getLastID();
  int64_t my_pool   = static_cast<int64_t>(mempool_->LoadAll().size());

  bool vote_yes = false;
  if (cand_blocks > my_blocks ||
      (cand_blocks == my_blocks && cand_pool   > my_pool)   ||
      (cand_blocks == my_blocks && cand_pool   == my_pool &&
       candidate > self_addr_))
  {
    vote_yes = true;
    state_.setVotedFor(candidate);
  }

  resp->set_vote(vote_yes);
  resp->set_term(0);              // unused
  resp->set_status("success");
  return grpc::Status::OK;
}

// --- NotifyLeadership: record new leader in state ---
grpc::Status BlockChainServiceImpl::NotifyLeadership(
    grpc::ServerContext* /*ctx*/,
    const blockchain::NotifyLeadershipRequest* req,
    blockchain::NotifyLeadershipResponse* resp)
{
  state_.setLeader(req->address());
  std::cout << "[NotifyLeadership] new leader = " << req->address() << "\n";
  resp->set_status("success");
  return grpc::Status::OK;
}



// grpc::Status BlockChainServiceImpl::VoteOnBlock(
//     grpc::ServerContext* /*ctx*/,
//     const blockchain::Vote* request,
//     blockchain::BlockVoteResponse* response)
// {
//   std::cout << "[VoteOnBlock] block_id="
//             << request->block_id() << std::endl;
//   response->set_success(true);
//   return grpc::Status::OK;
// }
