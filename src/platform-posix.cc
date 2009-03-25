// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Platform specific code for POSIX goes here. This is not a platform on its
// own but contains the parts which are the same across POSIX platforms Linux,
// Mac OS and FreeBSD.

#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include "v8.h"

#include "platform.h"


namespace v8 { namespace internal {


// ----------------------------------------------------------------------------
// POSIX socket support.
//

class POSIXSocket : public Socket {
 public:
  explicit POSIXSocket() {
    // Create the socket.
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  }
  explicit POSIXSocket(int socket): socket_(socket) { }
  virtual ~POSIXSocket() { Shutdown(); }

  // Server initialization.
  bool Bind(const int port);
  bool Listen(int backlog) const;
  Socket* Accept() const;

  // Client initialization.
  bool Connect(const char* host, const char* port);

  // Shutdown socket for both read and write.
  bool Shutdown();

  // Data Transimission
  int Send(const char* data, int len) const;
  int Receive(char* data, int len) const;

  bool SetReuseAddress(bool reuse_address);

  bool IsValid() const { return socket_ != -1; }

 private:
  int socket_;
};


bool POSIXSocket::Bind(const int port) {
  if (!IsValid())  {
    return false;
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  int status = bind(socket_,
                    reinterpret_cast<struct sockaddr *>(&addr),
                    sizeof(addr));
  return status == 0;
}


bool POSIXSocket::Listen(int backlog) const {
  if (!IsValid()) {
    return false;
  }

  int status = listen(socket_, backlog);
  return status == 0;
}


Socket* POSIXSocket::Accept() const {
  if (!IsValid()) {
    return NULL;
  }

  int socket = accept(socket_, NULL, NULL);
  if (socket == -1) {
    return NULL;
  } else {
    return new POSIXSocket(socket);
  }
}


bool POSIXSocket::Connect(const char* host, const char* port) {
  if (!IsValid()) {
    return false;
  }

  // Lookup host and port.
  struct addrinfo *result = NULL;
  struct addrinfo hints;
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  int status = getaddrinfo(host, port, &hints, &result);
  if (status != 0) {
    return false;
  }

  // Connect.
  status = connect(socket_, result->ai_addr, result->ai_addrlen);
  freeaddrinfo(result);
  return status == 0;
}


bool POSIXSocket::Shutdown() {
  if (IsValid()) {
    // Shutdown socket for both read and write.
    int status = shutdown(socket_, SHUT_RDWR);
    close(socket_);
    socket_ = -1;
    return status == 0;
  }
  return true;
}


int POSIXSocket::Send(const char* data, int len) const {
  int status = send(socket_, data, len, 0);
  return status;
}


int POSIXSocket::Receive(char* data, int len) const {
  int status = recv(socket_, data, len, 0);
  return status;
}


bool POSIXSocket::SetReuseAddress(bool reuse_address) {
  int on = reuse_address ? 1 : 0;
  int status = setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  return status == 0;
}


bool Socket::Setup() {
  // Nothing to do on POSIX.
  return true;
}


int Socket::LastError() {
  return errno;
}


uint16_t Socket::HToN(uint16_t value) {
  return htons(value);
}


uint16_t Socket::NToH(uint16_t value) {
  return ntohs(value);
}


uint32_t Socket::HToN(uint32_t value) {
  return htonl(value);
}


uint32_t Socket::NToH(uint32_t value) {
  return ntohl(value);
}


Socket* OS::CreateSocket() {
  return new POSIXSocket();
}


} }  // namespace v8::internal
