//  Copyright (c) 2007-2008 Facebook
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
// See accompanying file LICENSE or visit the Scribe site at:
// http://developers.facebook.com/scribe/
//
// @author Bobby Johnson
// @author James Wang
// @author Jason Sobel
// @author Avinash Lakshman


#include "common.h"
#include "scribe_server.h"
#include "conn_pool.h"

using std::string;
using std::ostringstream;
using std::map;
using boost::shared_ptr;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;
using namespace scribe::thrift;


ConnPool::ConnPool() {
  pthread_mutex_init(&mapMutex, NULL);
}

ConnPool::~ConnPool() {
  pthread_mutex_destroy(&mapMutex);
}

string ConnPool::makeKey(const string& hostname, unsigned long port) {
  string key(hostname);
  key += ":";

  ostringstream oss;
  oss << port;
  key += oss.str();
  return key;
}

bool ConnPool::open(const string& hostname, unsigned long port, int timeout) {
	return openCommon(makeKey(hostname, port),
                    shared_ptr<scribeConn>(new scribeConn(hostname, port, timeout)));
}

bool ConnPool::open(const string &service, const server_vector_t &servers, int timeout) {
	return openCommon(service,
                    shared_ptr<scribeConn>(new scribeConn(service, servers, timeout)));
}

void ConnPool::close(const string& hostname, unsigned long port) {
  closeCommon(makeKey(hostname, port));
}

void ConnPool::close(const string &service) {
  closeCommon(service);
}

bool ConnPool::send(const string& hostname, unsigned long port,
                    shared_ptr<logentry_vector_t> messages) {
  return sendCommon(makeKey(hostname, port), messages);
}

bool ConnPool::send(const string &service,
                    shared_ptr<logentry_vector_t> messages) {
  return sendCommon(service, messages);
}

bool ConnPool::openCommon(const string &key, shared_ptr<scribeConn> conn) {

  // note on locking:
  // The mapMutex locks all reads and writes to the connMap.
  // The locks on each connection serialize writes and deletion.
  // The lock on a connection doesn't protect its refcount, as refcounts
  // are only accessed under the mapMutex.
  // mapMutex MUST be held before attempting to lock particular connection

  pthread_mutex_lock(&mapMutex);
  conn_map_t::iterator iter = connMap.find(key);
  if (iter != connMap.end()) {
    (*iter).second->addRef();
    pthread_mutex_unlock(&mapMutex);
    return true;
  } else {
    // don't need to lock the conn yet, because no one know about
    // it until we release the mapMutex
    if (conn->open()) {
      // ref count starts at one, so don't addRef here
      connMap[key] = conn;
      pthread_mutex_unlock(&mapMutex);
      return true;
    } else {
      // conn object that failed to open is deleted
      pthread_mutex_unlock(&mapMutex);
      return false;
    }
  }
}

void ConnPool::closeCommon(const string &key) {
  pthread_mutex_lock(&mapMutex);
  conn_map_t::iterator iter = connMap.find(key);
  if (iter != connMap.end()) {
    (*iter).second->releaseRef();
    if ((*iter).second->getRef() <= 0) {
      (*iter).second->lock();
      (*iter).second->close();
      (*iter).second->unlock();
      connMap.erase(iter);
    }
  } else {
    // This can be bad. If one client double closes then other cleints are screwed
    LOG_OPER("LOGIC ERROR: attempting to close connection <%s> that connPool has no entry for", key.c_str());
  }
  pthread_mutex_unlock(&mapMutex);
}

bool ConnPool::sendCommon(const string &key,
                          shared_ptr<logentry_vector_t> messages) {
  pthread_mutex_lock(&mapMutex);
  conn_map_t::iterator iter = connMap.find(key);
  if (iter != connMap.end()) {
    (*iter).second->lock();
    pthread_mutex_unlock(&mapMutex);
    bool result = (*iter).second->send(messages);
    (*iter).second->unlock();
    return result;
  } else {
    LOG_OPER("send failed. No connection pool entry for <%s>", key.c_str());
    pthread_mutex_unlock(&mapMutex);
    return false;
  }
}

scribeConn::scribeConn(const string& hostname, unsigned long port, int timeout_)
  : refCount(1),
  smcBased(false),
  remoteHost(hostname),
  remotePort(port),
  timeout(timeout_) {
  pthread_mutex_init(&mutex, NULL);
#ifdef USE_ZOOKEEPER
  zkRegistrationZnode = hostname;
#endif
}

scribeConn::scribeConn(const string& service, const server_vector_t &servers, int timeout_)
  : refCount(1),
  smcBased(true),
  smcService(service),
  serverList(servers),
  timeout(timeout_) {
  pthread_mutex_init(&mutex, NULL);
}

scribeConn::~scribeConn() {
  pthread_mutex_destroy(&mutex);
}

void scribeConn::addRef() {
  ++refCount;
}

void scribeConn::releaseRef() {
  --refCount;
}

unsigned scribeConn::getRef() {
  return refCount;
}

void scribeConn::lock() {
  pthread_mutex_lock(&mutex);
}

void scribeConn::unlock() {
  pthread_mutex_unlock(&mutex);
}

bool scribeConn::isOpen() {
  return framedTransport->isOpen();
}

bool scribeConn::open() {
  try {

#ifdef USE_ZOOKEEPER
    if (0 == zkRegistrationZnode.find("zk://")) {
      string parentZnode = zkRegistrationZnode.substr(5, string::npos);
      if (g_ZKClient->getRemoteScribe(parentZnode, remoteHost, remotePort)) {
        LOG_OPER("Got remote scribe from zookeeper <%s:%lu>", remoteHost.c_str(), remotePort);
      } else {
        LOG_OPER("Unable to get a remote Scribe from %s", zkRegistrationZnode.c_str());
      }
    }
#endif

    socket = smcBased ?
      shared_ptr<TSocket>(new TSocketPool(serverList)) :
      shared_ptr<TSocket>(new TSocket(remoteHost, remotePort));

    if (!socket) {
      throw std::runtime_error("Failed to create socket");
    }

    socket->setConnTimeout(timeout);
    socket->setRecvTimeout(timeout);
    socket->setSendTimeout(timeout);

    framedTransport = shared_ptr<TFramedTransport>(new TFramedTransport(socket));
    if (!framedTransport) {
      throw std::runtime_error("Failed to create framed transport");
    }
    protocol = shared_ptr<TBinaryProtocol>(new TBinaryProtocol(framedTransport));
    if (!protocol) {
      throw std::runtime_error("Failed to create protocol");
    }
    protocol->setStrict(false, false);
    resendClient = shared_ptr<scribeClient>(new scribeClient(protocol));
    if (!resendClient) {
      throw std::runtime_error("Failed to create network client");
    }

    framedTransport->open();
    if (smcBased) {
      remoteHost = socket->getPeerHost();
    }
  } catch (TTransportException& ttx) {
    LOG_OPER("failed to open connection to remote scribe server %s thrift error <%s>",
             connectionString().c_str(), ttx.what());
    return false;
  } catch (std::exception& stx) {
    LOG_OPER("failed to open connection to remote scribe server %s std error <%s>",
             connectionString().c_str(), stx.what());
    return false;
  }
  LOG_OPER("Opened connection to remote scribe server %s",
           connectionString().c_str());
  return true;
}

void scribeConn::close() {
  try {
    framedTransport->close();
  } catch (TTransportException& ttx) {
    LOG_OPER("error <%s> while closing connection to remote scribe server %s",
             ttx.what(), connectionString().c_str());
  }
}

bool scribeConn::send(boost::shared_ptr<logentry_vector_t> messages) {
  int size = messages->size();
  if (size <= 0) {
    return true;
  }
  if (!isOpen()) {
    if (!open()) {
      return false;
    }
  }

  // Copy the vector of pointers to a vector of objects
  // This is because thrift doesn't support vectors of pointers,
  // but we need to use them internally to avoid even more copies.
  std::vector<LogEntry> msgs;
  msgs.reserve(size);
  for (logentry_vector_t::iterator iter = messages->begin();
       iter != messages->end();
       ++iter) {
    msgs.push_back(**iter);
  }
  ResultCode result = TRY_LATER;
  try {
    result = resendClient->Log(msgs);

    if (result == OK) {
      g_Handler->incCounter("sent", size);
      LOG_OPER("Successfully sent <%d> messages to remote scribe server %s",
          size, connectionString().c_str());
      return true;
    } else {
      LOG_OPER("Failed to send <%d> messages, remote scribe server %s returned error code <%d>",
          size, connectionString().c_str(), (int) result);
    }
  } catch (TTransportException& ttx) {
    LOG_OPER("Failed to send <%d> messages to remote scribe server %s error <%s>",
        size, connectionString().c_str(),
        ttx.what());
  } catch (...) {
    LOG_OPER("Unknown exception sending <%d> messages to remote scribe server %s",
        size, connectionString().c_str());
  }
   // we only get here if sending failed
  close();
  return false;
}

std::string scribeConn::connectionString() {
	if (smcBased) {
		return "<" + remoteHost + " SMC service: " + smcService + ">";
	} else {
		char port[10];
		snprintf(port, 10, "%lu", remotePort);
		return "<" + remoteHost + ":" + string(port) + ">";
	}
}
