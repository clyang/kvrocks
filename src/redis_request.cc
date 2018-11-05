#include <glog/logging.h>
#include <iostream>

#include "redis_cmd.h"
#include "redis_replication.h"
#include "redis_reply.h"
#include "redis_request.h"
#include "storage.h"

namespace Redis {

void Connection::OnRead(struct bufferevent *bev, void *ctx) {
  DLOG(INFO) << "on read: " << bufferevent_getfd(bev);
  auto conn = static_cast<Connection *>(ctx);

  conn->req_.Tokenize(conn->Input());
  conn->req_.ExecuteCommands(conn->Output(), conn);
}

void Connection::OnEvent(bufferevent *bev, short events, void *ctx) {
  auto conn = static_cast<Connection *>(ctx);
  if (events & BEV_EVENT_ERROR) {
    LOG(ERROR) << "bev error: "
               << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    DLOG(INFO) << "deleted: fd=" << conn->GetFD();
    delete conn;
    return;
  }
  if (events & BEV_EVENT_TIMEOUT) {
    LOG(INFO) << "timeout, fd=" << conn->GetFD();
    bufferevent_enable(bev, EV_READ | EV_WRITE);
  }
}

int Connection::GetFD() { return bufferevent_getfd(bev_); }

evbuffer *Connection::Input() { return bufferevent_get_input(bev_); }

evbuffer *Connection::Output() { return bufferevent_get_output(bev_); }

void Request::Tokenize(evbuffer *input) {
  char *line;
  size_t len;
  while (true) {
    switch (state_) {
      case ArrayLen:
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line) return;
        multi_bulk_len_ = len > 0 ? std::strtoull(line + 1, nullptr, 10) : 0;
        free(line);
        state_ = BulkLen;
        break;
      case BulkLen:
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line) return;
        bulk_len_ = std::strtoull(line + 1, nullptr, 10);
        free(line);
        state_ = BulkData;
        break;
      case BulkData:
        if (evbuffer_get_length(input) < bulk_len_ + 2) return;
        char *data =
            reinterpret_cast<char *>(evbuffer_pullup(input, bulk_len_ + 2));
        tokens_.emplace_back(data, bulk_len_);
        evbuffer_drain(input, bulk_len_ + 2);
        --multi_bulk_len_;
        if (multi_bulk_len_ <= 0) {
          state_ = ArrayLen;
          commands_.push_back(std::move(tokens_));
          tokens_.clear();
        } else {
          state_ = BulkLen;
        }
        break;
    }
  }
}

void Request::ExecuteCommands(evbuffer *output, Connection *conn) {
  if (commands_.empty()) return;

  std::unique_ptr<Commander> cmd;
  std::string reply;
  for (auto &cmd_tokens : commands_) {
    auto s = LookupCommand(cmd_tokens.front(), &cmd);
    if (!s.IsOK()) {
      // FIXME: change the err string
      Redis::Reply(output, Redis::Error("unknown command"));
      continue;
    }
    int arity = cmd->GetArity();
    if ((arity > 0 && cmd_tokens.size() != arity)
        || (arity < 0 && cmd_tokens.size() < -arity)) {
      Redis::Reply(output, Redis::Error("wrong number of arguments"));
      continue;
    }
    s = cmd->Parse(cmd_tokens);
    if (!s.IsOK()) {
      Redis::Reply(output, Redis::Error(s.msg()));
      continue;
    }
    if (!cmd->IsSidecar()) {
      s = cmd->Execute(svr_, &reply);
      if (!s.IsOK()) {
        Redis::Reply(output, Redis::Error(s.msg()));
        continue;
      }
      Redis::Reply(output, reply);
    } else {
      // Remove the bev from base, the thread will take over the bev
      auto bev = conn->DetachBufferEvent();
      TakeOverBufferEvent(bev);
      // NOTE: from now on, the bev is managed by the replication thread.
      // start the replication thread
      assert(bev != nullptr);
      auto t = new SidecarCommandThread(std::move(cmd), bev, svr_);
      t->Start();
      // TODO: track this thread in Server class; delete the req and sidecar obj
      // when done.
      return;  // NOTE: we break out the pipeline, even some commands left
    }
  }
  commands_.clear();
}

}  // namespace Redis