#include <cassert>
#include "raft.h"
#include "raft_impl.h"
#include "raft.pb.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <netdb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>

using namespace std;

// framing: 4 byte length (network byte order) followed by protobuf
struct Peer {
  string host;
  int port;
  int fd = -1;
  string buffer; // Read buffer
};

struct Connection {
  int fd;
  string buffer;
};

class KVServer {
public:
  using Message = raft::RaftMessagePb;
  using LogEntry = raft::RaftLogEntryPb;
  using Config = raft::RaftConfigPb;
  using RaftClass = raft::Raft<KVServer>;

  KVServer(const string &id, int port, const vector<Peer> &peers)
      : id_(id), port_(port), peers_(peers) {
    // Open WAL
    wal_fd_ = open(("wal_" + id_ + ".log").c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal_fd_ < 0) {
      perror("open wal");
      exit(1);
    }

    LoadWAL();
    SetupListener();
    SetupClientListener();
  }

  ~KVServer() {
    close(wal_fd_);
    close(listen_fd_);
    close(client_listen_fd_);
    for(auto& p : peers_) if(p.fd != -1) close(p.fd);
    for(auto& c : incoming_) close(c.fd);
    for(auto& c : clients_) close(c.fd);
  }

  void set_raft(RaftClass* raft) { raft_ = raft; }

  // ---- RaftServerInterface ----

  bool SendMessage(RaftClass *raft, const string &node, const Message &message) {
    (void)raft;
    // cout << "SendMessage to " << node << " type=" << message.ShortDebugString() << endl;
    for (auto &p : peers_) {
      // Simplification: identifying peer by port in node id
      if (to_string(p.port) == node || p.host + ":" + to_string(p.port) == node) {
         if (p.fd == -1) {
           if (!ConnectToPeer(p)) {
             cerr << "Failed to connect to " << node << endl;
             return false;
           }
         }

         string data;
         if (!message.SerializeToString(&data)) return false;
         if (!SendTypedMessage(p.fd, MSG_RAFT, data)) {
             close(p.fd);
             p.fd = -1;
             return false;
         }
         return true;
      }
    }
    return false;
  }

  void GetLogEntry(RaftClass *raft, int64_t term, int64_t index, int64_t end, LogEntry *entry) {
    (void)raft; (void)term; (void)end;
    if (index > 0 && index <= (int64_t)log_.size()) {
       *entry = log_[index - 1];
    }
  }

  void WriteLogEntry(RaftClass *raft, const LogEntry &entry) {
      (void)raft;

      // Update in-memory log
      if (entry.has_index()) {
          int64_t idx = entry.index(); // 1-based
          if (idx > 0) {
              if (idx <= (int64_t)log_.size()) {
                  log_.resize(idx - 1);
              }
              if (idx == (int64_t)log_.size() + 1) {
                  log_.push_back(entry);
              } else {
                  // Gap? Should not happen in Raft leader operations usually.
                  // But if it does, expanding vector with default entries is dangerous.
                  // Assume sequential.
                  if (idx > (int64_t)log_.size() + 1) {
                       cerr << "WriteLogEntry gap! size=" << log_.size() << " requested=" << idx << endl;
                       // Ideally fatal or panic
                  }
              }
          }
      }

      string data;
      if (!entry.SerializeToString(&data)) return;
      uint32_t len = htonl(data.size());

      struct iovec iov[2];
      iov[0].iov_base = &len;
      iov[0].iov_len = sizeof(len);
      iov[1].iov_base = (void*)data.data();
      iov[1].iov_len = data.size();

      ssize_t res = writev(wal_fd_, iov, 2);
      if (res < 0) {
          perror("writev");
          // Handle error (e.g., panic or retry)
      } else {
          fdatasync(wal_fd_); // Ensure durability
      }
  }

  void CommitLogEntry(RaftClass *raft, const LogEntry &entry) {
    (void)raft;
    if (entry.has_data()) {
      string s = entry.data();
      size_t eq = s.find('=');
      if (eq != string::npos) {
        kv_[s.substr(0, eq)] = s.substr(eq + 1);
        cout << "Committed: " << s.substr(0, eq) << " = " << s.substr(eq + 1) << endl;
      }
    }
  }

  void LeaderChange(RaftClass *raft, const string &leader) {
      (void)raft;
      leader_ = leader;
      cout << "Leader is now: " << leader << endl;
  }

  void ConfigChange(RaftClass *raft, const Config &config) {
      (void)raft;
      cout << "Config changed: " << config.ShortDebugString() << endl;
  }

  void Run() {
      double now = 0.0;
      while (true) {
          // Poll
          vector<struct pollfd> fds;
          fds.push_back({listen_fd_, POLLIN, 0});
          fds.push_back({client_listen_fd_, POLLIN, 0});

          for (auto &p : peers_) {
            if (p.fd != -1) fds.push_back({p.fd, POLLIN, 0});
          }
          for (auto &c : incoming_) {
            fds.push_back({c.fd, POLLIN, 0});
          }
          for (auto &c : clients_) {
            fds.push_back({c.fd, POLLIN, 0});
          }

          int ret = poll(fds.data(), fds.size(), 10); // 10ms timeout
          if (ret > 0) {
              // Check Peer Listener
              if (fds[0].revents & POLLIN) {
                  int client = accept(listen_fd_, nullptr, nullptr);
                  if (client >= 0) {
                      incoming_.push_back({client, ""});
                      cout << "Accepted new peer connection" << endl;
                  }
              }
              // Check Client Listener
              if (fds[1].revents & POLLIN) {
                  int client = accept(client_listen_fd_, nullptr, nullptr);
                  if (client >= 0) {
                      clients_.push_back({client, ""});
                      cout << "Accepted new client connection" << endl;
                  }
              }

              // Check Peers and Incoming
              int idx = 2; // after listeners
              for (auto &p : peers_) {
                  if (p.fd != -1) {
                      if (fds[idx].revents & (POLLIN|POLLERR|POLLHUP)) {
                          if (!ReadFromFd(p.fd, p.buffer, now)) {
                              cout << "Peer " << p.port << " disconnected" << endl;
                              close(p.fd);
                              p.fd = -1;
                              p.buffer.clear();
                          }
                      }
                      idx++;
                  }
              }

              for (auto it = incoming_.begin(); it != incoming_.end();) {
                  bool remove = false;
                  if (fds[idx].revents & (POLLIN|POLLERR|POLLHUP)) {
                       if (!ReadFromFd(it->fd, it->buffer, now)) {
                           remove = true;
                       }
                  }
                  idx++;

                  if (remove) {
                      close(it->fd);
                      it = incoming_.erase(it);
                  } else {
                      ++it;
                  }
              }

              for (auto it = clients_.begin(); it != clients_.end();) {
                  bool remove = false;
                  if (fds[idx].revents & (POLLIN|POLLERR|POLLHUP)) {
                       if (!ReadFromClientFd(it->fd, it->buffer)) {
                           remove = true;
                       }
                  }
                  idx++;

                  if (remove) {
                      close(it->fd);
                      it = clients_.erase(it);
                  } else {
                      ++it;
                  }
              }
          }

          now += 0.01; // approx
          raft_->Tick(now);
      }
  }

private:
  static const uint8_t MSG_RAFT = 0;
  static const uint8_t MSG_FWD_REQ = 1;
  static const uint8_t MSG_FWD_RESP = 2;

  // Helper to send typed message
  bool SendTypedMessage(int fd, uint8_t type, const string& payload) {
      if (fd == -1) return false;
      uint32_t len = htonl(payload.size() + 1); // +1 for type
      string frame((char*)&len, 4);
      frame += (char)type;
      frame += payload;
      ssize_t sent = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
      if (sent != (ssize_t)frame.size()) {
          // perror("send");
          return false;
      }
      return true;
  }

  bool ConnectToPeer(Peer &p) {
      int s = socket(AF_INET, SOCK_STREAM, 0);
      if (s < 0) return false;

      struct sockaddr_in start;
      memset(&start, 0, sizeof(start));
      start.sin_family = AF_INET;
      start.sin_port = htons(p.port);

      // Resolve host (assuming localhost or IP for now)
      if (inet_pton(AF_INET, p.host.c_str(), &start.sin_addr) <= 0) {
         // Naive gethostbyname fallback or just fail
         struct hostent *he = gethostbyname(p.host.c_str());
         if (!he) { close(s); return false; }
         memcpy(&start.sin_addr, he->h_addr_list[0], he->h_length);
      }

      if (connect(s, (struct sockaddr *)&start, sizeof(start)) < 0) {
          close(s);
          return false;
      }
      p.fd = s;
      cout << "Connected to peer " << p.host << ":" << p.port << endl;
      return true;
  }

  void SetupListener() {
      listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
      int opt = 1;
      setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(port_);

      if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
          perror("bind peer listener");
          exit(1);
      }
      if (listen(listen_fd_, 10) < 0) {
          perror("listen");
          exit(1);
      }
  }

  void SetupClientListener() {
      client_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
      int opt = 1;
      setsockopt(client_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(port_ + 1000);

      if (bind(client_listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
          perror("bind client listener");
          exit(1);
      }
      if (listen(client_listen_fd_, 10) < 0) {
          perror("listen");
          exit(1);
      }
      cout << "Listening for clients on port " << port_ + 1000 << endl;
  }

  void LoadWAL() {

      lseek(wal_fd_, 0, SEEK_SET);

      int64_t data_committed = 0;

      while (true) {
          uint32_t len;
          ssize_t n = read(wal_fd_, &len, sizeof(len));
          if (n == 0) break; // EOF
          if (n != sizeof(len)) {
              cerr << "WAL corruption (length)" << endl;
              break;
          }
          len = ntohl(len);

          string data;
          data.resize(len);
          n = read(wal_fd_, &data[0], len);
          if (n != (ssize_t)len) {
              cerr << "WAL corruption (payload)" << endl;
              break;
          }

          LogEntry entry;
          if (entry.ParseFromString(data)) {
              // Track committed index from internal entries
              if (entry.has_data_committed()) {
                  if (entry.data_committed() > data_committed) {
                      data_committed = entry.data_committed();
                  }
              }

              if (entry.has_index()) {
                  int64_t idx = entry.index();
                  if (idx > 0) {
                      if (idx <= (int64_t)log_.size()) {
                          log_.resize(idx - 1);
                      }
                      log_.push_back(entry);
                  }
              }
          }
      }

      // Replay committed entries
      for (const auto& entry : log_) {
          if (entry.has_index() && entry.index() <= data_committed) {
              if (entry.has_data()) {
                  string s = entry.data();
                  size_t eq = s.find('=');
                  if (eq != string::npos) {
                      kv_[s.substr(0, eq)] = s.substr(eq + 1);
                  }
              }
          }
      }

      // Restore position (though O_APPEND overrides for writes)
      lseek(wal_fd_, 0, SEEK_END);
      cout << "Recovered " << log_.size() << " entries from WAL. Committed up to " << data_committed << "." << endl;
  }

  bool ReadFromFd(int fd, string &buffer, double now) {
      char buf[1024];
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) return false;
      buffer.append(buf, n);

      while (buffer.size() >= 4) {
          uint32_t len;
          memcpy(&len, buffer.data(), 4);
          len = ntohl(len);

          if (buffer.size() < 4 + len) break;

          string payload = buffer.substr(4, len); // Includes type byte
          buffer = buffer.substr(4 + len);

          if (payload.size() < 1) continue; // Should not happen
          uint8_t type = (uint8_t)payload[0];
          string data = payload.substr(1);

          if (type == MSG_RAFT) {
              raft::RaftMessagePb message;
              if (message.ParseFromString(data)) {
                  raft_->Run(now, message);
              }
          } else if (type == MSG_FWD_REQ) {
              HandleForwardReq(fd, data);
          } else if (type == MSG_FWD_RESP) {
              HandleForwardResp(data);
          }
      }
      return true;
  }

  void HandleForwardReq(int peer_fd, const string& data) {
      if (data.size() < 8) return;
      uint64_t req_id;
      memcpy(&req_id, data.data(), 8);
      string cmd = data.substr(8);

      string response;
      if (leader_ == id_) {
           if (cmd.rfind("set ", 0) == 0) {
               size_t space = cmd.find(' ', 4);
               if (space != string::npos) {
                   string key = cmd.substr(4, space - 4);
                   string val = cmd.substr(space + 1);
                   LogEntry entry;
                   entry.set_data(key + "=" + val);
                   raft_->Propose(entry);
                   response = "OK\n";
               } else {
                   response = "ERROR\n";
               }
           } else {
               response = "UNKNOWN\n";
           }
      } else {
          response = "NOT_LEADER " + leader_ + "\n";
      }

      string payload;
      payload.resize(8);
      memcpy(&payload[0], &req_id, 8);
      payload += response;
      SendTypedMessage(peer_fd, MSG_FWD_RESP, payload);
  }

  void HandleForwardResp(const string& data) {
      if (data.size() < 8) return;
      uint64_t req_id;
      memcpy(&req_id, data.data(), 8);
      string resp = data.substr(8);

      auto it = pending_requests_.find(req_id);
      if (it != pending_requests_.end()) {
          int client_fd = it->second;
          send(client_fd, resp.data(), resp.size(), MSG_NOSIGNAL);
          pending_requests_.erase(it);
      }
  }

  bool ReadFromClientFd(int fd, string &buffer) {
      char buf[4096];
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) return false;
      buffer.append(buf, n);

      size_t newline;
      while ((newline = buffer.find('\n')) != string::npos) {
          string line = buffer.substr(0, newline);
          buffer.erase(0, newline + 1);
          if (!line.empty() && line.back() == '\r') line.pop_back();

          if (line.rfind("get ", 0) == 0) {
              string key = line.substr(4);
              if (kv_.count(key)) {
                  string val = kv_[key] + "\n";
                  send(fd, val.data(), val.size(), MSG_NOSIGNAL);
              } else {
                  string val = "NOT_FOUND\n";
                  send(fd, val.data(), val.size(), MSG_NOSIGNAL);
              }
          } else if (line.rfind("set ", 0) == 0) {
              size_t space = line.find(' ', 4);
              if (space != string::npos) {
                  string key = line.substr(4, space - 4);
                  string val = line.substr(space + 1);
                  if (leader_ == id_) {
                      LogEntry entry;
                      entry.set_data(key + "=" + val);
                      raft_->Propose(entry);
                      string resp = "OK\n";
                      send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
                  } else {
                      if (leader_.empty()) {
                          string resp = "NOT_LEADER\n";
                          send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
                      } else {
                          // Multiplex forward
                          SendForwardRequest(fd, line);
                      }
                  }
              } else {
                   string resp = "ERROR\n";
                   send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
              }
          } else {
               string resp = "UNKNOWN\n";
               send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
          }
      }
      return true;
  }

  // Forward request logic
  map<uint64_t, int> pending_requests_;
  uint64_t next_req_id_ = 1;

  void SendForwardRequest(int client_fd, const string& cmd) {
       int leader_fd = -1;
       // Find leader peer
       for (auto &p : peers_) {
           if (to_string(p.port) == leader_ || p.host + ":" + to_string(p.port) == leader_) {
               if (p.fd == -1) {
                   if (!ConnectToPeer(p)) {
                        break;
                   }
               }
               leader_fd = p.fd;
               break;
           }
       }

       if (leader_fd == -1) {
           string msg = "ERROR: No connection to leader\n";
           send(client_fd, msg.data(), msg.size(), MSG_NOSIGNAL);
           return;
       }

       uint64_t req_id = next_req_id_++;
       pending_requests_[req_id] = client_fd;

       string payload;
       payload.resize(8);
       memcpy(&payload[0], &req_id, 8);
       payload += cmd;
       SendTypedMessage(leader_fd, MSG_FWD_REQ, payload);
  }

  string id_;
  string leader_;
  int port_;
  vector<Peer> peers_;
  map<string, string> kv_;
  int wal_fd_;
  int listen_fd_;
  int client_listen_fd_;
  vector<Connection> incoming_;
  vector<Connection> clients_;
  vector<LogEntry> log_;
  RaftClass* raft_ = nullptr;
};

int main(int argc, char **argv) {
  if (argc < 3) {
    cerr << "Usage: " << argv[0] << " <id> <port> [peer_host:port ...]" << endl;
    return 1;
  }

  string id = argv[1];
  int port = atoi(argv[2]);
  vector<Peer> peers;

  // Parse Initial Config
  raft::RaftConfigPb initial_config;
  initial_config.add_node(id);

  for (int i = 3; i < argc; i++) {
     string p = argv[i];
     size_t colon = p.find(':');
     if (colon != string::npos) {
         Peer peer;
         peer.host = p.substr(0, colon);
         peer.port = atoi(p.substr(colon+1).c_str());
         peers.push_back(peer);
         initial_config.add_node(to_string(peer.port)); // Using port as ID for simplicity
     }
  }

  KVServer server(id, port, peers);
  raft::Raft<KVServer> *raft = raft::NewRaft(&server, id);
  server.set_raft(raft);

  // Bootstrap
  raft::RaftLogEntryPb config_entry;
  // config_entry.set_term(1);
  // config_entry.set_index(1);
  *config_entry.mutable_config() = initial_config;

  raft->Recover(config_entry);

  raft->Start(0.0, (int64_t)port); // Initialize with time 0 and seed=port

  cout << "Server " << id << " started on port " << port << endl;

  server.Run(); // Moves loop into server class

  return 0;
}
