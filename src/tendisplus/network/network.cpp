// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this tendis open source
// project for additional information.

#include "tendisplus/network/network.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#include "tendisplus/server/server_entry.h"
#include "tendisplus/storage/varint.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/test_util.h"

namespace tendisplus {

using asio::ip::tcp;

#if defined(TENDIS_DEBUG) && !defined(_WIN32)
void printShellResult(std::string cmd) {
  std::string cmdFull = cmd + " 2>&1";
  char buffer[1024];
  FILE* fp = popen(cmdFull.c_str(), "r");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
    std::cerr << buffer;
    LOG(ERROR) << buffer;
  }
  pclose(fp);
}

void printPortRunningInfo(uint32_t port) {
  std::string err =
    "running process info for port " + std::to_string(port) + ":";
  std::cerr << err.c_str();
  LOG(ERROR) << err;
  std::string cmdPid =
    "pid=`lsof -i:" + std::to_string(port) + "|tail -1|awk '{print $2}'`;";
  printShellResult("netstat |grep " + std::to_string(port));
  printShellResult("lsof -i:" + std::to_string(port));
  printShellResult(cmdPid + "ls -l /proc/$pid/exe");
  printShellResult(cmdPid + "ls -l /proc/$pid/cwd");
  printShellResult(cmdPid + "ps aux|grep $pid|grep -v grep");
}
#endif

constexpr ssize_t REDIS_IOBUF_LEN = (1024 * 16);
constexpr ssize_t REDIS_MAX_QUERYBUF_LEN = (1024 * 1024 * 1024);
constexpr ssize_t REDIS_INLINE_MAX_SIZE = (1024 * 64);
constexpr ssize_t REDIS_MBULK_BIG_ARG = (1024 * 32);
const int BUFFER_LONG_SIZE = 10 * 1024 * 1024;

std::string RequestMatrix::toString() const {
  std::stringstream ss;
  ss << "\nprocessed\t" << processed << "\nprocessCost\t" << processCost << "ns"
     << "\nsendPacketCost\t" << sendPacketCost << "ns";
  return ss.str();
}

void RequestMatrix::reset() {
  processed = 0;
  processCost = 0;
  sendPacketCost = 0;
}

RequestMatrix RequestMatrix::operator-(const RequestMatrix& right) {
  RequestMatrix result;
  result.processed = processed - right.processed;
  result.processCost = processCost - right.processCost;
  result.sendPacketCost = sendPacketCost - right.sendPacketCost;
  return result;
}

std::string NetworkMatrix::toString() const {
  std::stringstream ss;
  ss << "\nstickyPackets\t" << stickyPackets << "\nconnCreated\t" << connCreated
     << "\nconnReleased\t" << connReleased << "\ninvalidPackets\t"
     << invalidPackets;
  return ss.str();
}

void NetworkMatrix::reset() {
  stickyPackets = 0;
  connCreated = 0;
  connReleased = 0;
  invalidPackets = 0;
}

NetworkMatrix NetworkMatrix::operator-(const NetworkMatrix& right) {
  NetworkMatrix result;
  result.stickyPackets = stickyPackets - right.stickyPackets;
  result.connCreated = connCreated - right.connCreated;
  result.connReleased = connReleased - right.connReleased;
  result.invalidPackets = invalidPackets - right.invalidPackets;
  return result;
}

NetworkAsio::NetworkAsio(std::shared_ptr<ServerEntry> server,
                         std::shared_ptr<NetworkMatrix> netMatrix,
                         std::shared_ptr<RequestMatrix> reqMatrix,
                         std::shared_ptr<ServerParams> cfg,
                         const std::string& name)
  : _connCreated(0),
    _server(server),
    _acceptCtx(nullptr),
    _acceptor(nullptr),
    _acceptThd(nullptr),
    _acceptCtx2(nullptr),
    _acceptor2(nullptr),
    _acceptThd2(nullptr),
    _isRunning(false),
    _netMatrix(netMatrix),
    _reqMatrix(reqMatrix),
    _cfg(cfg),
    _name(name) {}

#ifdef _WIN32
void NetworkAsio::releaseForWin() {
  _server.reset();
  _cfg.reset();
  _netMatrix.reset();
  _reqMatrix.reset();
}

#endif

std::shared_ptr<asio::io_context> NetworkAsio::getRwCtx() {
  // if (_rwCtxList.size() != _rwThreads.size() || _rwCtxList.size() == 0) {
  if (_rwThreads.size() == 0 || _rwCtxList.size() == 0) {
    return NULL;
  }
  int rand = std::rand();
  int index = rand % _rwCtxList.size();
  return _rwCtxList[index];
}

std::shared_ptr<asio::io_context> NetworkAsio::getRwCtx(
  asio::ip::tcp::socket& socket) {
  for (auto& rwCtx : _rwCtxList) {
    if (&(socket.get_io_context()) == &(*rwCtx)) {
      return rwCtx;
    }
  }
  if (&(socket.get_io_context()) == &(*_acceptCtx)) {
    LOG(WARNING) << "NetworkAsio getRwCtx equal _acceptCtx";
  }
  LOG(WARNING) << "NetworkAsio getRwCtx return NULL";
  return NULL;
}

std::unique_ptr<BlockingTcpClient> NetworkAsio::createBlockingClient(
  size_t readBuf, uint64_t rateLimit) {
  auto rwCtx = getRwCtx();
  INVARIANT(rwCtx != nullptr);
  return std::make_unique<BlockingTcpClient>(
    rwCtx, readBuf, _cfg->netBatchSize, _cfg->netBatchTimeoutSec, rateLimit);
}

std::unique_ptr<BlockingTcpClient> NetworkAsio::createBlockingClient(
  asio::ip::tcp::socket socket,
  size_t readBuf,
  uint64_t rateLimit,
  uint32_t netBatchTimeoutSec) {
  if (netBatchTimeoutSec == 0) {
    netBatchTimeoutSec = _cfg->netBatchTimeoutSec;
  }
  auto rwCtx = getRwCtx(socket);
  INVARIANT(rwCtx != nullptr);
  return std::make_unique<BlockingTcpClient>(rwCtx,
                                             std::move(socket),
                                             readBuf,
                                             _cfg->netBatchSize,
                                             netBatchTimeoutSec,
                                             rateLimit);
}

Status NetworkAsio::prepare(const std::string& ip,
                            const std::string& ip2,
                            const uint16_t port,
                            uint32_t netIoThreadNum) {
  _ip = ip;
  _port = port;
  _netIoThreadNum = netIoThreadNum;
  _ip2 = ip2;
  if (_ip2 != "" && (_ip == "0.0.0.0" || _ip2 == "0.0.0.0")) {
    LOG(ERROR) << "be careful, bind two ip,"
               << " and one of them is 0.0.0.0 may have some error!";
  }
  _acceptCtx = std::make_shared<asio::io_context>();
  Status s = prepareAccept(_ip, _port, _acceptCtx, _acceptor);
  if (!s.ok()) {
    return s;
  }
  if (_ip2 != "") {
    _acceptCtx2 = std::make_shared<asio::io_context>();
    s = prepareAccept(_ip2, _port, _acceptCtx2, _acceptor2);
    if (!s.ok()) {
      return s;
    }
  }
  s = startThread();
  if (!s.ok()) {
    return s;
  }
  return {ErrorCodes::ERR_OK, ""};
}

Status NetworkAsio::prepareAccept(
  const std::string& ip,
  const uint16_t port,
  std::shared_ptr<asio::io_context>& acceptCtx,
  std::shared_ptr<asio::ip::tcp::acceptor>& acceptor) {
  bool supportDomain = _server->getParams()->domainEnabled;

  try {
    asio::ip::tcp::endpoint ep;
    LOG(INFO) << "NetworkAsio::prepare ip:" << ip << " port:" << port;
    /*NOTE(wayenchen) if bind domain name, use resolver to get endpoint*/
    if (supportDomain) {
      asio::ip::tcp::resolver resolver(*acceptCtx);
      std::stringstream ss;
      ss << port;
      asio::ip::tcp::resolver::query query(ip, ss.str());
      asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
      ep = iter->endpoint();
    } else {
      asio::ip::address address = asio::ip::make_address(ip);
      ep = tcp::endpoint(address, port);
    }
    std::error_code ec;
    acceptor = std::make_shared<tcp::acceptor>(*acceptCtx, ep);
    acceptor->set_option(tcp::acceptor::reuse_address(true));
    acceptor->non_blocking(true, ec);
    if (ec.value()) {
      return {ErrorCodes::ERR_NETWORK, ec.message()};
    }
  } catch (const std::exception& e) {
#if defined(TENDIS_DEBUG) && !defined(_WIN32)
    printPortRunningInfo(port);
#endif
    return {ErrorCodes::ERR_NETWORK, e.what()};
  }
  return {ErrorCodes::ERR_OK, ""};
}

Expected<uint64_t> NetworkAsio::client2Session(
  std::shared_ptr<BlockingTcpClient> c, bool migrateOnly) {
  if (c->getReadBufSize() > 0) {
    return {ErrorCodes::ERR_NETWORK,
            "client still have buf unread, cannot transfer to a session"};
  }
  uint64_t connId = _connCreated.fetch_add(1, std::memory_order_relaxed);
  auto sess = std::make_shared<NetSession>(
    _server, std::move(c->borrowConn()), connId, true, _netMatrix, _reqMatrix);
  LOG(INFO) << "new net session, id:" << sess->id() << ",connId:" << connId
            << ",from:" << sess->getRemoteRepr() << " client2Session";
  sess->getCtx()->setAuthed();
  sess->getCtx()->setFlags(c->getFlags());
  sess->getCtx()->setReplOnly(migrateOnly);
  _server->addSession(sess);
  ++_netMatrix->connCreated;
  return sess->id();
}

Expected<std::shared_ptr<ClusterSession>> NetworkAsio::client2ClusterSession(
  std::shared_ptr<BlockingTcpClient> c) {
  if (c->getReadBufSize() > 0) {
    return {ErrorCodes::ERR_NETWORK,
            "client still have buf unread, cannot transfer to a session"};
  }
  uint64_t connId = _connCreated.fetch_add(1, std::memory_order_relaxed);
  auto sess = std::make_shared<ClusterSession>(
    _server, std::move(c->borrowConn()), connId, true, _netMatrix, _reqMatrix);
  LOG(INFO) << "client2ClusterSession, id:" << sess->id()
            << ",connId:" << connId << ",remoteAddr:" << sess->getRemoteRepr();
  sess->getCtx()->setAuthed();
  _server->addSession(sess);
  ++_netMatrix->connCreated;
  return sess;
}

template <typename T>
void NetworkAsio::doAccept(std::shared_ptr<asio::ip::tcp::acceptor>& acceptor) {
  int index = _connCreated % _rwCtxList.size();
  auto cb = [this, index, &acceptor](const std::error_code& ec,
                                     tcp::socket socket) {
    if (!_isRunning.load(std::memory_order_relaxed)) {
      LOG(INFO) << "acceptCb, server is shuting down";
      return;
    }
    if (ec.value()) {
      LOG(WARNING) << "acceptCb errorcode:" << ec.message();
      // we log this error, but dont return
    }

    uint64_t newConnId = _connCreated.fetch_add(1, std::memory_order_relaxed);
    auto sess = std::make_shared<T>(
      _server, std::move(socket), newConnId, true, _netMatrix, _reqMatrix);
    DLOG(INFO) << "new net session, id:" << sess->id()
               << ",connId:" << newConnId << ",from:" << sess->getRemoteRepr()
               << " created";
    // TODO(wayenchen): check whether clusterSession should add to
    // ServerEntry::_sessions.
    if (_server->addSession(std::move(sess))) {
      ++_netMatrix->connCreated;
    }

    doAccept<T>(acceptor);
  };
  auto rwCtx = _rwCtxList[index];
  acceptor->async_accept(*rwCtx, std::move(cb));
}

void NetworkAsio::stop() {
  LOG(INFO) << "network-asio begin stops...";
  _isRunning.store(false, std::memory_order_relaxed);
  _acceptCtx->stop();
  if (_ip2 != "") {
    _acceptCtx2->stop();
  }
  for (auto& rwCtx : _rwCtxList) {
    rwCtx->stop();
  }
  _acceptThd->join();
  if (_ip2 != "") {
    _acceptThd2->join();
  }
  for (auto& v : _rwThreads) {
    v.join();
  }
  LOG(INFO) << "network-asio stops complete...";
}

Status NetworkAsio::startAcceptThread(
  std::shared_ptr<std::thread>& acceptThd,
  std::shared_ptr<asio::io_context>& acceptCtx) {
  acceptThd = std::make_shared<std::thread>([this, &acceptCtx] {
    std::string threadName = _name + "-accept";
    threadName.resize(15);
    INVARIANT(!pthread_setname_np(pthread_self(), threadName.c_str()));
    while (_isRunning.load(std::memory_order_relaxed)) {
      // if no work-gurad, the run() returns immediately if no other tasks
      asio::io_context::work work(*acceptCtx);
      try {
        acceptCtx->run();
      } catch (const std::exception& ex) {
        LOG(FATAL) << "accept failed:" << ex.what();
      } catch (...) {
        LOG(FATAL) << "unknown exception";
      }
    }
  });
  return {ErrorCodes::ERR_OK, ""};
}
Status NetworkAsio::startThread() {
  _isRunning.store(true, std::memory_order_relaxed);
  startAcceptThread(_acceptThd, _acceptCtx);
  if (_ip2 != "") {
    startAcceptThread(_acceptThd2, _acceptCtx2);
  }

  LOG(INFO) << "NetworkAsio::run netIO _netIoThreadNum:" << _netIoThreadNum;
  for (size_t i = 0; i < _netIoThreadNum; ++i) {
    _rwCtxList.push_back(std::make_shared<asio::io_context>());
  }
  for (size_t i = 0; i < _netIoThreadNum; ++i) {
    std::thread thd([this, i] {
      std::string threadName = _name + "-rw-" + std::to_string(i);
      threadName.resize(15);
      INVARIANT_D(!pthread_setname_np(pthread_self(), threadName.c_str()));
      LOG(INFO) << "add netIoThread, i:" << i
                << ", threadId:" << getCurThreadId();
      while (_isRunning.load(std::memory_order_relaxed)) {
        // if no workguard, the run() returns immediately if no
        // tasks
        asio::io_context::work work(*(_rwCtxList[i]));
        try {
          _rwCtxList[i]->run();
        } catch (const std::exception& ex) {
          LOG(FATAL) << "read/write thd failed:" << ex.what();
        } catch (...) {
          LOG(FATAL) << "unknown exception";
        }
      }
    });
    _rwThreads.emplace_back(std::move(thd));
  }
  return {ErrorCodes::ERR_OK, ""};
}

Status NetworkAsio::run(bool forGossip) {
  // TODO(deyukong): acceptor needs no explicitly listen.
  // but only through listen can we configure backlog.
  // _acceptor->listen(BACKLOG);
  if (!forGossip) {
    doAccept<NetSession>(_acceptor);
    if (_ip2 != "") {
      doAccept<NetSession>(_acceptor2);
    }
  } else {
    doAccept<ClusterSession>(_acceptor);
  }
  return {ErrorCodes::ERR_OK, ""};
}

NetSession::NetSession(std::shared_ptr<ServerEntry> server,
                       tcp::socket&& sock,
                       uint64_t connid,
                       bool initSock,
                       std::shared_ptr<NetworkMatrix> netMatrix,
                       std::shared_ptr<RequestMatrix> reqMatrix,
                       Session::Type type)
  : Session(server, type),
    _connId(connid),
    _closeAfterRsp(false),
    _state(State::Created),
    _sock(std::move(sock)),
    _queryBuf(std::vector<char>()),
    _queryBufPos(0),
    _reqType(RedisReqMode::REDIS_REQ_UNKNOWN),
    _multibulklen(0),
    _bulkLen(-1),
    _isSendRunning(false),
    _callbackCanWrite(false),
    _isEnded(false),
    _sendBufferBytes(0),
    _sendBufferBackBytes(0),
    _closeResponse(false),
    _netMatrix(netMatrix),
    _reqMatrix(reqMatrix),
    _commandUsedMemory(0),
    _hardMemoryLimit(0),
    _haveExceedHardLimit(false),
    _softMemoryLimit(0),
    _softMemoryLimitSeconds(0),
    _haveExceedSoftLimit(false),
    _firstTimePoint(SCLOCK::now()),
    _softLimitReachedTime(_firstTimePoint) {
  if (initSock) {
    std::error_code ec;
    _sock.non_blocking(true, ec);
    INVARIANT_D(ec.value() == 0);
    _sock.set_option(tcp::no_delay(true));
    _sock.set_option(asio::socket_base::keep_alive(true));
    // TODO(deyukong): keep-alive params
  }
  resetMemoryLimit();

  DLOG(INFO) << "net session, id:" << id() << ",connId:" << _connId
             << " createad";
  _first = true;
}

// NOTE(vinchen): It is useless,
// because NetSession is always in serverEntry::_sessions[],
// it can't be destructed.
// NetSession::~NetSession() {
//    endSession();
//}

void NetSession::setState(State s) {
  _state.store(s, std::memory_order_relaxed);
}

std::string NetSession::getRemote() const {
  return getRemoteRepr();
}

Expected<std::string> NetSession::getRemoteIp() const {
  try {
    if (_sock.is_open()) {
      return _sock.remote_endpoint().address().to_string();
    }
    return {ErrorCodes::ERR_NETWORK, "the session is closed"};
  } catch (const std::exception& e) {
    return {ErrorCodes::ERR_NETWORK, e.what()};
  }
}

Expected<uint32_t> NetSession::getRemotePort() const {
  try {
    if (_sock.is_open()) {
      return _sock.remote_endpoint().port();
    }
    return {ErrorCodes::ERR_NETWORK, "the session is closed"};
  } catch (const std::exception& e) {
    return {ErrorCodes::ERR_NETWORK, e.what()};
  }
}

Expected<std::string> NetSession::getLocalIp() const {
  try {
    if (_sock.is_open()) {
      return _sock.local_endpoint().address().to_string();
    }
    return {ErrorCodes::ERR_NETWORK, "the session is closed"};
  } catch (const std::exception& e) {
    return {ErrorCodes::ERR_NETWORK, e.what()};
  }
}

Expected<uint32_t> NetSession::getLocalPort() const {
  try {
    if (_sock.is_open()) {
      return _sock.local_endpoint().port();
    }
    return {ErrorCodes::ERR_NETWORK, "the session is closed"};
  } catch (const std::exception& e) {
    return {ErrorCodes::ERR_NETWORK, e.what()};
  }
}

int NetSession::getFd() {
  return _sock.native_handle();
}

std::string NetSession::getRemoteRepr() const {
  try {
    if (_sock.is_open()) {
      std::stringstream ss;
      ss << _sock.remote_endpoint().address().to_string() << ":"
         << _sock.remote_endpoint().port();
      return ss.str();
    }
    return "closed conn";
  } catch (const std::exception& e) {
    return e.what();
  }
}

std::string NetSession::getLocalRepr() const {
  if (_sock.is_open()) {
    std::stringstream ss;
    ss << _sock.local_endpoint().address().to_string() << ":"
       << _sock.local_endpoint().port();
    return ss.str();
  }
  return "closed conn";
}

void NetSession::schedule() {
  // incr the reference, so it's safe to remove sessions
  // from _serverEntry at executing time.
  auto self(shared_from_this());
  _server->schedule([this, self]() { stepState(); }, _ioCtxId);
}

asio::ip::tcp::socket NetSession::borrowConn() {
  return std::move(_sock);
}

asio::ip::tcp::socket* NetSession::getSock() {
  return &_sock;
}

Status NetSession::setResponse(std::string&& s) {
  std::lock_guard<std::mutex> lk(_mutex);
  // when writing response to socket, check memory limit first.
  // memory used = content(will be in sendbuffer)
  //             + sendbuffer(current) + _sendBufferBack(maybe not empty)
  _commandUsedMemory =
    s.size() + _sendBufferBytes + _sendBufferBackBytes;
  auto status = checkMemLimit();
  if (!status.ok()) {
    if (_server) {
      _server->getServerStat().memlimitExceededTimes.fetch_add(
        1, std::memory_order_relaxed);
      LOG(WARNING) << "sess: " << id()
                   << " exceed client-output-buffer-limit-normal."
                   << " current memory used: " << _commandUsedMemory;
    }
    return status;
  }

  if (_isEnded) {
    _closeAfterRsp = true;
    LOG(WARNING) << "setResponse _isEnded, id:" << id()
                 << " addr:" << getRemoteRepr();
    return {ErrorCodes::ERR_NETWORK, "connection is ended"};
  }

  if (s.empty()) {
    LOG(WARNING) << "setResponse response is empty, id:" << id()
                 << " addr:" << getRemoteRepr();
    return {ErrorCodes::ERR_OK, ""};
  }

  // Session closing,Already send the last response,needn't sent again
  if (_closeAfterRsp) {
    if (_closeResponse) {
      LOG(WARNING) << "setResponse _closeAfterRsp _closeResponse, id:" << id()
                   << " addr:" << getRemoteRepr();
      return {ErrorCodes::ERR_OK, ""};
    } else {
      _closeResponse = true;
    }
  }

#define NET_SEND_RSP_BATCH_SIZE 1400
  if (_isSendRunning) {
    _sendBufferBackBytes += s.size();
    //Afterwards, s is an empty string, content is moved to vector
    _sendBufferBack.push_back(std::move(s));
  } else {
    _sendBufferBytes += s.size();
    //Afterwards, s is an empty string, content is moved to vector
    _sendBuffer.push_back(std::move(s));
    if (_sendBufferBytes > NET_SEND_RSP_BATCH_SIZE) {
      drainRspWithoutLock();
    }
  }
  resetMemoryLimit();
  return {ErrorCodes::ERR_OK, ""};
}

void NetSession::start() {
  stepState();
}

Status NetSession::cancel() {
  std::error_code ec;
  _sock.cancel(ec);
  if (!ec) {
    return {ErrorCodes::ERR_OK, ""};
  }
  return {ErrorCodes::ERR_NETWORK, ec.message()};
}

// only for test!
void NetSession::setArgs(const std::vector<std::string>& args) {
  _args = args;
  _ctx->setReadPacketTs(nsSinceEpoch());
  _ctx->setArgsBrief(args);
}

const std::vector<std::string>& NetSession::getArgs() const {
  return _args;
}

void NetSession::setCloseAfterRsp() {
  _closeAfterRsp = true;
}

void NetSession::setRspAndClose(const std::string& s) {
  TEST_SYNC_POINT_CALLBACK("NetSession::setRspAndClose", (void*)&s);
  _closeAfterRsp = true;
  setResponse(redis_port::errorReply(s));
  resetMultiBulkCtx();
}

void NetSession::processInlineBuffer() {
  char* newline = nullptr;
  std::vector<std::string> argv;
  std::string aux;
  size_t querylen;
  size_t linefeed_chars = 1;

  /* Search for end of line */
  newline = strchr(_queryBuf.data(), '\n');

  /* Nothing to do without a \r\n */
  if (newline == NULL) {
    if (_queryBufPos > REDIS_INLINE_MAX_SIZE) {
      ++_netMatrix->invalidPackets;
      setRspAndClose("Protocol error: too big inline request");
      setState(State::Stop);
      return;
    }
    setState(State::DrainReqNet);
    return;
  }

  /* Handle the \r\n case. */
  if (newline && newline != _queryBuf.data() && *(newline - 1) == '\r') {
    newline--;
    linefeed_chars++;
  }

  /* Split the input buffer up to the \r\n */
  querylen = newline - (_queryBuf.data());
  aux = std::string(_queryBuf.data(), querylen);
  auto ret = redis_port::splitargs(argv, aux);
  if (ret == NULL) {
    setRspAndClose("Protocol error: unbalanced quotes in request");
    setState(State::Stop);
    return;
  }

  /* Leave data after the first line of the query in the buffer */
  shiftQueryBuf(querylen + linefeed_chars, -1);

  if (_args.size() != 0) {
    LOG(FATAL) << "BUG: _args.size:" << _args.size() << " not empty";
  }

  for (auto& v : argv) {
    if (v.length() != 0) {
      _args.emplace_back(v);
    }
  }

  setState(State::Process);
  processReq();
}

// NOTE(deyukong): mainly port from redis::networking.c,
// func:processMultibulkBuffer, the unportable part (long long, int and so on)
// are all from the redis source code, quite ugly.
// FIXME(deyukong): rewrite into a more c++ like code.
void NetSession::processMultibulkBuffer() {
  char* newLine = nullptr;
  long long ll;  // NOLINT(runtime/int)
  int pos = 0;
  int ok = 0;
  if (_multibulklen == 0) {
    newLine = strchr(_queryBuf.data(), '\r');
    if (newLine == nullptr) {
      if (_queryBufPos > REDIS_INLINE_MAX_SIZE) {
        ++_netMatrix->invalidPackets;
        setRspAndClose("Protocol error: too big mbulk count string");
        setState(State::Stop);
        return;
      }
      // not complete line
      setState(State::DrainReqNet);
      return;
    }
    /* Buffer should also contain \n */
    if (newLine - _queryBuf.data() > _queryBufPos - 2) {
      // not complete line
      setState(State::DrainReqNet);
      return;
    }

    /* We know for sure there is a whole line since newline != NULL,
     * so go ahead and find out the multi bulk length. */
    if (_queryBuf[0] != '*') {
      LOG(ERROR) << "multiBulk first char not *";
      ++_netMatrix->invalidPackets;
      setRspAndClose("Protocol error: multiBulk first char not *");
      setState(State::Stop);
      return;
    }
    char* newStart = _queryBuf.data() + 1;
    ok = redis_port::string2ll(newStart, newLine - newStart, &ll);
    if (!ok || ll > 1024 * 1024) {
      ++_netMatrix->invalidPackets;
      setRspAndClose("Protocol error: invalid multibulk length");
      setState(State::Stop);
      return;
    }
    pos = newLine - _queryBuf.data() + 2;
    if (ll <= 0) {
      shiftQueryBuf(pos, -1);

      INVARIANT(_args.size() == 0);
      setState(State::Process);
      processReq();
      return;
    }
    _multibulklen = ll;
  }

  INVARIANT(_multibulklen > 0);

  while (_multibulklen) {
    if (_bulkLen == -1) {
      newLine = strchr(_queryBuf.data() + pos, '\r');
      if (newLine == nullptr) {
        // NOTE(vinchen): For logical correctly, here it should minus
        // pos. In fact, it is also a bug for redis. But because of the
        // REDIS_MBULK_BIG_ARG optimization, it is not a problem in
        // redis now.
        if (_queryBufPos - pos > REDIS_INLINE_MAX_SIZE) {
          ++_netMatrix->invalidPackets;
          LOG(ERROR) << "_multibulklen = " << _multibulklen
                     << ", _queryBufPos = " << _queryBufPos << ", pos =" << pos;
          INVARIANT_D(0);
          setRspAndClose("Protocol error: too big bulk count string");
          setState(State::Stop);
          return;
        }
        break;
      }

      /* Buffer should also contain \n */
      if (newLine - _queryBuf.data() > _queryBufPos - 2) {
        break;
      }
      if (_queryBuf.data()[pos] != '$') {
        std::stringstream s;
        ++_netMatrix->invalidPackets;
        s << "Protocol error: expected '$', got '" << _queryBuf.data()[pos]
          << "'";
        setRspAndClose(s.str());
        setState(State::Stop);
        return;
      }
      char* newStart = _queryBuf.data() + pos + 1;
      ok = redis_port::string2ll(newStart, newLine - newStart, &ll);

      uint32_t maxBulkLen = CONFIG_DEFAULT_PROTO_MAX_BULK_LEN;
      if (getServerEntry()) {
        maxBulkLen = getServerEntry()->protoMaxBulkLen();
      }
      if (!ok || ll < 0 || ll > maxBulkLen) {
        ++_netMatrix->invalidPackets;
        setRspAndClose("Protocol error: invalid bulk length");
        setState(State::Stop);
        return;
      }
      pos += newLine - (_queryBuf.data() + pos) + 2;
      // the optimization of ll >= REDIS_MBULK_BIG_ARG
      // is not ported from redis
      if (ll >= REDIS_MBULK_BIG_ARG) {
        /* If we are going to read a large object from network
         * try to make it likely that it will start at c->querybuf
         * boundary so that we can optimize object creation
         * avoiding a large copy of data. */
        shiftQueryBuf(pos, -1);
        pos = 0;
      }
      _bulkLen = ll;
    }
    if (_queryBufPos - pos < _bulkLen + 2) {
      // not complete
      break;
    } else {
      // TODO(vinchen): There is a optimization is not ported from redis
      _args.push_back(std::string(_queryBuf.data() + pos, _bulkLen));
      pos += _bulkLen + 2;
      _bulkLen = -1;
      _multibulklen -= 1;
    }
  }
  if (pos != 0) {
    shiftQueryBuf(pos, -1);
  }
  if (_multibulklen == 0) {
    setState(State::Process);
    processReq();
  } else {
    setState(State::DrainReqNet);
  }
}

void NetSession::drainReqCallback(const std::error_code& ec, size_t actualLen) {
  if (ec) {
    DLOG(WARNING) << "drainReqCallback:" << ec.message();
    endSession();
    return;
  }

  const auto guard = MakeGuard([&] {
    if (_state == State::Stop) {
      drainRsp();
    }
  });

  if (_server) {
    // TODO(vinchen): Is it right when cluster is enable?
    _server->getServerStat().netInputBytes += actualLen;
  }

  // TODO(wayenchen): include clusterSession?
  if (_first && getServerEntry()) {
    uint32_t maxClients = getServerEntry()->getParams()->maxClients;
    if (getServerEntry()->getSessionCount() > maxClients) {
      ++_server->getServerStat().rejectedConn;

      LOG(WARNING) << "-ERR max number of clients reached, clients: "
                   << maxClients;
      setRspAndClose("-ERR max number of clients reached\r\n");
      setState(State::Stop);
      return;
    }
    _first = false;
  }

  State curr = _state.load(std::memory_order_relaxed);
  INVARIANT(curr == State::DrainReqBuf || curr == State::DrainReqNet);

  _queryBufPos += actualLen;
  _queryBuf[_queryBufPos] = 0;
  if (_queryBufPos > REDIS_MAX_QUERYBUF_LEN) {
    ++_netMatrix->invalidPackets;
    setRspAndClose("Closing client that reached max query buffer length");
    setState(State::Stop);
    return;
  }
  setState(State::DrainReqBuf);
  schedule();
}

void NetSession::parseAndProcessReq() {
  while (_state == State::DrainReqBuf) {
    if (_reqType == RedisReqMode::REDIS_REQ_UNKNOWN) {
      if (_queryBuf[0] == '*') {
        _reqType = RedisReqMode::REDIS_REQ_MULTIBULK;
      } else {
        _reqType = RedisReqMode::REDIS_REQ_INLINE;
      }
    }
    if (_reqType == RedisReqMode::REDIS_REQ_MULTIBULK) {
      processMultibulkBuffer();
    } else if (_reqType == RedisReqMode::REDIS_REQ_INLINE) {
      processInlineBuffer();
    } else {
      LOG(FATAL) << "unknown request type";
    }
  }
  drainRsp();
  if (_state == State::DrainReqNet) {
    drainReqNet();
  }
}

// NOTE(deyukong): an O(n) impl of array shifting, an alternative to sdsrange,
// which also has O(n) time-complexity
void NetSession::shiftQueryBuf(ssize_t start, ssize_t end) {
  if (_queryBufPos == 0) {
    return;
  }
  int64_t newLen = 0;
  if (start < 0) {
    start = _queryBufPos + start;
    if (start < 0) {
      start = 0;
    }
  }
  if (end < 0) {
    end = _queryBufPos + end;
    if (end < 0) {
      end = 0;
    }
  }
  newLen = (start > end) ? 0 : (end - start + 1);
  if (newLen != 0) {
    if (start >= _queryBufPos) {
      newLen = 0;
    } else if (end >= _queryBufPos) {
      end = _queryBufPos - 1;
      newLen = (start > end) ? 0 : (end - start) + 1;
    }
  } else {
    start = 0;
  }
  if (start && newLen) {
    memmove(_queryBuf.data(), _queryBuf.data() + start, newLen);
  }
  _queryBuf[newLen] = 0;
  _queryBufPos = newLen;
}

void NetSession::resetMultiBulkCtx() {
  _reqType = RedisReqMode::REDIS_REQ_UNKNOWN;
  _multibulklen = 0;
  _bulkLen = -1;
  _args.clear();
}

void NetSession::drainReqBuf() {
  INVARIANT(_queryBufPos != 0);
  drainReqCallback(std::error_code(), 0);
}

void NetSession::drainReqNet() {
  // we may do a sync-read to reduce async-callbacks
  size_t wantLen = REDIS_IOBUF_LEN;
  // here we use >= than >, so the last element will always be 0,
  // it's convinent for c-style string search
  if (wantLen + _queryBufPos >= _queryBuf.size()) {
    // the fill should be as fast as memset in 02 mode, refer to here
    // NOLINT(whitespace/line_length)
    // https://stackoverflow.com/questions/8848575/fastest-way-to-reset-every-value-of-stdvectorint-to-0)
    _queryBuf.resize((wantLen + _queryBufPos) * 2, 0);
  }

  // TODO(deyukong): I believe async_read_some wont callback if no
  // readable-event is set on the fd or this callback will be a deadloop
  // it needs futher tests
  auto self(shared_from_this());
  _sock.async_read_some(
    asio::buffer(_queryBuf.data() + _queryBufPos, wantLen),
    [this, self](const std::error_code& ec, size_t actualLen) {
      _ctx->setReadPacketTs(nsSinceEpoch());
      drainReqCallback(ec, actualLen);
    });
}

void NetSession::processReq() {
  bool continueSched = true;
  if (_args.size()) {
    _ctx->setProcessPacketStart(nsSinceEpoch());
    continueSched = _server->processRequest(reinterpret_cast<Session*>(this));
    _reqMatrix->processed += 1;
    _reqMatrix->processCost += nsSinceEpoch() - _ctx->getProcessPacketStart();
    _ctx->resetStatisticInfo();
  }
  if (!continueSched) {
    endSession();
    setState(State::Stop);
  } else if (!_closeAfterRsp) {
    resetMultiBulkCtx();
    if (_queryBufPos == 0) {
      setState(State::DrainReqNet);
      return;
    } else {
      setState(State::DrainReqBuf);
      ++_netMatrix->stickyPackets;
      return;
    }
  } else {
    // closeAfterRsp, donot process more requests
    // let drainRspCallback end this session
    setState(State::Stop);
  }
  return;
}

void NetSession::drainRsp() {
  std::lock_guard<std::mutex> lk(_mutex);
  if (_isSendRunning) {
    _callbackCanWrite = true;
    return;
  }
  drainRspWithoutLock();
}

void NetSession::drainRspWithoutLock() {
  if (_sendBuffer.size() == 0) {
    return;
  }
  uint64_t now = nsSinceEpoch();
  _isSendRunning = true;
  auto self(shared_from_this());
  if (_sendBufferBytes > BUFFER_LONG_SIZE) {
    LOG(WARNING) << "drainRspWithoutLock async_write long size:"
                 << _sendBufferBytes;
  }
  std::vector<asio::const_buffer> buffers;
  for (auto &buffer : _sendBuffer) {
    buffers.push_back(asio::buffer(buffer.c_str(), buffer.size()));
  }
  asio::async_write(
    _sock,
    buffers,
    [this, self, now](const std::error_code& ec, size_t actualLen) {
      _reqMatrix->sendPacketCost += nsSinceEpoch() - now;
      drainRspCallback(ec, actualLen);
    });
}

void NetSession::drainRspCallback(const std::error_code& ec, size_t actualLen) {
  if (ec) {
    LOG(WARNING) << "drainRspCallback:" << ec.message();
    endSession();
    return;
  }
  if (actualLen != _sendBufferBytes) {
    LOG(ERROR) << "conn:" << _connId << ",actualLen:" << actualLen
               << ",bufsize:" << _sendBuffer.size() << ",invalid drainRsp len";
    endSession();
    _sendBuffer.clear();
    _sendBufferBytes = 0;
    return;
  }
  if (_server) {
    // TODO(vinchen): Is it right when cluster = true
    _server->getServerStat().netOutputBytes += _sendBufferBytes;
  }

  {
    std::lock_guard<std::mutex> lk(_mutex);
    INVARIANT(_isSendRunning);
    _sendBuffer.clear();
    _sendBufferBytes = 0;
    _sendBuffer.swap(_sendBufferBack);
    _sendBufferBytes = _sendBufferBackBytes;
    _sendBufferBackBytes = 0;

    if (_callbackCanWrite && !_sendBuffer.empty()) {
      _callbackCanWrite = false;
      drainRspWithoutLock();
    } else {
      _callbackCanWrite = false;
      _isSendRunning = false;
    }
  }
  if (_closeResponse && _sendBuffer.empty()) {
    endSession();
  }
}

void NetSession::endSession() {
  {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_isEnded) {
      return;
    }
    _isEnded = true;
    ++_netMatrix->connReleased;
    DLOG(INFO) << "net session, id:" << id() << ",connId:" << _connId
               << " destroyed";
    // NOTE(tanninzhu): When we want to call endSession,
    // we actually want to release the Session object and
    // release the socket connection through RAII.
    // But when the socket is executing async_read_some,
    // its Session shared_ptr is held by ASIO, reading process is a deadloop,
    // and the loop cannot be ended by means other than the connection closing,
    // and the object cannot be released. So when we endSession,
    // we call shutdown to actively close his read connection,
    // end the deadloop and release the connection.
    try {
      if (_sock.is_open()) {
        _sock.shutdown(asio::ip::tcp::socket::shutdown_receive);
      }
    } catch (const std::exception& ex) {
      // NOTE(takenliu): it may be normal, change LOG level to INFO
      LOG(INFO) << "shutdown socket failed." << ex.what() << " id:" << id();
    }
  }
  _server->endSession(id());
}

void NetSession::stepState() {
  if (_state.load(std::memory_order_relaxed) == State::Created) {
    INVARIANT(_reqType == RedisReqMode::REDIS_REQ_UNKNOWN);
    INVARIANT(_multibulklen == 0 && _bulkLen == -1);
    setState(State::DrainReqNet);
  }
  auto currState = _state.load(std::memory_order_relaxed);
  switch (currState) {
    case State::DrainReqNet:
      drainReqNet();
      return;
    case State::DrainReqBuf:
      INVARIANT_D(_type != Session::Type::CLUSTER);
      parseAndProcessReq();
      return;
    case State::Process:
      INVARIANT_D(_type != Session::Type::NET);
      processReq();
      return;
    default:
      LOG(FATAL) << "connId:" << _connId
                 << ",invalid state:" << int32_t(currState);
  }
}

void NetSession::resetMemoryLimit() {
  if (!_haveExceedSoftLimit) {
    _softLimitReachedTime = _firstTimePoint;
  }
  _haveExceedHardLimit = false;
  _haveExceedSoftLimit = false;

  if (!_server) {
    return;
  }

  _hardMemoryLimit = _server->getParams()->clientOutputBufferLimitNormalHardMB
    << 20;
  _softMemoryLimit = _server->getParams()->clientOutputBufferLimitNormalSoftMB
    << 20;
  _softMemoryLimitSeconds =
    _server->getParams()->clientOutputBufferLimitNormalSoftSecond;

  _commandUsedMemory = 0;
}

Status NetSession::checkMemLimit() {
  if (_hardMemoryLimit && !_haveExceedHardLimit &&
      _commandUsedMemory >= _hardMemoryLimit) {
    _haveExceedHardLimit = true;
    return {ErrorCodes::ERR_MEMORY_LIMIT, ""};
  }
  if (_softMemoryLimit && !_haveExceedSoftLimit &&
      _commandUsedMemory >= _softMemoryLimit) {
    _haveExceedSoftLimit = true;
    if (_softLimitReachedTime == _firstTimePoint) {
      _softLimitReachedTime = SCLOCK::now();
    } else {
      if (_softMemoryLimitSeconds &&
          (SCLOCK::now() - _softLimitReachedTime >
           std::chrono::seconds(_softMemoryLimitSeconds))) {
        return {ErrorCodes::ERR_MEMORY_LIMIT, ""};
      }
    }
  }
  return {};
}

Status NetSession::memLimitRequest(uint64_t sizeUsed) {
  _commandUsedMemory += sizeUsed;
  return checkMemLimit();
}

}  // namespace tendisplus
