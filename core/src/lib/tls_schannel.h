/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
#ifndef BAREOS_LIB_TLS_SCHANNEL_H_
#define BAREOS_LIB_TLS_SCHANNEL_H_

#include "lib/tls.h"
#include <sspi.h>
#include <memory>
#include <cstring>

class TlsSchannel : public Tls {
 public:
  TlsSchannel() {}

  ~TlsSchannel() override {}

  bool init() override
  {
    // do this immediately after connection!
    if (SECURITY_STATUS scRet = QueryContextAttributes(
            SecurityContext, SECPKG_ATTR_STREAM_SIZES, &Sizes);
        FAILED(scRet)) {
      // MyHandleError("Error reading SECPKG_ATTR_STREAM_SIZES");
      return false;
    }

    buffer = std::make_unique<char[]>(Sizes.cbHeader + Sizes.cbMaximumMessage
                                      + Sizes.cbTrailer);

    header = buffer.get();
    data = header + Sizes.cbHeader;
    trailer = data + Sizes.cbMaximumMessage;

    return false;
  }

  void SetTlsPskClientContext(const PskCredentials&) override
  {
    // psk not supported for now
  }

  void SetTlsPskServerContext(ConfigurationParser*) override
  {
    // psk not supported for now
  }

  bool TlsPostconnectVerifyHost(JobControlRecord* jcr,
                                const char* host) override
  {
    return false;
  }
  bool TlsPostconnectVerifyCn(
      JobControlRecord* jcr,
      const std::vector<std::string>& verify_list) override
  {
    return false;
  }

  bool TlsBsockAccept(BareosSocket* bsock) override { return false; }
  int TlsBsockWriten(BareosSocket* bsock, char* ptr, int32_t nbytes) override
  {
    SecBuffer Buffers[4] = {};
    SecBufferDesc Message = {};
    Buffers[0].pvBuffer = header;
    Buffers[0].BufferType = SECBUFFER_STREAM_HEADER;

    Buffers[1].pvBuffer = data;
    Buffers[1].BufferType = SECBUFFER_DATA;

    Buffers[2].pvBuffer = trailer;
    Buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;

    Buffers[3].BufferType = SECBUFFER_EMPTY;

    Message.ulVersion = SECBUFFER_VERSION;
    Message.cBuffers = 4;
    Message.pBuffers = Buffers;

    int32_t send = 0;
    while (send < nbytes) {
      int32_t packetsize = std::min(nbytes - send, Sizes.cbMaximumMessage);
      std::memcpy(data, ptr + send, packetsize);
      Buffers[0].cbBuffer = Sizes.cbHeader;
      Buffers[1].cbBuffer = packetsize;
      Buffers[2].cbBuffer = Sizes.cbTrailer;
      Message.ulVersion = SECBUFFER_VERSION;
      Message.cBuffers = 4;
      Message.pBuffers = Buffers;
      EncryptMessage(SecurityContext, 0, &Message, 0);

      if (!SendBytes(bsock->underlying(), header,
                     Buffers[0].cbBuffer + Buffers[1].cbBuffer
                         + Buffers[2].cbBuffer)) {
        return 0;
      }

      send += packetsize;
    }

    return send;
  }
  int TlsBsockReadn(BareosSocket* bsock, char* ptr, int32_t nbytes) override
  {
    SecBuffer Buffers[4] = {};
    SecBufferDesc Message = {};
    Buffers[0].pvBuffer = data.get();
    Buffers[0].cbBuffer = Sizes.cbHeader + Sizes.cbMaximumMessage
                          + Sizes.cbTrailer Buffers[0].BufferType
        = SECBUFFER_DATA;

    Buffers[1].BufferType = SECBUFFER_EMPTY;
    Buffers[2].BufferType = SECBUFFER_EMPTY;
    Buffers[3].BufferType = SECBUFFER_EMPTY;

    Message.ulVersion = SECBUFFER_VERSION;
    Message.cBuffers = 4;
    Message.pBuffers = Buffers;

    bool completed = false;
    while (!completed) {
      ReadBytes();
      SECURITY_STATUS scRet
          = DecryptMessage(SecurityContext, &Message, 0, NULL);
      switch (scRet) {
        case SEC_E_OK: {
          completed = true;
        } break;
        case SEC_E_INCOMPLETE_MESSAGE: {
          WaitForReadableFd(bsoch->fd_, 10000, false);
        } break;
        default: {
          return -1;
        }
      }
    }

    int32_t free = nbytes;
    char* current = ptr;

    for (int i = 0; i < 4; ++i) {
      if (Buffers[i].BufferType == SECBUFFER_DATA) {
        ASSERT(free >= Buffers[i].cbBuffers);
        memcpy(current, Buffers[i].pvBuffer, Buffers[i].cbBuffers);
        current += Buffers[i].cbBuffers;
        free -= Buffers[i].cbBuffers;
      }
    }

    return 0;
  }
  bool TlsBsockConnect(BareosSocket* bsock) override { return false; }
  void TlsBsockShutdown(BareosSocket* bsock) override {}
  void TlsLogConninfo(JobControlRecord* jcr,
                      const char* host,
                      int port,
                      const char* who) const override
  {
    if (!hctxt) {
      Qmsg(jcr, M_INFO, 0, T_("No schannel to %s at %s:%d established\n"), who,
           host, port);
    } else {
      std::string cipher_name = TlsCipherGetName();
      Qmsg(jcr, M_INFO, 0, T_("Connected %s at %s:%d, encryption: %s\n"), who,
           host, port, cipher_name.empty() ? "Unknown" : cipher_name.c_str());
    }
  }

  std::string TlsCipherGetName() const override
  {
    SecPkgContext_CipherInfo info;
    QueryContextAttributes(&SecurityContext, SECPKG_ATTR_CIPHER_INFO, &info);
    return wchar_2_utf8(info.szCipher) + std::to_string(info.dwVersion);
  }

  void SetCipherList(const std::string& cipherlist) override
  {
    cipher_list = cipherlist;
  }
  void SetCipherSuites(const std::string& ciphersuites) override
  {
    cipher_suite = ciphersuites;
  }
  void SetProtocol(const std::string& version) override { protocol = version; }

  bool KtlsSendStatus() override
  {
    // ktls not supported for now
    return false;
  }
  bool KtlsRecvStatus() override
  {
    // ktls not supported for now
    return false;
  }

  void SetCaCertfile(const std::string& ca_certfile) override {}
  void SetCaCertdir(const std::string& ca_certdir) override {}
  void SetCrlfile(const std::string& crlfile_) override {}
  void SetCertfile(const std::string& certfile_) override {}
  void SetKeyfile(const std::string& keyfile_) override {}
  void SetPemCallback(CRYPTO_PEM_PASSWD_CB pem_callback) override {}
  void SetPemUserdata(void* pem_userdata) override {}
  void SetDhFile(const std::string& dhfile_) override {}
  void SetVerifyPeer(const bool& verify_peer) override {}
  void SetEnableKtls(bool ktls) override {}
  void SetTcpFileDescriptor(const int& fd) override {}

 private:
  SecHandle SecurityContext;
  SecPkgContext_StreamSizes Sizes;
  std::unique_ptr<char[]> buffer;
  char *header, *data, *trailer;

  std::string protocol;
  std::string cipher_list;
  std::string cipher_suite;
};

#endif  // BAREOS_LIB_TLS_SCHANNEL_H_
