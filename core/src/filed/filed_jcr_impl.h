/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2023 Bareos GmbH & Co. KG

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

#ifndef BAREOS_FILED_FILED_JCR_IMPL_H_
#define BAREOS_FILED_FILED_JCR_IMPL_H_

#include "include/bareos.h"
#include "lib/crypto.h"

#include <atomic>
#include <variant>
#include <thread>
#include <cstring>
#include "lib/channel.h"
#include "lib/bsock.h"

struct AclData;
struct XattrData;

namespace filedaemon {
class BareosAccurateFilelist;
}

class send_context {
  using signal_type = int32_t;
  struct data_type {
    POOLMEM* msg_start{nullptr};
    std::size_t length{0};

    data_type() = default;
    data_type(POOLMEM* msg_start, std::size_t length)
        : msg_start{msg_start}, length{length}
    {
    }
    data_type(const data_type&) = delete;
    data_type(data_type&& other)
    {
      std::swap(msg_start, other.msg_start);
      std::swap(length, other.length);
    }

    ~data_type()
    {
      if (msg_start) { FreeMemory(msg_start); }
    }
  };
  using packet = std::variant<signal_type, data_type>;

  BareosSocket* sd;
  channel::input<char> in;
  channel::output<char> out;

  std::thread sender;

  send_context(std::pair<channel::input<char>, channel::output<char>> cpair,
               BareosSocket* sd)
      : sd{sd}
      , in{std::move(cpair.first)}
      , out{std::move(cpair.second)}
      , sender{send_work, this}
  {
  }

  static void send_work(send_context* ctx) { ctx->do_send_work(); }

  void do_send_work()
  {
    std::vector<char> data;
    for (;;) {
      if (!out.get_all(data)) { break; }

      if (data.size() > 0) {
        data_send += data.size();
        num_sends += 1;
        sd->write_nbytes(data.data(), data.size());
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

 public:
  bool send(POOLMEM* data, std::size_t size)
  {
    auto ret = send((const char*)data, size);
    FreePoolMemory(data);
    return ret;
  }

  bool send(const char* data, std::size_t size)
  {
    requested += 1;
    send_bytes += size;
    char sdata[sizeof(uint32_t)];
    int32_t s = htonl((int32_t)size);
    std::memcpy(sdata, &s, sizeof(sdata));

    if (!in.insert(std::begin(sdata), std::end(sdata))) { return false; }

    return in.insert(data, data + size);
  }

  bool format(const char* fmt, ...)
  {
    POOLMEM* msg = GetPoolMemory(PM_MESSAGE);
    int len, maxlen;
    va_list ap;

    while (1) {
      maxlen = SizeofPoolMemory(msg) - 1;
      va_start(ap, fmt);
      len = Bvsnprintf(msg, maxlen, fmt, ap);
      va_end(ap);

      if (len < 0 || len >= (maxlen - 5)) {
        msg = ReallocPoolMemory(msg, maxlen + maxlen / 2);
        continue;
      }

      break;
    }

    return send(msg, len);
  }

  bool signal(int32_t signal)
  {
    requested += 1;
    send_bytes += 4;
    char sdata[sizeof(int32_t)];
    int32_t s = htonl((int32_t)signal);
    std::memcpy(sdata, &s, sizeof(sdata));

    return in.insert((char*)sdata, ((char*)sdata) + 4);
  }

  std::size_t num_bytes_send() const { return send_bytes; }

  const char* error() { return sd->bstrerror(); }

  send_context(BareosSocket* sd)
      : send_context(channel::CreateBufferedChannel<char>(1024 * 1024 * 128),
                     sd)
  {
  }

  ~send_context()
  {
    in.close();
    sender.join();

    double avg_send = (double)data_send / (double)num_sends;
    double avg_req = (double)send_bytes / (double)requested;
    Dmsg4(50, "num_sends: %llu, requested: %llu, avg_send: %lf, avg_req: %lf\n",
          num_sends, requested, avg_send, avg_req);
  }

 private:
  std::size_t send_bytes{};
  std::size_t requested{};
  std::size_t num_sends{};
  std::size_t data_send{};
};

/* clang-format off */
struct CryptoContext {
  bool pki_sign{};                /**< Enable PKI Signatures? */
  bool pki_encrypt{};             /**< Enable PKI Encryption? */
  DIGEST* digest{};               /**< Last file's digest context */
  X509_KEYPAIR* pki_keypair{};    /**< Encryption key pair */
  alist<X509_KEYPAIR*>* pki_signers{};           /**< Trusted Signers */
  alist<X509_KEYPAIR*>* pki_recipients{};        /**< Trusted Recipients */
  CRYPTO_SESSION* pki_session{};  /**< PKE Public Keys + Symmetric Session Keys */
  POOLMEM* crypto_buf{};          /**< Encryption/Decryption buffer */
  POOLMEM* pki_session_encoded{}; /**< Cached DER-encoded copy of pki_session */
  int32_t pki_session_encoded_size{}; /**< Size of DER-encoded pki_session */
};

struct FiledJcrImpl {
  uint32_t num_files_examined{};  /**< Files examined this job */
  POOLMEM* last_fname{};          /**< Last file saved/verified */
  POOLMEM* job_metadata{};        /**< VSS job metadata */
  std::unique_ptr<AclData> acl_data{};         /**< ACLs for backup/restore */
  std::unique_ptr<XattrData> xattr_data{};     /**< Extended Attributes for backup/restore */
  int32_t last_type{};            /**< Type of last file saved/verified */
  bool incremental{};             /**< Set if incremental for SINCE */
  utime_t since_time{};           /**< Begin time for SINCE */
  int listing{};                  /**< Job listing in estimate */
  int32_t Ticket{};               /**< Ticket */
  char* big_buf{};                /**< I/O buffer */
  int32_t replace{};              /**< Replace options */
  FindFilesPacket* ff{};          /**< Find Files packet */
  char PrevJob[MAX_NAME_LENGTH]{};/**< Previous job name assiciated with since time */
  uint32_t ExpectedFiles{};       /**< Expected restore files */
  uint32_t StartFile{};
  uint32_t EndFile{};
  uint32_t StartBlock{};
  uint32_t EndBlock{};
  pthread_t heartbeat_id{};       /**< Id of heartbeat thread */
  std::atomic<bool> hb_initialized_once{};    /**< Heartbeat initialized */
  std::atomic<bool> hb_running{};             /**< Heartbeat running */
  std::shared_ptr<BareosSocket> hb_bsock;     /**< Duped SD socket */
  std::shared_ptr<BareosSocket> hb_dir_bsock; /**< Duped DIR socket */
  alist<RunScript*>* RunScripts{};            /**< Commands to run before and after job */
  CryptoContext crypto;           /**< Crypto ctx */
  filedaemon::DirectorResource* director{}; /**< Director resource */
  bool enable_vss{};              /**< VSS used by FD */
  bool got_metadata{};            /**< Set when found job_metadata */
  bool multi_restore{};           /**< Dir can do multiple storage restore */
  filedaemon::BareosAccurateFilelist* file_list{}; /**< Previous file list (accurate mode) */
  uint64_t base_size{};           /**< Compute space saved with base job */
  filedaemon::save_pkt* plugin_sp{}; /**< Plugin save packet */
#ifdef HAVE_WIN32
  VSSClient* pVSSClient{};        /**< VSS Client Instance */
#endif

  std::optional<send_context> send_ctx;
  void* submit_ctx{nullptr};
};
/* clang-format on */

#endif  // BAREOS_FILED_FILED_JCR_IMPL_H_
