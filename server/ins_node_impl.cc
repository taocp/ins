#include "ins_node_impl.h"

#include <sys/utsname.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <gflags/gflags.h>
#include "common/this_thread.h"
#include "storage/meta.h"
#include "storage/binlog.h"

DECLARE_string(ins_data_dir);
DECLARE_int32(max_cluster_size);

namespace galaxy {
namespace ins {

void GetHostName(std::string* hostname) {
  struct utsname buf;
  if (0 != uname(&buf)) {
    *buf.nodename = '\0';
  }
  *hostname = buf.nodename;
}

InsNodeImpl::InsNodeImpl (std::string& server_id,
                          const std::vector<std::string>& members
                          ) : stop_(false),
                              self_id_(server_id),
                              current_term_(0),
                              status_(kFollower),
                              heartbeat_count_(0),
                              meta_(NULL),
                              binlogger_(NULL),
                              replicatter_(FLAGS_max_cluster_size),
                              commit_index_(-1),
                              last_applied_index_(-1) {
    srand(time(NULL));
    replication_cond_ = new CondVar(&mu_);
    commit_cond_ = new CondVar(&mu_);
    std::vector<std::string>::const_iterator it = members.begin();
    bool self_in_cluster = false;
    for(; it != members.end(); it++) {
        members_.push_back(*it);
        if (self_id_ == *it) {
            LOG(INFO, "cluster member[Self]: %s", it->c_str());
            self_in_cluster = true;
        } else {
            LOG(INFO, "cluster member: %s", it->c_str());
        }
    }
    if (!self_in_cluster) {
        LOG(FATAL, "this node is not in cluster membership,"
                   " please check your configuration. self: %s", self_id_.c_str());
        exit(-1);
    }
    if (members_.size() > static_cast<size_t>(FLAGS_max_cluster_size)) {
        LOG(FATAL, "cluster size is larger than configuration: %d > %d",
            members_.size(), FLAGS_max_cluster_size);
    }
    std::string sub_dir = self_id_;
    boost::replace_all(sub_dir, ":", "_");
    meta_ = new Meta(FLAGS_ins_data_dir + "/" + sub_dir);
    binlogger_ = new BinLogger(FLAGS_ins_data_dir + "/" + sub_dir);
    current_term_ = meta_->ReadCurrentTerm();
    meta_->ReadVotedFor(voted_for_);
    committer_.AddTask(boost::bind(&InsNodeImpl::CommitIndexObserv, this));
    MutexLock lock(&mu_);
    CheckLeaderCrash();
}

InsNodeImpl::~InsNodeImpl() {
    MutexLock lock(&mu_);
    stop_ = true;
    delete meta_;
    delete binlogger_;
}

int32_t InsNodeImpl::GetRandomTimeout() {
    int32_t timeout = 150 + (int32_t) (300.0 * rand()/(RAND_MAX+1.0));
    return timeout;
}

void InsNodeImpl::CheckLeaderCrash() {
    mu_.AssertHeld();
    int32_t timeout = GetRandomTimeout();
    elect_leader_task_  = pool_.DelayTask(timeout, 
                                        boost::bind(&InsNodeImpl::TryToBeLeader,
                                                    this)
                                        );
    //LOG(INFO, "InsNodeImpl::CheckLeaderCrash, elect_leader_task_:%d", elect_leader_task_);
}

void InsNodeImpl::ShowStatus(::google::protobuf::RpcController* /*controller*/,
                             const ::galaxy::ins::ShowStatusRequest* /*request*/,
                             ::galaxy::ins::ShowStatusResponse* response,
                             ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    int64_t last_log_index;
    int64_t last_log_term;
    GetLastLogIndexAndTerm(&last_log_index, &last_log_term);
    response->set_status(status_);
    response->set_term(current_term_);    
    response->set_last_log_index(last_log_index);
    response->set_last_log_term(last_log_term);
    done->Run();
}

void InsNodeImpl::TransToFollower(const char* msg, int64_t new_term) {
    mu_.AssertHeld();
    LOG(INFO, "%s, my term is outdated(%ld < %ld), trans to follower",
        msg, 
        current_term_, 
        new_term);
    status_ = kFollower;
    current_term_ = new_term;
    meta_->WriteCurrentTerm(current_term_);
}

void InsNodeImpl::CommitIndexObserv() {
    MutexLock lock(&mu_);
    while (!stop_) {
        while (commit_index_ <=  last_applied_index_) {
            LOG(INFO, "commit_idx: %ld, last_applied_index: %ld",
                commit_index_, last_applied_index_);
            commit_cond_->Wait();
        }
        int64_t from_idx = last_applied_index_;
        int64_t to_idx = commit_index_;
        mu_.Unlock();
        for (int64_t i = from_idx + 1; i <= to_idx; i++) {
            LogEntry log_entry;
            binlogger_->ReadSlot(i, &log_entry);
            mu_.Lock();
            switch(log_entry.op) {
                case kPut:
                    LOG(INFO, "add to data_map_, key: %s, value: %s",
                        log_entry.key.c_str(), log_entry.value.c_str());
                    data_map_[log_entry.key] = log_entry.value;
                    break;
                case kDel:
                    LOG(INFO, "delete from data_map_, key: %s",
                        log_entry.key.c_str());
                    data_map_.erase(log_entry.key);
                    break;
                case kNop:
                    LOG(INFO, "kNop got, do nothing, key: %s", 
                              log_entry.key.c_str());
                    break;
            }
            if (client_ack_.find(i) != client_ack_.end()) {
                ClientAck& ack = client_ack_[i];
                if (ack.response) {
                    ack.response->set_success(true);
                    ack.response->set_leader_id("");
                    ack.done->Run(); //client put ok;
                }
                if (ack.del_response) {
                    ack.del_response->set_success(true);
                    ack.del_response->set_leader_id("");
                    ack.done->Run(); //client del ok;   
                }
                client_ack_.erase(i);
            }
            last_applied_index_ += 1;
            mu_.Unlock();
        }
    }
}

void InsNodeImpl::HearBeatCallback(const ::galaxy::ins::AppendEntriesRequest* request,
                                  ::galaxy::ins::AppendEntriesResponse* response,
                                  bool failed, int /*error*/) {
    MutexLock lock(&mu_);
    boost::scoped_ptr<const galaxy::ins::AppendEntriesRequest> request_ptr(request);
    boost::scoped_ptr<galaxy::ins::AppendEntriesResponse> response_ptr(response);
    if (status_ != kLeader) {
        LOG(INFO, "outdated HearBeatCallback, I am no longer leader now.");
        return ;
    }
    if (!failed) {
        if (response_ptr->current_term() > current_term_) {
            TransToFollower("InsNodeImpl::HearBeatCallback", 
                            response_ptr->current_term());
        }
        else {
            //LOG(INFO, "I am the leader at term: %ld", current_term_);
        }
    }  
}

void InsNodeImpl::BroadCastHeartBeat() {
    MutexLock lock(&mu_);
    if (status_ != kLeader) {
        return;
    }
    //LOG(INFO,"broadcast heartbeat to clusters");
    std::vector<std::string>::iterator it = members_.begin();
    for(; it!= members_.end(); it++) {
        if (*it == self_id_) {
            continue;
        }
        InsNode_Stub* stub;
        rpc_client_.GetStub(*it, &stub);
        ::galaxy::ins::AppendEntriesRequest* request = 
                    new ::galaxy::ins::AppendEntriesRequest();
        ::galaxy::ins::AppendEntriesResponse* response =
                    new ::galaxy::ins::AppendEntriesResponse();
        request->set_term(current_term_);
        request->set_leader_id(self_id_);
        request->set_leader_commit_index(commit_index_);
        boost::function<void (const ::galaxy::ins::AppendEntriesRequest*,
                        ::galaxy::ins::AppendEntriesResponse*,
                        bool, int) > callback;
        callback = boost::bind(&InsNodeImpl::HearBeatCallback, this,
                               _1, _2, _3, _4);
        rpc_client_.AsyncRequest(stub, &InsNode_Stub::AppendEntries, 
                                 request, response, callback, 2);
    }
    heart_beat_pool_.DelayTask(50, boost::bind(&InsNodeImpl::BroadCastHeartBeat, this));
}

void InsNodeImpl::StartReplicateLog() {
    mu_.AssertHeld();
    std::vector<std::string>::iterator it = members_.begin();
    for(; it!= members_.end(); it++) {
        if (*it == self_id_) {
            continue;
        }
        std::string follower_id = *it;
        next_index_[follower_id] = binlogger_->GetLength();
        match_index_[follower_id] = -1;
        replicatter_.AddTask(boost::bind(&InsNodeImpl::ReplicateLog,
                                         this, *it));
    }
    LogEntry log_entry;
    log_entry.key = "Ping";
    log_entry.value = "";
    log_entry.term = current_term_;
    log_entry.op = kNop;
    binlogger_->AppendEntry(log_entry);
}

void InsNodeImpl::VoteCallback(const ::galaxy::ins::VoteRequest* request,
                               ::galaxy::ins::VoteResponse* response,
                               bool failed, int /*error*/) {
    MutexLock lock(&mu_);
    boost::scoped_ptr<const galaxy::ins::VoteRequest> request_ptr(request);
    boost::scoped_ptr<galaxy::ins::VoteResponse> response_ptr(response);
    if (!failed && status_ == kCandidate) {
        int64_t their_term = response_ptr->term();
        LOG(INFO, "InsNodeImpl::VoteCallback[%ld], result:%s",
            their_term, response_ptr->vote_granted()?"true":"false");
        if (response_ptr->vote_granted() && their_term == current_term_) {
            vote_grant_[current_term_]++;
            if (vote_grant_[current_term_] > (members_.size() / 2)) {
                status_ = kLeader;
                current_leader_ = self_id_;
                LOG(INFO, "I win the election, term:%d", current_term_);
                heart_beat_pool_.AddTask(
                    boost::bind(&InsNodeImpl::BroadCastHeartBeat, this));
                StartReplicateLog();
            }
        } else {
            if (their_term > current_term_) {
                TransToFollower("InsNodeImpl::VoteCallback", their_term);
            }
        }
    }
}

void InsNodeImpl::GetLastLogIndexAndTerm(int64_t* last_log_index,
                                         int64_t* last_log_term) {
    mu_.AssertHeld();
    *last_log_index = binlogger_->GetLength() - 1;
    *last_log_term = -1;
    if (*last_log_index >= 0) {
        LogEntry log_entry;
        binlogger_->ReadSlot(*last_log_index, &log_entry);
        *last_log_term = log_entry.term;
    }
}

void InsNodeImpl::TryToBeLeader() {
    MutexLock lock(&mu_);
    if (members_.size() == 1) { //single node mode
        status_ = kLeader;
        current_leader_ =  self_id_;
        return;
    }
    if (status_ == kLeader) {
        CheckLeaderCrash();
        return;
    }
    if (status_ == kFollower && heartbeat_count_ > 0) {
        heartbeat_count_ = 0;
        CheckLeaderCrash();
        return;
    }
    current_term_++;
    meta_->WriteCurrentTerm(current_term_);
    status_ =  kCandidate;
    voted_for_[current_term_] = self_id_;
    meta_->WriteVotedFor(current_term_, self_id_);
    vote_grant_[current_term_] ++;
    std::vector<std::string>::iterator it = members_.begin();
    int64_t last_log_index;
    int64_t last_log_term;
    GetLastLogIndexAndTerm(&last_log_index, &last_log_term);
    LOG(INFO, "broad cast vote request to cluster, new term: %ld", current_term_);
    for(; it!= members_.end(); it++) {
        if (*it == self_id_) {
            continue;
        }
        InsNode_Stub* stub;
        rpc_client_.GetStub(*it, &stub);
        ::galaxy::ins::VoteRequest* request = new ::galaxy::ins::VoteRequest();
        ::galaxy::ins::VoteResponse* response = new ::galaxy::ins::VoteResponse();
        request->set_candidate_id(self_id_);
        request->set_term(current_term_);
        request->set_last_log_index(last_log_index);
        request->set_last_log_term(last_log_term);
        boost::function<void (const ::galaxy::ins::VoteRequest* ,
                              ::galaxy::ins::VoteResponse* ,
                              bool, int ) > callback;
        callback = boost::bind(&InsNodeImpl::VoteCallback, this, _1, _2, _3, _4);
        rpc_client_.AsyncRequest(stub, &InsNode_Stub::Vote, request, response,
                                 callback, 2);
    }
    CheckLeaderCrash();
}

void InsNodeImpl::AppendEntries(::google::protobuf::RpcController* /*controller*/,
                                const ::galaxy::ins::AppendEntriesRequest* request,
                                ::galaxy::ins::AppendEntriesResponse* response,
                                ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    if (request->term() >= current_term_) {
        status_ = kFollower;
        if (request->term() > current_term_) {
            meta_->WriteCurrentTerm(request->term());
        }
        current_term_ = request->term();
    } else {
        response->set_current_term(current_term_);
        response->set_success(false);
        LOG(INFO, "[AppendEntries] term is outdated");
        done->Run();
        return;
    }

    if (status_ == kFollower) {
        current_leader_ = request->leader_id();
        heartbeat_count_++;
        if (request->entries_size() > 0) {
            if (request->prev_log_index() >= binlogger_->GetLength()){
                response->set_current_term(current_term_);
                response->set_success(false);
                LOG(INFO, "[AppendEntries] prev log is beyond");
                done->Run();
                return;
            }
            if (request->prev_log_index() >= 0) {
                LogEntry prev_log_entry;
                binlogger_->ReadSlot(request->prev_log_index(),
                                     &prev_log_entry);
                int64_t prev_log_term = prev_log_entry.term;
                if (prev_log_term != request->prev_log_term()) {
                    binlogger_->Truncate(request->prev_log_index() - 1);
                    response->set_current_term(current_term_);
                    response->set_success(false);
                    LOG(INFO, "[AppendEntries] term not match");
                    done->Run();
                    return;
                }
            }
        }
        for (int i=0; i < request->entries_size(); i++) {
            LogEntry log_entry;
            log_entry.op = request->entries(i).op();
            log_entry.key = request->entries(i).key();
            log_entry.value = request->entries(i).value();
            log_entry.term = request->entries(i).term();
            binlogger_->AppendEntry(log_entry);
        }
        int64_t old_commit_index = commit_index_;
        commit_index_ = std::min(binlogger_->GetLength() - 1,
                                 request->leader_commit_index());
       
        if (commit_index_ > old_commit_index) {
            commit_cond_->Signal();
            LOG(INFO, "follower: update my commit index to :%ld", commit_index_);
        }
        response->set_current_term(current_term_);
        response->set_success(true);
        done->Run();
    } else {
        LOG(FATAL, "invalid status: %d", status_);
        abort();
    }
    return;
}

void InsNodeImpl::Vote(::google::protobuf::RpcController* /*controller*/,
                       const ::galaxy::ins::VoteRequest* request,
                       ::galaxy::ins::VoteResponse* response,
                       ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    if (request->term() < current_term_) {
        response->set_vote_granted(false);
        response->set_term(current_term_);
        done->Run();
        return;
    }
    int64_t last_log_index;
    int64_t last_log_term;
    GetLastLogIndexAndTerm(&last_log_index, &last_log_term);
    if (request->last_log_index() < last_log_index) {
        response->set_vote_granted(false);
        response->set_term(current_term_);
        done->Run();
        return;
    } else if(request->last_log_index() == last_log_index) {
        if (request->last_log_term() < last_log_term) {
            response->set_vote_granted(false);
            response->set_term(current_term_);
            done->Run();
            return;
        }
    }

    if (request->term() > current_term_) {
        TransToFollower("InsNodeImpl::Vote", request->term());
    }
    if (voted_for_.find(current_term_) != voted_for_.end() &&
        voted_for_[current_term_] != request->candidate_id()) {
        response->set_vote_granted(false);
        response->set_term(current_term_);
        done->Run();
        return;
    }
    voted_for_[current_term_] = request->candidate_id();
    meta_->WriteVotedFor(current_term_, request->candidate_id());
    response->set_vote_granted(true);
    response->set_term(current_term_);
    done->Run();
    return;
}


void InsNodeImpl::UpdateCommitIndex(int64_t a_index) {
    mu_.AssertHeld();
    std::vector<std::string>::const_iterator it;
    uint32_t match_count = 0;
    for (it = members_.begin(); it != members_.end(); it++) {
        std::string server_id = *it;
        if (match_index_[server_id] >= a_index) {
            match_count += 1;
        }
    }
    if (match_count > members_.size()/2 && a_index > commit_index_) {
        commit_index_ = a_index;
        LOG(INFO, "update to new commit index: %ld", commit_index_);
        commit_cond_->Signal();
    }
}

void InsNodeImpl::ReplicateLog(std::string follower_id) {
    MutexLock lock(&mu_);
    while (!stop_ && status_ == kLeader) {
        while (binlogger_->GetLength() <= next_index_[follower_id]) {
            LOG(INFO, "no new log entry for %s", follower_id.c_str());
            replication_cond_->TimeWait(2000);
            if (status_ != kLeader) {
                break;
            }
        }
        if (status_ != kLeader) {
            break;
        }
        int64_t index = next_index_[follower_id];
        int64_t cur_term = current_term_;
        int64_t prev_index = index - 1;
        int64_t prev_term = -1;
        std::string leader_id = self_id_;
        LogEntry log_entry, prev_log_entry;
        binlogger_->ReadSlot(index, &log_entry);
        if (prev_index > -1) {
            binlogger_->ReadSlot(prev_index, &prev_log_entry);
            prev_term = prev_log_entry.term;
        } 
        mu_.Unlock();

        InsNode_Stub* stub;
        rpc_client_.GetStub(follower_id, &stub);
        galaxy::ins::AppendEntriesRequest request;
        galaxy::ins::AppendEntriesResponse response;
        request.set_term(cur_term);
        request.set_leader_id(leader_id);
        request.set_prev_log_index(prev_index);
        request.set_prev_log_term(prev_term);
        request.set_leader_commit_index(commit_index_);
        galaxy::ins::Entry * entry = request.add_entries();
        entry->set_term(log_entry.term);
        entry->set_key(log_entry.key);
        entry->set_value(log_entry.value);
        entry->set_op(log_entry.op);
        bool ok = rpc_client_.SendRequest(stub, 
                                          &InsNode_Stub::AppendEntries,
                                          &request,
                                          &response,
                                          2, 1);
        mu_.Lock();
        mu_.AssertHeld();
        if (ok && response.current_term() > current_term_) {
            TransToFollower("InsNodeImpl::ReplicateLog", 
                            response.current_term());
        }
        if (status_ != kLeader) {
            break;
        }
        if (ok) {
            if (response.success()) { // log replicated
                next_index_[follower_id] = index + 1;
                match_index_[follower_id] = index;
                UpdateCommitIndex(index);
            } else { // (index, term ) miss match
                next_index_[follower_id] -= 1;
                LOG(INFO, "adjust next_index of %s to %ld",
                    follower_id.c_str(), 
                    next_index_[follower_id]);
                if (next_index_[follower_id] < 0 ){
                    next_index_[follower_id] = 0;
                }
            }
        } else { //rpc error;
            mu_.Unlock();
            LOG(FATAL, "faild to send replicate-rpc to %s ", 
                follower_id.c_str());
            ThisThread::Sleep(20);
        }
    }
}

void InsNodeImpl::Get(::google::protobuf::RpcController* /*controller*/,
                      const ::galaxy::ins::GetRequest* request,
                      ::galaxy::ins::GetResponse* response,
                      ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    if (status_ == kFollower) {
        response->set_hit(false);
        response->set_leader_id(current_leader_);
        response->set_success(false);
        done->Run();
        return;
    }

    if (status_ == kCandidate) {
        response->set_hit(false);
        response->set_leader_id("");
        response->set_success(false);
        done->Run();
        return;
    }
    std::string key = request->key();
    LOG(INFO, "client get key: %s", key.c_str());
    if (data_map_.find(key) != data_map_.end()) {
        response->set_hit(true);
        response->set_success(true);
        response->set_value(data_map_[key]);
        response->set_leader_id("");
    } else {
        response->set_hit(false);
        response->set_success(true);
        response->set_leader_id("");
    }
    done->Run();
    return;
}

void InsNodeImpl::Delete(::google::protobuf::RpcController* /*controller*/,
                        const ::galaxy::ins::DelRequest* request,
                        ::galaxy::ins::DelResponse* response,
                        ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    if (status_ == kFollower) {
        response->set_success(false);
        response->set_leader_id(current_leader_);
        done->Run();
        return;
    }

    if (status_ == kCandidate) {
        response->set_success(false);
        response->set_leader_id("");
        done->Run();
        return;
    }

    const std::string& key = request->key();
    LOG(INFO, "client want delete key :%s", key.c_str());
    LogEntry log_entry;
    log_entry.key = key;
    log_entry.value = "";
    log_entry.term = current_term_;
    log_entry.op = kDel;
    binlogger_->AppendEntry(log_entry);
    int64_t cur_index = binlogger_->GetLength() - 1;
    ClientAck& ack = client_ack_[cur_index];
    ack.done = done;
    ack.del_response = response;
    replication_cond_->Broadcast();
    return;
}

void InsNodeImpl::Put(::google::protobuf::RpcController* /*controller*/,
                      const ::galaxy::ins::PutRequest* request,
                      ::galaxy::ins::PutResponse* response,
                      ::google::protobuf::Closure* done) {
    MutexLock lock(&mu_);
    if (status_ == kFollower) {
        response->set_success(false);
        response->set_leader_id(current_leader_);
        done->Run();
        return;
    }

    if (status_ == kCandidate) {
        response->set_success(false);
        response->set_leader_id("");
        done->Run();
        return;
    }
    const std::string& key = request->key();
    const std::string& value = request->value();
    LOG(INFO, "client want put key :%s", key.c_str());
    LogEntry log_entry;
    log_entry.key = key;
    log_entry.value = value;
    log_entry.term = current_term_;
    log_entry.op = kPut;
    binlogger_->AppendEntry(log_entry);
    int64_t cur_index = binlogger_->GetLength() - 1;
    ClientAck& ack = client_ack_[cur_index];
    ack.done = done;
    ack.response = response;
    replication_cond_->Broadcast();
    return;
}


} //namespace ins
} //namespace galaxy



