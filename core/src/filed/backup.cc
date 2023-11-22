/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
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
// Kern Sibbald, March MM
/**
 * @file
 * Bareos File Daemon  backup.c  send file attributes and data to the Storage
 * daemon.
 */
#include "include/fcntl_def.h"
#include "include/bareos.h"
#include "include/filetypes.h"
#include "include/streams.h"
#include "filed/filed.h"
#include "filed/filed_globals.h"
#include "filed/accurate.h"
#include "filed/compression.h"
#include "filed/crypto.h"
#include "filed/heartbeat.h"
#include "filed/backup.h"
#include "filed/filed_jcr_impl.h"
#include "include/ch.h"
#include "findlib/attribs.h"
#include "findlib/hardlink.h"
#include "findlib/find_one.h"
#include "lib/attribs.h"
#include "lib/berrno.h"
#include "lib/bsock.h"
#include "lib/btimers.h"
#include "lib/parse_conf.h"
#include "lib/util.h"
#include "lib/serial.h"
#include "lib/compression.h"

namespace filedaemon {

#ifdef HAVE_DARWIN_OS
const bool have_darwin_os = true;
#else
const bool have_darwin_os = false;
#endif

#if defined(HAVE_ACL)
const bool have_acl = true;
#else
const bool have_acl = false;
#endif

#if defined(HAVE_XATTR)
const bool have_xattr = true;
#else
const bool have_xattr = false;
#endif

#ifndef compressBound
#  define compressBound(sourceLen) \
    (sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) + 13)
#endif

/* Forward referenced functions */

#if 0
static int send_data(JobControlRecord* jcr,
                     int stream,
                     FindFilesPacket* ff_pkt,
                     DIGEST* digest,
                     DIGEST* signature_digest);
#endif
bool EncodeAndSendAttributes(JobControlRecord* jcr,
                             FindFilesPacket* ff_pkt,
                             int& data_stream);
#if defined(WIN32_VSS)
static void CloseVssBackupSession(JobControlRecord* jcr);
#endif

#if 0

// Save OSX specific resource forks and finder info.
static inline bool SaveRsrcAndFinder(b_save_ctx& bsctx,
				     BareosSocket* sd)
{
  char flags[FOPTS_BYTES];
  int rsrc_stream;
  bool retval = false;

  if (bsctx.ff_pkt->hfsinfo.rsrclength > 0) {
    if (BopenRsrc(&bsctx.ff_pkt->bfd, bsctx.ff_pkt->fname, O_RDONLY | O_BINARY,
                  0)
        < 0) {
      bsctx.ff_pkt->ff_errno = errno;
      BErrNo be;
      Jmsg(bsctx.jcr, M_NOTSAVED, -1,
           _("     Cannot open resource fork for \"%s\": ERR=%s.\n"),
           bsctx.ff_pkt->fname, be.bstrerror());
      bsctx.jcr->JobErrors++;
      if (IsBopen(&bsctx.ff_pkt->bfd)) { bclose(&bsctx.ff_pkt->bfd); }
    } else {
      int status;

      memcpy(flags, bsctx.ff_pkt->flags, sizeof(flags));
      ClearBit(FO_COMPRESS, bsctx.ff_pkt->flags);
      ClearBit(FO_SPARSE, bsctx.ff_pkt->flags);
      ClearBit(FO_OFFSETS, bsctx.ff_pkt->flags);
      rsrc_stream = BitIsSet(FO_ENCRYPT, flags)
                        ? STREAM_ENCRYPTED_MACOS_FORK_DATA
                        : STREAM_MACOS_FORK_DATA;

      status = send_data(bsctx.jcr, rsrc_stream, bsctx.ff_pkt, bsctx.digest,
                         bsctx.signing_digest);

      memcpy(bsctx.ff_pkt->flags, flags, sizeof(flags));
      bclose(&bsctx.ff_pkt->bfd);
      if (!status) { goto bail_out; }
    }
  }

  Dmsg1(300, "Saving Finder Info for \"%s\"\n", bsctx.ff_pkt->fname);
  sd->fsend("%ld %d 0", bsctx.jcr->JobFiles, STREAM_HFSPLUS_ATTRIBUTES);
  Dmsg1(300, "filed>stored:header %s", sd->msg);
  PmMemcpy(sd->msg, bsctx.ff_pkt->hfsinfo.fndrinfo, 32);
  sd->message_length = 32;
  if (bsctx.digest) {
    CryptoDigestUpdate(bsctx.digest, (uint8_t*)sd->msg, sd->message_length);
  }
  if (bsctx.signing_digest) {
    CryptoDigestUpdate(bsctx.signing_digest, (uint8_t*)sd->msg,
                       sd->message_length);
  }
  sd->send();
  sd->signal(BNET_EOD);

  retval = true;

bail_out:
  return retval;
}
#endif

/**
 * Setup for digest handling. If this fails, the digest will be set to NULL
 * and not used. Note, the digest (file hash) can be any one of the four
 * algorithms below.
 *
 * The signing digest is a single algorithm depending on
 * whether or not we have SHA2.
 *   ****FIXME****  the signing algorithm should really be
 *   determined a different way!!!!!!  What happens if
 *   sha2 was available during backup but not restore?
 */
static inline bool SetupEncryptionDigests(b_save_ctx& bsctx)
{
  bool retval = false;
  // TODO landonf: Allow the user to specify the digest algorithm
#ifdef HAVE_SHA2
  crypto_digest_t signing_algorithm = CRYPTO_DIGEST_SHA256;
#else
  crypto_digest_t signing_algorithm = CRYPTO_DIGEST_SHA1;
#endif

  if (BitIsSet(FO_MD5, bsctx.ff_pkt->flags)) {
    bsctx.digest = crypto_digest_new(bsctx.jcr, CRYPTO_DIGEST_MD5);
    bsctx.digest_stream = STREAM_MD5_DIGEST;
  } else if (BitIsSet(FO_SHA1, bsctx.ff_pkt->flags)) {
    bsctx.digest = crypto_digest_new(bsctx.jcr, CRYPTO_DIGEST_SHA1);
    bsctx.digest_stream = STREAM_SHA1_DIGEST;
  } else if (BitIsSet(FO_SHA256, bsctx.ff_pkt->flags)) {
    bsctx.digest = crypto_digest_new(bsctx.jcr, CRYPTO_DIGEST_SHA256);
    bsctx.digest_stream = STREAM_SHA256_DIGEST;
  } else if (BitIsSet(FO_SHA512, bsctx.ff_pkt->flags)) {
    bsctx.digest = crypto_digest_new(bsctx.jcr, CRYPTO_DIGEST_SHA512);
    bsctx.digest_stream = STREAM_SHA512_DIGEST;
  } else if (BitIsSet(FO_XXH128, bsctx.ff_pkt->flags)) {
    bsctx.digest = crypto_digest_new(bsctx.jcr, CRYPTO_DIGEST_XXH128);
    bsctx.digest_stream = STREAM_XXH128_DIGEST;
  }

  // Did digest initialization fail?
  if (bsctx.digest_stream != STREAM_NONE && bsctx.digest == NULL) {
    Jmsg(bsctx.jcr, M_WARNING, 0, _("%s digest initialization failed\n"),
         stream_to_ascii(bsctx.digest_stream));
  }

  /* Set up signature digest handling. If this fails, the signature digest
   * will be set to NULL and not used. */
  /* TODO landonf: We should really only calculate the digest once, for
   * both verification and signing.
   */
  if (bsctx.jcr->fd_impl->crypto.pki_sign) {
    bsctx.signing_digest = crypto_digest_new(bsctx.jcr, signing_algorithm);

    // Full-stop if a failure occurred initializing the signature digest
    if (bsctx.signing_digest == NULL) {
      Jmsg(bsctx.jcr, M_NOTSAVED, 0,
           _("%s signature digest initialization failed\n"),
           stream_to_ascii(signing_algorithm));
      bsctx.jcr->JobErrors++;
      goto bail_out;
    }
  }

  // Enable encryption
  if (bsctx.jcr->fd_impl->crypto.pki_encrypt) {
    SetBit(FO_ENCRYPT, bsctx.ff_pkt->flags);
  }
  retval = true;

bail_out:
  return retval;
}

#if 0
// Terminate the signing digest and send it to the Storage daemon
static inline bool TerminateSigningDigest(b_save_ctx& bsctx)
{
  uint32_t size = 0;
  bool retval = false;
  SIGNATURE* signature = NULL;
  BareosSocket* sd = bsctx.jcr->store_bsock;

  if ((signature = crypto_sign_new(bsctx.jcr)) == NULL) {
    Jmsg(bsctx.jcr, M_FATAL, 0,
         _("Failed to allocate memory for crypto signature.\n"));
    goto bail_out;
  }

  if (!CryptoSignAddSigner(signature, bsctx.signing_digest,
                           bsctx.jcr->fd_impl->crypto.pki_keypair)) {
    Jmsg(bsctx.jcr, M_FATAL, 0,
         _("An error occurred while signing the stream.\n"));
    goto bail_out;
  }

  // Get signature size
  if (!CryptoSignEncode(signature, NULL, &size)) {
    Jmsg(bsctx.jcr, M_FATAL, 0,
         _("An error occurred while signing the stream.\n"));
    goto bail_out;
  }

  // Grow the bsock buffer to fit our message if necessary
  if (SizeofPoolMemory(sd->msg) < (int32_t)size) {
    sd->msg = ReallocPoolMemory(sd->msg, size);
  }

  // Send our header
  sd->fsend("%ld %ld 0", bsctx.jcr->JobFiles, STREAM_SIGNED_DIGEST);
  Dmsg1(300, "filed>stored:header %s", sd->msg);

  // Encode signature data
  if (!CryptoSignEncode(signature, (uint8_t*)sd->msg, &size)) {
    Jmsg(bsctx.jcr, M_FATAL, 0,
         _("An error occurred while signing the stream.\n"));
    goto bail_out;
  }

  sd->message_length = size;
  sd->send();
  sd->signal(BNET_EOD); /* end of checksum */
  retval = true;

bail_out:
  if (signature) { CryptoSignFree(signature); }
  return retval;
}

// Terminate any digest and send it to Storage daemon
static inline bool TerminateDigest(b_save_ctx& bsctx)
{
  uint32_t size;
  bool retval = false;
  BareosSocket* sd = bsctx.jcr->store_bsock;

  sd->fsend("%ld %d 0", bsctx.jcr->JobFiles, bsctx.digest_stream);
  Dmsg1(300, "filed>stored:header %s", sd->msg);

  size = CRYPTO_DIGEST_MAX_SIZE;

  // Grow the bsock buffer to fit our message if necessary
  if (SizeofPoolMemory(sd->msg) < (int32_t)size) {
    sd->msg = ReallocPoolMemory(sd->msg, size);
  }

  if (!CryptoDigestFinalize(bsctx.digest, (uint8_t*)sd->msg, &size)) {
    Jmsg(bsctx.jcr, M_FATAL, 0,
         _("An error occurred finalizing signing the stream.\n"));
    goto bail_out;
  }

  // Keep the checksum if this file is a hardlink
  if (bsctx.ff_pkt->linked) {
    bsctx.ff_pkt->linked->set_digest(bsctx.digest_stream, sd->msg, size);
  }

  sd->message_length = size;
  sd->send();
  sd->signal(BNET_EOD); /* end of checksum */
  retval = true;

bail_out:
  return retval;
}

#endif

enum class file_type
{
  LNKSAVED = 1,   /**< hard link to file already saved */
  REGE = 2,       /**< Regular file but empty */
  REG = 3,        /**< Regular file */
  LNK = 4,        /**< Soft Link */
  DIREND = 5,     /**< Directory at end (saved) */
  SPEC = 6,       /**< Special file -- chr, blk, fifo, sock */
  NOACCESS = 7,   /**< Not able to access */
  NOFOLLOW = 8,   /**< Could not follow link */
  NOSTAT = 9,     /**< Could not stat file */
  NOCHG = 10,     /**< Incremental option, file not changed */
  DIRNOCHG = 11,  /**< Incremental option, directory not changed */
  ISARCH = 12,    /**< Trying to save archive file */
  NORECURSE = 13, /**< No recursion into directory */
  NOFSCHG = 14,   /**< Different file system, prohibited */
  NOOPEN = 15,    /**< Could not open directory */
  RAW = 16,       /**< Raw block device */
  FIFO = 17,      /**< Raw fifo device */
  /* The DIRBEGIN packet is sent to the FD file processing routine so
   * that it can filter packets, but otherwise, it is not used
   * or saved */
  DIRBEGIN = 18,      /**< Directory at beginning (not saved) */
  INVALIDFS = 19,     /**< File system not allowed for */
  INVALIDDT = 20,     /**< Drive type not allowed for */
  REPARSE = 21,       /**< Win NTFS reparse point */
  PLUGIN = 22,        /**< Plugin generated filename */
  DELETED = 23,       /**< Deleted file entry */
  BASE = 24,          /**< Duplicate base file entry */
  RESTORE_FIRST = 25, /**< Restore this "object" first */
  JUNCTION = 26,      /**< Win32 Junction point */
  PLUGIN_CONFIG = 27, /**< Object for Plugin configuration */
  PLUGIN_CONFIG_FILLED
  = 28, /**< Object for Plugin configuration filled by Director */
};

enum class data_stream : int
{
  NONE = 0,                 /**< Reserved Non-Stream */
  UNIX_ATTRIBUTES = 1,      /**< Generic Unix attributes */
  FILE_DATA = 2,            /**< Standard uncompressed data */
  MD5_SIGNATURE = 3,        /**< MD5 signature - Deprecated */
  MD5_DIGEST = 3,           /**< MD5 digest for the file */
  GZIP_DATA = 4,            /**< GZip compressed file data - Deprecated */
  UNIX_ATTRIBUTES_EX = 5,   /**< Extended Unix attr for Win32 EX - Deprecated */
  SPARSE_DATA = 6,          /**< Sparse data stream */
  SPARSE_GZIP_DATA = 7,     /**< Sparse gzipped data stream - Deprecated */
  PROGRAM_NAMES = 8,        /**< Program names for program data */
  PROGRAM_DATA = 9,         /**< Data needing program */
  SHA1_SIGNATURE = 10,      /**< SHA1 signature - Deprecated */
  SHA1_DIGEST = 10,         /**< SHA1 digest for the file */
  WIN32_DATA = 11,          /**< Win32 BackupRead data */
  WIN32_GZIP_DATA = 12,     /**< Gzipped Win32 BackupRead data - Deprecated */
  MACOS_FORK_DATA = 13,     /**< Mac resource fork */
  HFSPLUS_ATTRIBUTES = 14,  /**< Mac OS extra attributes */
  UNIX_ACCESS_ACL = 15,     /**< Standard ACL attributes on UNIX - Deprecated */
  UNIX_DEFAULT_ACL = 16,    /**< Default ACL attributes on UNIX - Deprecated */
  SHA256_DIGEST = 17,       /**< SHA-256 digest for the file */
  SHA512_DIGEST = 18,       /**< SHA-512 digest for the file */
  SIGNED_DIGEST = 19,       /**< Signed File Digest, ASN.1, DER Encoded */
  ENCRYPTED_FILE_DATA = 20, /**< Encrypted, uncompressed data */
  ENCRYPTED_WIN32_DATA
  = 21, /**< Encrypted, uncompressed Win32 BackupRead data */
  ENCRYPTED_SESSION_DATA
  = 22, /**< Encrypted, Session Data, ASN.1, DER Encoded */
  ENCRYPTED_FILE_GZIP_DATA = 23, /**< Encrypted, compressed data - Deprecated */
  ENCRYPTED_WIN32_GZIP_DATA
  = 24, /**< Encrypted, compressed Win32 BackupRead data - Deprecated */
  ENCRYPTED_MACOS_FORK_DATA
  = 25,                /**< Encrypted, uncompressed Mac resource fork */
  PLUGIN_NAME = 26,    /**< Plugin "file" string */
  PLUGIN_DATA = 27,    /**< Plugin specific data */
  RESTORE_OBJECT = 28, /**< Plugin restore object */
  /* Compressed streams. These streams can handle arbitrary compression
   * algorithm data as an additional header is stored at the beginning of the
   * stream. See stream_compressed_header definition for more details. */
  COMPRESSED_DATA = 29,                /**< Compressed file data */
  SPARSE_COMPRESSED_DATA = 30,         /**< Sparse compressed data stream */
  WIN32_COMPRESSED_DATA = 31,          /**< Compressed Win32 BackupRead data */
  ENCRYPTED_FILE_COMPRESSED_DATA = 32, /**< Encrypted, compressed data */
  ENCRYPTED_WIN32_COMPRESSED_DATA
  = 33, /**< Encrypted, compressed Win32 BackupRead data */

  XXH128_DIGEST = 40, /**< xxHash128 digest for the file */

  NDMP_SEPARATOR
  = 999, /**< NDMP separator between multiple data streams of one job */

  /* The Stream numbers from 1000-1999 are reserved for ACL and extended
   * attribute streams. Each different platform has its own stream id(s), if a
   * platform supports multiple stream types it should supply different handlers
   * for each type it supports and this should be called from the stream
   * dispatch function. Currently in this reserved space we allocate the
   * different acl streams from 1000 on and the different extended attributes
   * streams from 1999 down. So the two naming spaces grow towards each other.
   */
  ACL_AIX_TEXT = 1000, /**< AIX specific string representation from acl_get */
  ACL_DARWIN_ACCESS_ACL = 1001, /**< Darwin (OSX) specific acl_t string
                                 * representation from acl_to_text (POSIX acl)
                                 */
  ACL_FREEBSD_DEFAULT_ACL
  = 1002, /**< FreeBSD specific acl_t string representation
           * from acl_to_text (POSIX acl) for default acls.
           */
  ACL_FREEBSD_ACCESS_ACL
  = 1003,                    /**< FreeBSD specific acl_t string representation
                              * from acl_to_text (POSIX acl) for access acls.
                              */
  ACL_HPUX_ACL_ENTRY = 1004, /**< HPUX specific acl_entry string representation
                              * from acltostr (POSIX acl)
                              */
  ACL_IRIX_DEFAULT_ACL = 1005, /**< IRIX specific acl_t string representation
                                * from acl_to_text (POSIX acl) for default acls.
                                */
  ACL_IRIX_ACCESS_ACL = 1006,  /**< IRIX specific acl_t string representation
                                * from acl_to_text (POSIX acl) for access acls.
                                */
  ACL_LINUX_DEFAULT_ACL
  = 1007,                      /**< Linux specific acl_t string representation
                                * from acl_to_text (POSIX acl) for default acls.
                                */
  ACL_LINUX_ACCESS_ACL = 1008, /**< Linux specific acl_t string representation
                                * from acl_to_text (POSIX acl) for access acls.
                                */
  ACL_TRU64_DEFAULT_ACL
  = 1009, /**< Tru64 specific acl_t string representation
           * from acl_to_text (POSIX acl) for default acls.
           */
  ACL_TRU64_DEFAULT_DIR_ACL
  = 1010,                      /**< Tru64 specific acl_t string representation
                                * from acl_to_text (POSIX acl) for default acls.
                                */
  ACL_TRU64_ACCESS_ACL = 1011, /**< Tru64 specific acl_t string representation
                                * from acl_to_text (POSIX acl) for access acls.
                                */
  ACL_SOLARIS_ACLENT
  = 1012,                 /**< Solaris specific aclent_t string representation
                           * from acltotext or acl_totext (POSIX acl)
                           */
  ACL_SOLARIS_ACE = 1013, /**< Solaris specific ace_t string representation from
                           * from acl_totext (NFSv4 or ZFS acl)
                           */
  ACL_AFS_TEXT = 1014,    /**< AFS specific string representation from pioctl */
  ACL_AIX_AIXC = 1015,    /**< AIX specific string representation from
                           * aclx_printStr (POSIX acl)
                           */
  ACL_AIX_NFS4 = 1016,    /**< AIX specific string representation from
                           * aclx_printStr (NFSv4 acl)
                           */
  ACL_FREEBSD_NFS4_ACL = 1017, /**< FreeBSD specific acl_t string representation
                                * from acl_to_text (NFSv4 or ZFS acl)
                                */
  ACL_HURD_DEFAULT_ACL
  = 1018,                     /**< GNU HURD specific acl_t string representation
                               * from acl_to_text (POSIX acl) for default acls.
                               */
  ACL_HURD_ACCESS_ACL = 1019, /**< GNU HURD specific acl_t string representation
                               * from acl_to_text (POSIX acl) for access acls.
                               */
  ACL_PLUGIN = 1020,          /**< Plugin specific acl encoding */
  XATTR_PLUGIN = 1988,        /**< Plugin specific extended attributes */
  XATTR_HURD = 1989,          /**< GNU HURD specific extended attributes */
  XATTR_IRIX = 1990,          /**< IRIX specific extended attributes */
  XATTR_TRU64 = 1991,         /**< TRU64 specific extended attributes */
  XATTR_AIX = 1992,           /**< AIX specific extended attributes */
  XATTR_OPENBSD = 1993,       /**< OpenBSD specific extended attributes */
  XATTR_SOLARIS_SYS = 1994,   /**< Solaris specific extensible attributes or
                               * otherwise named extended system attributes.
                               */
  XATTR_SOLARIS = 1995,       /**< Solaris specific extented attributes */
  XATTR_DARWIN = 1996,        /**< Darwin (OSX) specific extended attributes */
  XATTR_FREEBSD = 1997,       /**< FreeBSD specific extended attributes */
  XATTR_LINUX = 1998,         /**< Linux specific extended attributes */
  XATTR_NETBSD = 1999,        /**< NetBSD specific extended attributes */
};

enum class acl_stream : int
{
  AIX_TEXT = (int)data_stream::ACL_AIX_TEXT,
  DARWIN_ACCESS = (int)data_stream::ACL_DARWIN_ACCESS_ACL,
  FREEBSD_DEFAULT = (int)data_stream::ACL_FREEBSD_DEFAULT_ACL,
  FREEBSD_ACCESS = (int)data_stream::ACL_FREEBSD_ACCESS_ACL,
  HPUX_ENTRY = (int)data_stream::ACL_HPUX_ACL_ENTRY,
  IRIX_DEFAULT = (int)data_stream::ACL_IRIX_DEFAULT_ACL,
  IRIX_ACCESS = (int)data_stream::ACL_IRIX_ACCESS_ACL,
  LINUX_DEFAULT = (int)data_stream::ACL_LINUX_DEFAULT_ACL,
  LINUX_ACCESS = (int)data_stream::ACL_LINUX_ACCESS_ACL,
  TRU64_DEFAULT = (int)data_stream::ACL_TRU64_DEFAULT_ACL,
  TRU64_DEFAULT_DIR = (int)data_stream::ACL_TRU64_DEFAULT_DIR_ACL,
  TRU64_ACCESS = (int)data_stream::ACL_TRU64_ACCESS_ACL,
  SOLARISENT = (int)data_stream::ACL_SOLARIS_ACLENT,
  SOLARIS_ACE = (int)data_stream::ACL_SOLARIS_ACE,
  AFS_TEXT = (int)data_stream::ACL_AFS_TEXT,
  AIX_AIXC = (int)data_stream::ACL_AIX_AIXC,
  AIX_NFS4 = (int)data_stream::ACL_AIX_NFS4,
  FREEBSD_NFS4 = (int)data_stream::ACL_FREEBSD_NFS4_ACL,
  HURD_DEFAULT = (int)data_stream::ACL_HURD_DEFAULT_ACL,
  HURD_ACCESS = (int)data_stream::ACL_HURD_ACCESS_ACL,
  PLUGIN = (int)data_stream::ACL_PLUGIN,
};

enum class attr_stream : int
{
  DEFAULT = (int)data_stream::UNIX_ATTRIBUTES,
  WIN32 = (int)data_stream::UNIX_ATTRIBUTES_EX,
};

enum class digest_stream : int
{
  MD5 = (int)data_stream::MD5_DIGEST,
  SHA1 = (int)data_stream::SHA1_DIGEST,
  SHA256 = (int)data_stream::SHA256_DIGEST,
  SHA512 = (int)data_stream::SHA512_DIGEST,
  XXH128 = (int)data_stream::XXH128_DIGEST,
  SIGNED = (int)data_stream::SIGNED_DIGEST,
};

struct bareos_stat {
  std::uint64_t dev;
  std::uint64_t ino;
  std::uint64_t mode;
  std::uint64_t nlink;
  std::uint64_t uid;
  std::uint64_t gid;
  std::uint64_t rdev;
  std::uint64_t size;
  std::uint64_t blksize;
  std::uint64_t blocks;
  std::uint64_t atime;
  std::uint64_t mtime;
  std::uint64_t ctime;
  std::uint64_t flags;
};

struct encoded_meta {
  attr_stream stream;
  std::string enc;
};

class file_index {
  struct invalid_index_type {};

 public:
  static constexpr invalid_index_type INVALID{};

  std::int32_t to_underlying() const { return id; }

  constexpr explicit file_index(std::int32_t id) : id{id} {}
  constexpr file_index(invalid_index_type) : file_index{0} {}

 private:
  std::int32_t id;
};

class bareos_file {
 public:
  file_type type() { return type_; }
  std::string_view bareos_path() { return path; }
  const bareos_stat& lstat() { return stat; }

  virtual bool has_data() = 0;
  virtual data_stream stream() = 0;
  virtual std::optional<encoded_meta> extra_meta() { return std::nullopt; };
  virtual BareosFilePacket open() = 0;
  virtual std::optional<BareosFilePacket> open_rsrc() = 0;
  virtual bool send_postamble(send_context& sctx, file_index idx) = 0;
  virtual bool send_preamble(send_context& sctx, file_index idx) = 0;
  virtual ~bareos_file() = default;

  bareos_file(file_type type_, std::string path, bareos_stat stat)
      : type_{type_}, path{std::move(path)}, stat{stat}
  {
  }


 private:
  file_type type_;
  std::string path;
  bareos_stat stat;
};

enum class save_file_result
{
  Error,
  Success,
  Skip,
};

static inline bool DoBackupAcl(JobControlRecord* jcr,
                               send_context& sctx,
                               file_index fi,
                               AclData* data)
{
  bacl_exit_code retval;

  data->start_saving();
  if (jcr->IsPlugin()) {
    retval = PluginBuildAclStreams(jcr, data);
  } else {
    retval = BuildAclStreams(jcr, data);
  }
  std::vector msgs = data->reap_saved();

  switch (retval) {
    case bacl_exit_fatal:
      return false;
    case bacl_exit_error:
      Jmsg(jcr, M_ERROR, 0, "%s", jcr->errmsg);
      data->u.build->nr_errors++;
      [[fallthrough]];
    case bacl_exit_ok: {
    } break;
  }

  for (auto& msg : msgs) {
    if (!sctx.format("%ld %d 0", fi.to_underlying(), msg.stream)
        // think of a way to remove the copy
        || !sctx.send((const char*)msg.content.data(), msg.content.size())
        || !sctx.signal(BNET_EOD)) {
      return false;
    } else {
      jcr->JobBytes += msg.content.size();
    }
  }

  return true;
}

static inline bool DoBackupXattr(JobControlRecord* jcr,
                                 send_context& sctx,
                                 file_index fi,
                                 XattrData* data)
{
  BxattrExitCode retval;

  data->start_saving();
  if (jcr->IsPlugin()) {
    retval = PluginBuildXattrStreams(jcr, data);
  } else {
    retval = BuildXattrStreams(jcr, data);
  }

  std::vector msgs = data->reap_saved();

  switch (retval) {
    case BxattrExitCode::kErrorFatal:
      return false;
    case BxattrExitCode::kWarning:
      Jmsg(jcr, M_WARNING, 0, "%s", jcr->errmsg);
      break;
    case BxattrExitCode::kError:
      Jmsg(jcr, M_ERROR, 0, "%s", jcr->errmsg);
      data->u.build->nr_errors++;
      break;
      [[fallthrough]];
    case BxattrExitCode::kSuccess: {
    } break;
  }

  for (auto& msg : msgs) {
    if (!sctx.format("%ld %d 0", fi.to_underlying(), msg.stream)
        // todo: think of a way to remove the copy
        || !sctx.send((const char*)msg.content.data(), msg.content.size())
        || !sctx.signal(BNET_EOD)) {
      return false;
    } else {
      jcr->JobBytes += msg.content.size();
    }
  }

  return true;
}


template <typename T> struct span {
  T* array{nullptr};
  std::size_t count{0};

  span() = default;
  span(T* array, std::size_t count) : array{array}, count{count} {}

  span(std::vector<T>& vec) : array{vec.data()}, count{vec.size()} {}

  std::size_t size() const { return count; }

  T* data() { return array; }
  T* begin() { return array; }
  T* end() { return array + count; }

  span<const T> as_const() { return span<const T>{array, count}; }
};

class bareos_file_ref {
 public:
  file_index index() { return idx; }
  std::string_view bareos_path() { return path; }

  std::pair<digest_stream, span<const char>> checksum()
  {
    return std::make_pair(digest, std::cref(encoded_checksum));
  }

  bareos_file_ref(std::string path,
                  file_index idx,
                  digest_stream stream,
                  span<const char> checksum)
      : path{std::move(path)}
      , idx{idx}
      , digest{stream}
      , encoded_checksum{checksum}
  {
  }

  bareos_file_ref(std::string path)
      : bareos_file_ref(std::move(path), file_index::INVALID, {}, {})
  {
  }

 private:
  std::string path;
  file_index idx;

  digest_stream digest;
  span<const char> encoded_checksum;
};


static constexpr file_index INVALID{0};

file_index next_file_index(JobControlRecord* jcr)
{
  return file_index{static_cast<std::int32_t>(++jcr->JobFiles)};
}

static uint8_t const base64_digits[64]
    = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
       'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
       'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
       'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
       '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

/* Convert a value to base64 characters.
 * The result is stored in where, which
 * must be at least 8 characters long.
 *
 * Returns the number of characters
 * stored (not including the EOS).
 */
static int ToBase64(int64_t value, char* where)
{
  uint64_t val;
  int i = 0;
  int n;

  /* Handle negative values */
  if (value < 0) {
    where[i++] = '-';
    value = -value;
  }

  /* Determine output size */
  val = value;
  do {
    val >>= 6;
    i++;
  } while (val);
  n = i;

  /* Output characters */
  val = value;
  where[i] = 0;
  do {
    where[--i] = base64_digits[val & (uint64_t)0x3F];
    val >>= 6;
  } while (val);
  return n;
}

encoded_meta EncodeDefaultMeta(const bareos_stat& statp,
                               file_index link,
                               data_stream stream)
{
  std::string encoded;
  encoded.resize(16 * 8 + 16);
  char* p = encoded.data();
  p += ToBase64((int64_t)statp.dev, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.ino, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.mode, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.nlink, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.uid, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.gid, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.rdev, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.size, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.blksize, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.blocks, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.atime, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.mtime, p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.ctime, p);
  *p++ = ' ';
  p += ToBase64((int64_t)link.to_underlying(), p);
  *p++ = ' ';
  p += ToBase64((int64_t)statp.flags, p);
  *p++ = ' ';
  p += ToBase64((int64_t)stream, p);

  ASSERT(p < encoded.data() + encoded.size());
  encoded.resize(p - encoded.data());

  return {attr_stream::DEFAULT, std::move(encoded)};
}

bool SendMetaInfo(
    send_context& sctx,
    file_index idx,
    file_type type,
    std::string_view name, /* canonical name; i.e. dirs with trailing slash */
    std::optional<bareos_file_ref> original,
    std::uint32_t delta_seq,
    encoded_meta def,
    std::optional<encoded_meta> extra)
{
  attr_stream stream = def.stream;
  const char* extra_str = "";

  if (extra) {
    extra_str = extra->enc.c_str();
    stream = extra->stream;
  }

  if (!sctx.format("%ld %ld 0", idx.to_underlying(), stream)) { return false; }

  switch (type) {
    case file_type::LNK:
      [[fallthrough]];
    case file_type::JUNCTION:
      [[fallthrough]];
    case file_type::LNKSAVED: {
      if (!original) { return false; }
      if (!sctx.format("%ld %d %s%c%s%c%s%c%s%c%u%c", idx.to_underlying(), type,
                       std::string{name}.c_str(), 0, def.enc.c_str(), 0,
                       std::string{original->bareos_path()}.c_str(), 0,
                       extra_str, 0, delta_seq, 0)) {
        return false;
      }
    } break;
    case file_type::DIREND:
      [[fallthrough]];
    case file_type::REPARSE: {
      if (!sctx.format("%ld %d %s%c%s%c%s%c%s%c%u%c", idx.to_underlying(), type,
                       std::string{name}.c_str(), 0, def.enc.c_str(), 0, "", 0,
                       extra_str, 0, delta_seq, 0)) {
        return false;
      }
    } break;
    default: {
      if (!sctx.format("%ld %d %s%c%s%c%s%c%s%c%d%c", idx.to_underlying(), type,
                       std::string{name}.c_str(), 0, def.enc.c_str(), 0, "", 0,
                       extra_str, 0, delta_seq, 0)) {
        return false;
      }
    } break;
  };

  if (!sctx.signal(BNET_EOD)) { return false; }

  return true;
}

// struct file_stream {
//   BareosSocket* sock;
//   file_index idx;

//   struct open_stream
//   {
//     const file_stream *stream;
//     open_stream(const file_stream* stream) : stream{stream}
//     {
//     }

//     bool write(const std::vector<char>&) {

//     }

//     open_stream(const open_stream&) = delete;
//     open_stream& operator=(const open_stream&) = delete;

//   private:
//     ~open_stream() {}
//   };
//   file_stream(BareosSocket* sock, file_index idx) : sock{sock}
// 						  , idx{idx}
//   {

//     open_stream open(data_stream stream) {
//       sock->fsend("%ld %d 0", idx.to_underlying(), stream);
//       return open_stream{this};
//     }

//     bool close(open_stream open) {
//       ASSERT(open.stream == this);


//     }
//   }

// };

static inline bool SendFinder(send_context& sctx,
                              file_index idx,
                              span<const char> finder_info,
                              DIGEST* checksum,
                              DIGEST* signing)
{
  // Dmsg1(300, "Saving Finder Info for \"%s\"\n", bsctx.ff_pkt->fname);
  sctx.format("%ld %d 0", idx.to_underlying(), STREAM_HFSPLUS_ATTRIBUTES);
  sctx.send(finder_info.data(), finder_info.size());

  if (checksum) {
    CryptoDigestUpdate(checksum, (const uint8_t*)finder_info.data(),
                       finder_info.size());
  }
  if (signing) {
    CryptoDigestUpdate(signing, (const uint8_t*)finder_info.data(),
                       finder_info.size());
  }
  sctx.signal(BNET_EOD);

  return true;
}

struct send_options {
  struct compression {
    uint32_t algo;
    uint32_t level;
  };

  struct encryption {
    CIPHER_CONTEXT* cipher{nullptr};
    std::size_t buf_size{0};
    POOLMEM* buf{nullptr};

    encryption() = default;
    encryption(const encryption&) = delete;
    encryption& operator=(const encryption&) = delete;
    encryption(encryption&& other) : encryption() { *this = std::move(other); }
    encryption& operator=(encryption&& other)
    {
      std::swap(cipher, other.cipher);
      std::swap(buf_size, other.buf_size);
      std::swap(buf, other.buf);
      return *this;
    }
    ~encryption()
    {
      if (buf) { FreeAndNullPoolMemory(buf); }
      if (cipher) { CryptoCipherFree(cipher); }
    }
  };

  std::optional<compression> compress;
  std::optional<encryption> encrypt;
  bool discard_empty_blocks;
  bool insert_file_offsets;
};

class data_message {
  /* some data is prefixed by a OFFSET_FADDR_SIZE-byte number -- called header
   * here, which basically contains the file position to which to write the
   * following block of data.
   * The difference between FADDR and OFFSET is that offset may be any value
   * (given to the core by a plugin), whereas FADDR is computed by the core
   * itself and is equal to the number of bytes already read from the file
   * descriptor. */
  static inline constexpr std::size_t header_size = OFFSET_FADDR_SIZE;
  /* our bsocket functions assume that they are allowed to overwrite
   * the four bytes directly preceding the given buffer
   * To keep the message alignment to 8, we "allocate" full 8 bytes instead
   * of the required 4. */
  static inline constexpr std::size_t bnet_size = 8;
  static inline constexpr std::size_t data_offset = header_size + bnet_size;

  std::vector<char> buffer{};
  bool has_header{false};

 public:
  data_message(std::size_t data_size)
  {
    buffer.resize(data_size + data_offset);
  }
  data_message() : data_message(0) {}
  data_message(const data_message&) = delete;
  data_message& operator=(const data_message&) = delete;
  data_message(data_message&&) = default;
  data_message& operator=(data_message&&) = default;

  // creates a message with the same header -- if any
  data_message derived(std::size_t size = 0) const
  {
    data_message derived(size);

    if (has_header) {
      derived.has_header = true;
      std::memcpy(derived.header_ptr(), header_ptr(), header_size);
    }

    return derived;
  }

  void set_header(std::uint64_t h)
  {
    has_header = true;
    auto* ptr = header_ptr();
    std::memcpy(ptr, &h, header_size);

    for (std::size_t i = 0; i < header_size / 2; ++i) {
      std::swap(ptr[i], ptr[header_size - 1 - i]);
    }
  }

  void resize(std::size_t new_size) { buffer.resize(data_offset + new_size); }

  char* header_ptr() { return &buffer[bnet_size]; }
  char* data_ptr() { return &buffer[bnet_size + header_size]; }
  const char* header_ptr() const { return &buffer[bnet_size]; }
  const char* data_ptr() const { return &buffer[bnet_size + header_size]; }

  std::size_t data_size() const
  {
    ASSERT(buffer.size() >= data_offset);
    return buffer.size() - data_offset;
  }

  std::pair<POOLMEM*, std::size_t> transmute_to_message()
  {
    auto size = message_size();
    POOLMEM* mem = GetMemory(size);

    std::memcpy(mem, as_socket_message(), size);

    return {mem, size};
  }
  /* important: this is not actually a POOLMEM*; do not pass it to POOLMEM*
   *            functions, except to pass it to BareosSocket::SendData(). */
  POOLMEM* as_socket_message() const
  {
    if (has_header) {
      return const_cast<char*>(header_ptr());
    } else {
      return const_cast<char*>(data_ptr());
    }
  }

  std::size_t message_size() const
  {
    auto size_with_header = buffer.size() - bnet_size;
    if (has_header) {
      return size_with_header;
    } else {
      return size_with_header - header_size;
    }
  }
};

// Send plugin name start/end record to SD
// TODO: put into fd_plugins.cc
static bool SendPluginName(send_context& sctx,
                           save_pkt* sp,
                           int file_index,
                           bool start)
{
  auto debuglevel = 100;
  Dmsg1(debuglevel, "SendPluginName=%s\n", sp->cmd);

  // Send stream header
  if (!sctx.format("%ld %d 0", file_index, STREAM_PLUGIN_NAME)) {
    return false;
  }

  if (start) {
    // Send data -- not much
    if (!sctx.format("%ld 1 %d %s%c", file_index, sp->portable, sp->cmd, 0)) {
      return false;
    }
  } else {
    // Send end of data
    if (!sctx.format("%ld 0", file_index)) { return false; }
  }

  sctx.signal(BNET_EOD); /* indicate end of plugin name data */

  return true;
}

static bool SendDataToSd(send_context& sctx,
                         send_options& options,
                         data_message msg,
                         DIGEST* checksum,
                         DIGEST* signing)
{
  {
    auto* ptr = reinterpret_cast<const uint8_t*>(msg.data_ptr());
    auto length = msg.data_size();

    // Update checksum digest if requested
    if (checksum) { CryptoDigestUpdate(checksum, ptr, length); }

    // Update signing digest if requested
    if (signing) { CryptoDigestUpdate(signing, ptr, length); }
  }

  if (options.compress) {
#if 0
    auto& c = options.compress.value();
    std::size_t max_size
      = RequiredCompressionOutputBufferSize(c.algo, msg.data_size());

    auto der = msg.derived(max_size);

    auto compressed_length = ThreadlocalCompress(c.algo, c.level,
						 der.data_ptr(), der.data_size(),
						 msg.data_ptr(), msg.data_size());

    if (!compressed_length) {
      Dmsg1(50, "compression error\n");
      return false;
    }

    msg = der;
#endif
  }

  POOLMEM* data{nullptr};
  std::size_t size{0};
  if (options.encrypt) {
    std::int64_t res
        = EncryptData(options.encrypt->cipher, options.encrypt->buf,
                      msg.data_ptr(), msg.data_size());

    if (res < 0) {
      // encryption error
      return false;
    }

    if (res == 0) {
      return true;  // too little data, nothing to send
    }

    data = options.encrypt->buf;
    size = res;
    options.encrypt->buf = GetMemory(options.encrypt->buf_size);
  } else {
    std::tie(data, size) = msg.transmute_to_message();
  }

  Dmsg1(130, "Send data to SD len=%d\n", size);
  return sctx.send(data, size);
}

bool SendPlainData(send_context& sctx,
                   send_options& options,
                   std::size_t bufsize,
                   BareosFilePacket* bfd,
                   DIGEST* checksum,
                   DIGEST* signing)
{
  // Read the file data
  data_message msg(bufsize);
  std::size_t bytes_read = 0;
  for (;;) {
    auto message_length = bread(bfd, msg.data_ptr(), msg.data_size());
    if (message_length < 0) {
      return false;
    } else if (message_length == 0) {
      break;
    } else {
      if (options.discard_empty_blocks) {
        if (IsBufZero(msg.data_ptr(), message_length)) {
          continue;
        } else {
          msg.set_header(bytes_read);
        }
      } else if (options.insert_file_offsets) {
        msg.set_header(bfd->offset);
      }

      msg.resize(bufsize);
      SendDataToSd(sctx, options, std::move(msg), checksum, signing);
      msg = data_message(bufsize);
    }
    bytes_read += message_length;
  }

  return true;
}


#ifdef HAVE_WIN32

struct efs_callback_context {
  send_context* sctx;
  send_options* options;
  DIGEST* checksum;
  DIGEST* signing;
};
// Callback method for ReadEncryptedFileRaw()
static DWORD WINAPI send_efs_data(PBYTE pbData,
                                  PVOID pvCallbackContext,
                                  ULONG ulLength)
{
  efs_callback_context* ecc = (efs_callback_context*)pvCallbackContext;

  if (ulLength == 0) { return ERROR_SUCCESS; }

  POOLMEM* mem = GetMemory(ulLength);
  std::memcpy(mem, pbData, ulLength);
  if (!SendDataToSd(*ecc->sctx, *ecc->options, mem, ulLength, ecc->checksum,
                    ecc->signing)) {
    return ERROR_NET_WRITE_FAULT;
  }

  return ERROR_SUCCESS;
}

// Send the content of an Encrypted file on an EFS filesystem.
static inline bool SendEncryptedData(send_context& sctx,
                                     send_options& options,
                                     BareosFilePacket* bfd,
                                     DIGEST* checksum,
                                     DIGEST* signing)
{
  bool retval = false;

  if (!p_ReadEncryptedFileRaw) {
    Jmsg0(bctx.jcr, M_FATAL, 0,
          _("Encrypted file but no EFS support functions\n"));
  }

  efs_callback_context ecc = {&sctx, &options, checksum, signing};
  /* The EFS read function, ReadEncryptedFileRaw(), works in a specific way.
   * You have to give it a function that it calls repeatedly every time the
   * read buffer is filled.
   *
   * So ReadEncryptedFileRaw() will not return until it has read the whole file.
   */
  if (p_ReadEncryptedFileRaw((PFE_EXPORT_FUNC)send_efs_data, &ecc,
                             bfd->pvContext)) {
    goto bail_out;
  }
  retval = true;

bail_out:
  return retval;
}
#endif

struct file_info {
  bool is_block_file;
  bool is_encrypted;
};

bool SendData(send_context& sctx,
              send_options& options,
              file_index idx,
              data_stream stream,
              std::size_t bufsize,
              [[maybe_unused]] file_info finfo,
              BareosFilePacket* bfd,
              DIGEST* checksum,
              DIGEST* signing)
{
  if (!sctx.format("%ld %d 0", idx.to_underlying(), stream)) { return false; }

  /* Make space at beginning of buffer for fileAddr because this
   *   same buffer will be used for writing if compression is off. */
  if (options.discard_empty_blocks || options.insert_file_offsets) {
#ifdef HAVE_FREEBSD_OS
    // To read FreeBSD partitions, the read size must be a multiple of 512.
    bufsize = (bufsize / 512) * 512;
#endif
  }

  // A RAW device read on win32 only works if the buffer is a multiple of 512
#ifdef HAVE_WIN32
  if (finfo.is_block_file) { bufsize = (bufsize / 512) * 512; }

  if (finfo.is_encrypted) {
    if (!SendEncryptedData(sctx, options, bfd, checksum, signing)) {
      return false;
    }
  } else {
    if (!SendPlainData(sctx, options, bufsize, bfd, checksum, signing)) {
      return false;
    }
  }
#else
  if (!SendPlainData(sctx, options, bufsize, bfd, checksum, signing)) {
    return false;
  }
#endif

  if (options.encrypt) {
    uint32_t len = 0;
    if (!CryptoCipherFinalize(options.encrypt->cipher,
                              (uint8_t*)options.encrypt->buf, &len)) {
      return false;
    }

    auto* buf = options.encrypt->buf;
    options.encrypt->buf = nullptr;
    if (!sctx.send(buf, len)) { return false; }
    // todo update job bytes
  }

  if (!sctx.signal(BNET_EOD)) { return false; }
  return true;
}

std::vector<char> TerminateChecksum(DIGEST* checksum)
{
  std::vector<char> buffer;
  buffer.resize(CRYPTO_DIGEST_MAX_SIZE);
  uint32_t size = buffer.size();

  if (!CryptoDigestFinalize(checksum, (uint8_t*)buffer.data(), &size)) {
    return {};
  }

  ASSERT(size <= buffer.size());
  buffer.resize(size);

  return buffer;
}

std::vector<char> TerminateSigning(JobControlRecord* jcr,
                                   X509_KEYPAIR* keypair,
                                   DIGEST* signing)
{
  struct deleter {
    void operator()(SIGNATURE* sign) const
    {
      if (sign) CryptoSignFree(sign);
    }
  };
  auto signature = std::unique_ptr<SIGNATURE, deleter>(crypto_sign_new(jcr));
  if (!signature) { return {}; }

  if (!CryptoSignAddSigner(signature.get(), signing, keypair)) { return {}; }

  uint32_t size;

  if (!CryptoSignEncode(signature.get(), NULL, &size)) { return {}; }

  std::vector<char> buffer;
  buffer.resize(size);

  if (!CryptoSignEncode(signature.get(), (uint8_t*)buffer.data(), &size)) {
    return {};
  }

  ASSERT(size <= buffer.size());
  buffer.resize(size);

  return buffer;
}

bool SendDigest(send_context& sctx,
                file_index idx,
                digest_stream stream,
                span<const char> buffer)
{
  if (!sctx.format("%ld %d 0", idx.to_underlying(), stream)) { return false; }

  if (!sctx.send(buffer.data(), buffer.size())) { return false; }

  if (!sctx.signal(BNET_EOD)) { return false; }

  return true;
}

enum class checksum_type : int
{
  MD5 = (int)digest_stream::MD5,
  SHA1 = (int)digest_stream::SHA1,
  SHA256 = (int)digest_stream::SHA256,
  SHA512 = (int)digest_stream::SHA512,
  XXH128 = (int)digest_stream::XXH128,
};

DIGEST* SetupChecksum(JobControlRecord* jcr, checksum_type type)
{
  switch (type) {
    case checksum_type::MD5: {
      return crypto_digest_new(jcr, CRYPTO_DIGEST_MD5);
    } break;
    case checksum_type::SHA1: {
      return crypto_digest_new(jcr, CRYPTO_DIGEST_SHA1);
    } break;
    case checksum_type::SHA256: {
      return crypto_digest_new(jcr, CRYPTO_DIGEST_SHA256);
    } break;
    case checksum_type::SHA512: {
      return crypto_digest_new(jcr, CRYPTO_DIGEST_SHA512);
    } break;
    case checksum_type::XXH128: {
      return crypto_digest_new(jcr, CRYPTO_DIGEST_XXH128);
    } break;
  }

  return nullptr;
}

DIGEST* SetupSigning(JobControlRecord* jcr)
{
#ifdef HAVE_SHA2
  crypto_digest_t signing_algorithm = CRYPTO_DIGEST_SHA256;
#else
  crypto_digest_t signing_algorithm = CRYPTO_DIGEST_SHA1;
#endif

  return crypto_digest_new(jcr, signing_algorithm);
}

struct save_options {
  CRYPTO_SESSION* encrypt{nullptr};    // nullptr == no encryption
  X509_KEYPAIR* signing_key{nullptr};  // nullptr == no signing
  std::optional<checksum_type> checksum;
  bool compress{false};
  bool acl{false};
  bool xattr{false};
  bool discard_empty_blocks{false};
  bool insert_file_offsets{false};
};

digest_stream DigestStream(DIGEST* digest)
{
  switch (digest->type) {
    case CRYPTO_DIGEST_MD5:
      return digest_stream::MD5;
    case CRYPTO_DIGEST_SHA1:
      return digest_stream::SHA1;
    case CRYPTO_DIGEST_SHA256:
      return digest_stream::SHA256;
    case CRYPTO_DIGEST_SHA512:
      return digest_stream::SHA512;
    case CRYPTO_DIGEST_XXH128:
      return digest_stream::XXH128;
    default: {
      __builtin_unreachable();
    }
  }
};

std::optional<send_options::encryption> SetupEncryption(
    CRYPTO_SESSION* pki_session,
    std::size_t bufsize)
{
  send_options::encryption enc;
  uint32_t cipher_block_size;
  enc.cipher = crypto_cipher_new(pki_session, true, &cipher_block_size);
  if (enc.cipher == nullptr) { return std::nullopt; }

  enc.buf_size = bufsize + sizeof(std::uint32_t) + cipher_block_size;
  enc.buf = GetMemory(enc.buf_size);

  return enc;
}

bool SendEncryptionSession(send_context& sctx,
                           file_index fi,
                           CRYPTO_SESSION* pki_session)
{
  ASSERT(pki_session);

  std::uint32_t size;
  if (!CryptoSessionEncode(pki_session, nullptr, &size)) {
    // Jmsg(jcr, M_FATAL, 0,
    // 	 _("An error occurred while encrypting the stream.\n"));
    return false;
  }
  PoolMem encoded(PM_MESSAGE);
  encoded.check_size(size);
  if (!CryptoSessionEncode(pki_session, (uint8_t*)encoded.addr(), &size)) {
    // Jmsg(jcr, M_FATAL, 0,
    // 	 _("An error occurred while encrypting the stream.\n"));
    return false;
  }

  if (!sctx.format("%ld %d 0", fi.to_underlying(),
                   STREAM_ENCRYPTED_SESSION_DATA)) {
    return false;
  }
  if (!sctx.send(encoded.release(), size)) { return false; }
  if (!sctx.signal(BNET_EOD)) { return false; }

  return true;
}

save_file_result SaveFile(JobControlRecord* jcr,
                          bareos_file* file,
                          std::optional<std::uint32_t> delta_seq,
                          std::optional<bareos_file_ref> original,
                          save_options options)
{
  if (jcr->IsJobCanceled() || jcr->IsIncomplete()) {
    return save_file_result::Skip;
  }

  if (!jcr->fd_impl->send_ctx) {
    Jmsg1(jcr, M_FATAL, 0, "Send context not initialised.");
    return save_file_result::Error;
  }

  auto& sctx = jcr->fd_impl->send_ctx.value();

  std::string bpath(file->bareos_path());

  Dmsg1(130, "filed: sending %s to stored\n", bpath.c_str());

  send_options opts{};
  if (options.encrypt) {
    if (options.discard_empty_blocks || options.insert_file_offsets) {
      //   Jmsg0(bctx.jcr, M_FATAL, 0,
      //         _("Encrypting sparse or offset data not supported.\n"));
      return save_file_result::Error;
    }

    opts.encrypt = SetupEncryption(options.encrypt, jcr->buf_size);
    if (!opts.encrypt) { return save_file_result::Error; }
  }

  file_index fi = next_file_index(jcr);

  file->send_preamble(sctx, fi);

  {
    // encode & send attributes
    file_index orig_index = file_index::INVALID;
    if (original) { orig_index = original->index(); }

    auto stats = EncodeDefaultMeta(file->lstat(), orig_index, file->stream());

    std::optional extra = file->extra_meta();

    if (!SendMetaInfo(sctx, fi, file->type(), file->bareos_path(),
                      std::move(original), delta_seq.value_or(0),
                      std::move(stats), std::move(extra))) {
      if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
        Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
              sctx.error());
      }
      return save_file_result::Error;
    }
  }
  DIGEST *checksum = nullptr, *signing = nullptr;
  if (options.checksum) {
    checksum = SetupChecksum(jcr, *options.checksum);
    if (!checksum) { return save_file_result::Success; }
  }
  if (options.signing_key) {
    signing = SetupSigning(jcr);
    if (!signing) { return save_file_result::Success; }
  }

  if (file->has_data()) {
    if (options.encrypt) { SendEncryptionSession(sctx, fi, options.encrypt); }
    // todo: this should be an raii type

    BareosFilePacket bfd = file->open();


    file_info finfo = {};

#ifdef HAVE_WIN32
    finfo.is_block_file = S_ISBLK(file->lstat().st_mode);
    finfo.is_encrypted = file->lstat().st_rdev & FILE_ATTRIBUTE_ENCRYPTED;
#endif
    if (!SendData(sctx, opts, fi, file->stream(), jcr->buf_size, finfo, &bfd,
                  checksum, signing)) {
      if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
        Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
              sctx.error());
      }

      bclose(&bfd);
      return save_file_result::Error;
    }

    bclose(&bfd);
  }

#if 0
  std::optional rsrc_bfd = file->open_rsrc();
  if (rsrc_bfd) {
    auto rsrc_stream = options.encrypt
      ? data_stream::ENCRYPTED_MACOS_FORK_DATA
      : data_stream::MACOS_FORK_DATA;
    if (!SendData(sctx, fi, rsrc_stream, jcr->buf_size, &rsrc_bfd.value(),
		  checksum, signing)) {
      if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
	Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
	      sctx.error());
      }
      bclose(&rsrc_bfd.value());
      return save_file_result::Error;
    }
    bclose(&rsrc_bfd.value());
  }

  if (!SendFinder(sctx, fi, finder_info, checksum, signing)) {
    if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
	    sctx.error());
    }
    return save_file_result::Error;
  }
#endif

  // Save ACLs when requested and available for anything not being a symlink.
  if constexpr (have_acl) {
    if (options.acl) {
      AclData* data = jcr->fd_impl->acl_data.get();
      data->filetype = (int)file->type();
      data->last_fname = bpath.c_str();  // TODO: probably systempath here ?
      data->next_dev = file->lstat().dev;
      if (!DoBackupAcl(jcr, sctx, fi, data)) { return save_file_result::Error; }
    }
  }

  if constexpr (have_xattr) {
    if (options.xattr) {
      XattrData* data = jcr->fd_impl->xattr_data.get();
      data->last_fname = bpath.c_str();  // TODO: probably systempath here ?
      data->next_dev = file->lstat().dev;
      data->ignore_acls = options.acl;
      if (!DoBackupXattr(jcr, sctx, fi, data)) {
        return save_file_result::Error;
      }
    }
  }

  if (checksum) {
    auto check_encoded = TerminateChecksum(checksum);
    if (check_encoded.size()) {
      if (!SendDigest(sctx, fi, DigestStream(checksum),
                      span{check_encoded}.as_const())) {
        if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
          Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
                sctx.error());
        }
        // todo: handle error case here
      }
    } else {
      // todo: handle error case here
    }
  }

  if (signing) {
    auto sign_encoded = TerminateSigning(jcr, options.signing_key, signing);
    if (sign_encoded.size()) {
      if (!SendDigest(sctx, fi, digest_stream::SIGNED,
                      span{sign_encoded}.as_const())) {
        if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
          Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
                sctx.error());
        }
        // todo: handle error case here
      }
    } else {
      // todo: handle error case here
    }
  }

  // this should always happen -> raii
  if (checksum) { CryptoDigestFree(checksum); }
  if (signing) { CryptoDigestFree(signing); }

  if (file->type() == file_type::LNKSAVED && original) {
    auto [stream, check_encoded] = original->checksum();

    if (check_encoded.size()) { SendDigest(sctx, fi, stream, check_encoded); }
  }

  file->send_postamble(sctx, fi);

  return save_file_result::Success;
}

class submit_context {
  using packet = std::pair<std::unique_ptr<bareos_file>, save_options>;
  JobControlRecord* jcr;
  channel::input<packet> in;
  channel::output<packet> out;

  std::thread sender;

  submit_context(
      std::pair<channel::input<packet>, channel::output<packet>> cpair,
      JobControlRecord* jcr)
      : jcr{jcr}
      , in{std::move(cpair.first)}
      , out{std::move(cpair.second)}
      , sender{send_work, this}
  {
  }

  static void send_work(submit_context* ctx) { ctx->do_submit_work(); }

  void do_submit_work()
  {
    for (;;) {
      std::optional data = out.get();
      if (!data) { break; }

      auto [file, opts] = std::move(data).value();

      auto res = SaveFile(jcr, file.get(), std::nullopt, std::nullopt, opts);

      (void)res;
    }
  }

 public:
  bool submit(std::unique_ptr<bareos_file> file, save_options opts)
  {
    return in.emplace(std::move(file), opts);
  }

  const char* error() { return ""; }

  submit_context(JobControlRecord* jcr)
      : submit_context(channel::CreateBufferedChannel<packet>(20), jcr)
  {
  }

  ~submit_context()
  {
    in.close();
    sender.join();
  }
};

/**
 * Find all the requested files and send them
 * to the Storage daemon.
 *
 * Note, we normally carry on a one-way
 * conversation from this point on with the SD, sfd_imply blasting
 * data to him.  To properly know what is going on, we
 * also run a "heartbeat" monitor which reads the socket and
 * reacts accordingly (at the moment it has nothing to do
 * except echo the heartbeat to the Director).
 */
bool BlastDataToStorageDaemon(JobControlRecord* jcr, crypto_cipher_t cipher)
{
  bool ok = true;
  BareosSocket* sd = jcr->store_bsock;

  jcr->setJobStatusWithPriorityCheck(JS_Running);

  Dmsg1(300, "filed: opened data connection %d to stored\n", sd->fd_);
  ClientResource* client = nullptr;
  {
    ResLocker _{my_config};
    client = (ClientResource*)my_config->GetNextRes(R_CLIENT, NULL);
  }
  uint32_t buf_size;
  if (client) {
    buf_size = client->max_network_buffer_size;
  } else {
    buf_size = 0; /* use default */
  }
  if (!sd->SetBufferSize(buf_size, BNET_SETBUF_WRITE)) {
    jcr->setJobStatusWithPriorityCheck(JS_ErrorTerminated);
    Jmsg(jcr, M_FATAL, 0, _("Cannot set buffer size FD->SD.\n"));
    return false;
  }

  jcr->buf_size = sd->message_length;

  if (!AdjustCompressionBuffers(jcr)) { return false; }

  if (!CryptoSessionStart(jcr, cipher)) { return false; }

  SetFindOptions((FindFilesPacket*)jcr->fd_impl->ff, jcr->fd_impl->incremental,
                 jcr->fd_impl->since_time);

  // In accurate mode, we overload the find_one check function
  if (jcr->accurate) {
    SetFindChangedFunction((FindFilesPacket*)jcr->fd_impl->ff,
                           AccurateCheckFile);
  }

  StartHeartbeatMonitor(jcr);
  Bmicrosleep(3, 0);

  if (have_acl) {
    jcr->fd_impl->acl_data = std::make_unique<AclData>();
    jcr->fd_impl->acl_data->u.build
        = (acl_build_data_t*)malloc(sizeof(acl_build_data_t));
    memset(jcr->fd_impl->acl_data->u.build, 0, sizeof(acl_build_data_t));
    jcr->fd_impl->acl_data->u.build->content = GetPoolMemory(PM_MESSAGE);
  }

  if (have_xattr) {
    jcr->fd_impl->xattr_data = std::make_unique<XattrData>();
    jcr->fd_impl->xattr_data->u.build
        = (xattr_build_data_t*)malloc(sizeof(xattr_build_data_t));
    memset(jcr->fd_impl->xattr_data->u.build, 0, sizeof(xattr_build_data_t));
    jcr->fd_impl->xattr_data->u.build->content = GetPoolMemory(PM_MESSAGE);
  }

  jcr->store_bsock = nullptr;
  jcr->fd_impl->send_ctx.emplace(sd);
  jcr->fd_impl->submit_ctx = new submit_context{jcr};

  // Subroutine SaveFile() is called for each file
  if (!FindFiles(jcr, (FindFilesPacket*)jcr->fd_impl->ff, SaveFile,
                 PluginSave)) {
    ok = false; /* error */
    jcr->setJobStatusWithPriorityCheck(JS_ErrorTerminated);
  }

  delete (submit_context*)jcr->fd_impl->submit_ctx;
  jcr->fd_impl->submit_ctx = nullptr;
  jcr->JobBytes = jcr->fd_impl->send_ctx->num_bytes_send();
  jcr->fd_impl->send_ctx.reset();
  jcr->store_bsock = sd;

  if (have_acl && jcr->fd_impl->acl_data->u.build->nr_errors > 0) {
    Jmsg(jcr, M_WARNING, 0,
         _("Encountered %ld acl errors while doing backup\n"),
         jcr->fd_impl->acl_data->u.build->nr_errors);
  }
  if (have_xattr && jcr->fd_impl->xattr_data->u.build->nr_errors > 0) {
    Jmsg(jcr, M_WARNING, 0,
         _("Encountered %ld xattr errors while doing backup\n"),
         jcr->fd_impl->xattr_data->u.build->nr_errors);
  }

#if defined(WIN32_VSS)
  CloseVssBackupSession(jcr);
#endif

  AccurateFinish(jcr); /* send deleted or base file list to SD */

  StopHeartbeatMonitor(jcr);

  sd->signal(BNET_EOD); /* end of sending data */

  if (have_acl && jcr->fd_impl->acl_data) {
    FreePoolMemory(jcr->fd_impl->acl_data->u.build->content);
    free(jcr->fd_impl->acl_data->u.build);
  }

  if (have_xattr && jcr->fd_impl->xattr_data) {
    FreePoolMemory(jcr->fd_impl->xattr_data->u.build->content);
    free(jcr->fd_impl->xattr_data->u.build);
  }

  if (jcr->fd_impl->big_buf) {
    free(jcr->fd_impl->big_buf);
    jcr->fd_impl->big_buf = NULL;
  }

  CleanupCompression(jcr);
  CryptoSessionEnd(jcr);

  Dmsg1(100, "end blast_data ok=%d\n", ok);
  return ok;
}


bareos_stat NativeToBareos(struct stat statp)
{
  return bareos_stat{
      .dev = statp.st_dev,
      .ino = statp.st_ino,
      .mode = statp.st_mode,
      .nlink = statp.st_nlink,
      .uid = statp.st_uid,
      .gid = statp.st_gid,
      .rdev = statp.st_rdev,
      .size = (uint64_t)statp.st_size,
      .blksize = (uint64_t)statp.st_blksize,
      .blocks = (uint64_t)statp.st_blocks,
      .atime = statp.st_atime,
      .mtime = statp.st_mtime,
      .ctime = statp.st_ctime,
      .flags = 0,
  };
}

file_type NativeToBareos(int type)
{
  switch (type) {
    case FT_LNKSAVED:
      return file_type::LNKSAVED;
    case FT_REGE:
      return file_type::REGE;
    case FT_REG:
      return file_type::REG;
    case FT_LNK:
      return file_type::LNK;
    case FT_DIREND:
      return file_type::DIREND;
    case FT_SPEC:
      return file_type::SPEC;
    case FT_NOACCESS:
      return file_type::NOACCESS;
    case FT_NOFOLLOW:
      return file_type::NOFOLLOW;
    case FT_NOSTAT:
      return file_type::NOSTAT;
    case FT_NOCHG:
      return file_type::NOCHG;
    case FT_DIRNOCHG:
      return file_type::DIRNOCHG;
    case FT_ISARCH:
      return file_type::ISARCH;
    case FT_NORECURSE:
      return file_type::NORECURSE;
    case FT_NOFSCHG:
      return file_type::NOFSCHG;
    case FT_NOOPEN:
      return file_type::NOOPEN;
    case FT_RAW:
      return file_type::RAW;
    case FT_FIFO:
      return file_type::FIFO;
    case FT_DIRBEGIN:
      return file_type::DIRBEGIN;
    case FT_INVALIDFS:
      return file_type::INVALIDFS;
    case FT_INVALIDDT:
      return file_type::INVALIDDT;
    case FT_REPARSE:
      return file_type::REPARSE;
    case FT_PLUGIN:
      return file_type::PLUGIN;
    case FT_DELETED:
      return file_type::DELETED;
    case FT_BASE:
      return file_type::BASE;
    case FT_RESTORE_FIRST:
      return file_type::RESTORE_FIRST;
    case FT_JUNCTION:
      return file_type::JUNCTION;
    case FT_PLUGIN_CONFIG:
      return file_type::PLUGIN_CONFIG;
    case FT_PLUGIN_CONFIG_FILLED:
      return file_type::PLUGIN_CONFIG_FILLED;
  };

  return file_type::REG;
}

struct test_file : bareos_file {
  std::string fname{};
  bool noatime{};
  data_stream my_stream{};
  bool hasdata{true};
  save_pkt* sp{nullptr};

  test_file(FindFilesPacket* ff, save_pkt* sp)
      : bareos_file(NativeToBareos(ff->type),
                    ff->fname,
                    NativeToBareos(ff->statp))
      , fname{ff->fname}
      , noatime{BitIsSet(FO_NOATIME, ff->flags)}
      , my_stream{(data_stream)SelectDataStream(ff)}
      , hasdata(ff->cmd_plugin ? !ff->no_read : true)
      , sp(sp)
  {
  }

  bool send_postamble(send_context& sctx, file_index idx) override
  {
    if (sp) { return SendPluginName(sctx, sp, idx.to_underlying(), true); }

    return true;
  }

  bool send_preamble(send_context& sctx, file_index idx) override
  {
    if (sp) { return SendPluginName(sctx, sp, idx.to_underlying(), false); }

    return true;
  }

  bool has_data() override
  {
    if (!hasdata) return false;

    switch ((int)type()) {
      case FT_REGE:
        [[fallthrough]];
      case FT_REG:
        [[fallthrough]];
      case FT_RAW: {
        return true;
      } break;
      default: {
        return false;
      }
    }
    return false;
  }

  data_stream stream() override { return my_stream; }

  BareosFilePacket open() override
  {
    BareosFilePacket bfd;
    binit(&bfd);
    int flag = noatime ? O_NOATIME : 0;
    ASSERT(
        bopen(&bfd, fname.c_str(), O_RDONLY | O_BINARY | flag, 0, lstat().rdev)
        > 0);

    return bfd;
  }

  std::optional<BareosFilePacket> open_rsrc() override
  {
    BareosFilePacket bfd;
    binit(&bfd);
    if (BopenRsrc(&bfd, fname.c_str(), O_RDONLY | O_BINARY, 0) < 0) {
      // TODO: send jmsg if hfsinfo.rsrc_length > 0
      return std::nullopt;
    } else {
      return bfd;
    }
  }

  ~test_file() = default;
};

enum class plugin_object_type : int
{
  TEST
};

struct plugin_object {
  plugin_object_type type() { return {}; }
  int index() { return 0; }
  int length() { return 0; }
  const char* name() { return nullptr; }
  const char* file_name() { return nullptr; }

  span<const char> data() { return {}; }
};

int SavePluginObject(JobControlRecord* jcr, plugin_object obj)
{
  auto& sctx = jcr->fd_impl->send_ctx;

  if (jcr->IsJobCanceled() || jcr->IsIncomplete()) {
    return -1;
    // return save_file_result::Skip;
  }

  file_index fi = next_file_index(jcr);
  sctx->format("%ld %d 0", fi.to_underlying(), STREAM_RESTORE_OBJECT);

  auto data = obj.data();
  int comp_len = data.size();
  int comp = 0;
  const char* obj_data = data.data();
  if (data.size() > 1000) {
    // Big object, compress it
    comp_len = compressBound(data.size());
    POOLMEM* comp_obj = GetMemory(comp_len);
    // FIXME: check Zdeflate error
    Zdeflate(data.data(), data.size(), comp_obj, comp_len);
    if (comp_len < (int)data.size()) {
      obj_data = comp_obj;
      comp = 1; /* zlib level 9 compression */
      Dmsg2(100, "Object compressed from %d to %d bytes\n", data.size(),
            comp_len);
    } else {
      // Uncompressed object smaller, use it
      comp_len = data.size();
      FreePoolMemory(comp_obj);
    }
  }

  POOLMEM* mem = GetPoolMemory(PM_MESSAGE);

  auto message_length
      = Mmsg(mem, "%d %d %d %d %d %d %s%c%s%c", fi.to_underlying(), obj.type(),
             obj.index(), comp_len, obj.length(), comp, obj.file_name(), 0,
             obj.name(), 0);

  mem = CheckPoolMemorySize(mem, message_length + comp_len + 2);
  std::memcpy(mem + message_length, obj_data, comp_len);

  // Note we send one extra byte so Dir can store zero after object
  message_length += comp_len + 1;
  sctx->send(mem, message_length);

  if (comp) {
    // if comp is 1, then obj_data points to compressed object data
    // which was saved in POOLMEM.
    FreePoolMemory((POOLMEM*)obj_data);
  }

  sctx->signal(BNET_EOD);
  return -1;
}

/**
 * Called here by find() for each file included.
 * This is a callback. The original is FindFiles() above.
 *
 * Send the file and its data to the Storage daemon.
 *
 * Returns: 1 if OK
 *          0 if error
 *         -1 to ignore file/directory (not used here) */
int SaveFile(JobControlRecord* jcr, FindFilesPacket* ff_pkt, bool)
{
#if 1
  switch (ff_pkt->type) {
    case FT_DIRBEGIN:
      jcr->fd_impl->num_files_examined--; /* correct file count */
      return 1;                           /* not used */
    case FT_NOFSCHG:
      ff_pkt->type = FT_DIREND; /* Backup only the directory entry */
      break;
    case FT_INVALIDFS:
      ff_pkt->type = FT_DIREND; /* Backup only the directory entry */
      break;
    case FT_SPEC:
      if (S_ISSOCK(ff_pkt->statp.st_mode)) { return 1; }
      break;
    case FT_NOACCESS: {
      jcr->JobErrors++;
      return 1;
    }
    case FT_NOFOLLOW: {
      jcr->JobErrors++;
      return 1;
    }
    case FT_NOSTAT: {
      jcr->JobErrors++;
      return 1;
    }
    case FT_DIRNOCHG:
    case FT_NOCHG:
      return 1;
    case FT_ISARCH:
      return 1;
    case FT_NOOPEN: {
      jcr->JobErrors++;
      return 1;
    }
    case FT_RESTORE_FIRST:
      [[fallthrough]];
    case FT_PLUGIN_CONFIG:
      [[fallthrough]];
    case FT_PLUGIN_CONFIG_FILLED: {
      return SavePluginObject(jcr, {});
    } break;
  }

  std::optional<checksum_type> chk;
  if (BitIsSet(FO_MD5, ff_pkt->flags)) {
    chk = checksum_type::MD5;
  } else if (BitIsSet(FO_SHA1, ff_pkt->flags)) {
    chk = checksum_type::SHA1;
  } else if (BitIsSet(FO_SHA256, ff_pkt->flags)) {
    chk = checksum_type::SHA256;
  } else if (BitIsSet(FO_SHA512, ff_pkt->flags)) {
    chk = checksum_type::SHA512;
  } else if (BitIsSet(FO_XXH128, ff_pkt->flags)) {
    chk = checksum_type::XXH128;
  }

  // todo: there are a lot of incompatible options
  //       we have to take care to take this into consideration.
  //       For example even if pki_session exists, we cannot encrypt
  //       sparse files, but we still need to send the session data.
  //       So we need more than one encrypt option (encrypt and
  //       encrypt_data).
  //       Etc...
  auto opts = save_options{
      .encrypt = jcr->fd_impl->crypto.pki_session,
      .signing_key = jcr->fd_impl->crypto.pki_keypair,
      .checksum = chk,
      .compress = BitIsSet(FO_COMPRESS, ff_pkt->flags),
      .acl = BitIsSet(FO_ACL, ff_pkt->flags),
      .xattr = BitIsSet(FO_XATTR, ff_pkt->flags),
      .discard_empty_blocks = BitIsSet(FO_SPARSE, ff_pkt->flags),
      .insert_file_offsets = BitIsSet(FO_OFFSETS, ff_pkt->flags),
  };

  if (opts.encrypt) { SetBit(FO_ENCRYPT, ff_pkt->flags); }

  // if (BitIsSet(FO_ENCRYPT, ff_pkt->flags)) {
  //   opts.encrypt = jcr->fd_impl->crypto.pki_session;
  //   if (opts.encrypt == nullptr) {
  //     Jmsg(jcr, M_FATAL, 0,
  //          _("Encryption requested but no pki session not set.\n"));
  //     return -1;
  //   }
  // }

#  if 1
  test_file f{ff_pkt, jcr->fd_impl->plugin_sp};

  if (f.stream() == data_stream::NONE) {
    /* This should not happen */
    Jmsg(jcr, M_FATAL, 0,
         _("Invalid file flags, no supported data stream type.\n"));
    return false;
  }

  std::optional<bareos_file_ref> original;
  switch (f.type()) {
    case file_type::JUNCTION:
      [[fallthrough]];
    case file_type::LNK: {
      original.emplace(ff_pkt->link);
    } break;
    case file_type::LNKSAVED: {
      original.emplace(ff_pkt->link, file_index{ff_pkt->LinkFI},
                       (digest_stream)ff_pkt->digest_stream,
                       span{ff_pkt->digest, ff_pkt->digest_len}.as_const());
    } break;
    default: {
    } break;
  }

  auto res = SaveFile(jcr, &f, std::nullopt, std::move(original), opts);
  switch (res) {
    case save_file_result::Error: {
      return 0;
    } break;
    case save_file_result::Success: {
      ff_pkt->FileIndex = jcr->JobFiles;
      return 1;
    } break;
    case save_file_result::Skip: {
      return -1;
    } break;
  };
#  else
  ((submit_context*)jcr->fd_impl->submit_ctx)
      ->submit(std::make_unique<test_file>(ff_pkt), opts);

#  endif

  return 0;
#else
  bool do_read = false;
  bool plugin_started = false;
  bool do_plugin_set = false;
  int status, data_stream;
  int rtnstat = 0;
  b_save_ctx bsctx;
  bool has_file_data = false;
  save_pkt sp; /* use by option plugin */
  BareosSocket* sd = jcr->store_bsock;

  if (jcr->IsJobCanceled() || jcr->IsIncomplete()) { return 0; }

  jcr->fd_impl->num_files_examined++; /* bump total file count */

  switch (ff_pkt->type) {
    case FT_LNKSAVED: /* Hard linked, file already saved */
      Dmsg2(130, "FT_LNKSAVED hard link: %s => %s\n", ff_pkt->fname,
            ff_pkt->link);
      break;
    case FT_REGE:
      Dmsg1(130, "FT_REGE saving: %s\n", ff_pkt->fname);
      has_file_data = true;
      break;
    case FT_REG:
      Dmsg1(130, "FT_REG saving: %s\n", ff_pkt->fname);
      has_file_data = true;
      break;
    case FT_LNK:
      Dmsg2(130, "FT_LNK saving: %s -> %s\n", ff_pkt->fname, ff_pkt->link);
      break;
    case FT_RESTORE_FIRST:
      Dmsg1(100, "FT_RESTORE_FIRST saving: %s\n", ff_pkt->fname);
      break;
    case FT_PLUGIN_CONFIG:
      Dmsg1(100, "FT_PLUGIN_CONFIG saving: %s\n", ff_pkt->fname);
      break;
    case FT_DIRBEGIN:
      jcr->fd_impl->num_files_examined--; /* correct file count */
      return 1;                           /* not used */
    case FT_NORECURSE:
      Jmsg(jcr, M_INFO, 1,
           _("     Recursion turned off. Will not descend from %s into %s\n"),
           ff_pkt->top_fname, ff_pkt->fname);
      ff_pkt->type = FT_DIREND; /* Backup only the directory entry */
      break;
    case FT_NOFSCHG:
      /* Suppress message for /dev filesystems */
      if (!IsInFileset(ff_pkt)) {
        Jmsg(jcr, M_INFO, 1,
             _("     %s is a different filesystem. Will not descend from %s "
               "into it.\n"),
             ff_pkt->fname, ff_pkt->top_fname);
      }
      ff_pkt->type = FT_DIREND; /* Backup only the directory entry */
      break;
    case FT_INVALIDFS:
      Jmsg(jcr, M_INFO, 1,
           _("     Disallowed filesystem. Will not descend from %s into %s\n"),
           ff_pkt->top_fname, ff_pkt->fname);
      ff_pkt->type = FT_DIREND; /* Backup only the directory entry */
      break;
    case FT_INVALIDDT:
      Jmsg(jcr, M_INFO, 1,
           _("     Disallowed drive type. Will not descend into %s\n"),
           ff_pkt->fname);
      break;
    case FT_REPARSE:
    case FT_JUNCTION:
    case FT_DIREND:
      Dmsg1(130, "FT_DIREND: %s\n", ff_pkt->link);
      break;
    case FT_SPEC:
      Dmsg1(130, "FT_SPEC saving: %s\n", ff_pkt->fname);
      if (S_ISSOCK(ff_pkt->statp.st_mode)) {
        Jmsg(jcr, M_SKIPPED, 1, _("     Socket file skipped: %s\n"),
             ff_pkt->fname);
        return 1;
      }
      break;
    case FT_RAW:
      Dmsg1(130, "FT_RAW saving: %s\n", ff_pkt->fname);
      has_file_data = true;
      break;
    case FT_FIFO:
      Dmsg1(130, "FT_FIFO saving: %s\n", ff_pkt->fname);
      break;
    case FT_NOACCESS: {
      BErrNo be;
      Jmsg(jcr, M_NOTSAVED, 0, _("     Could not access \"%s\": ERR=%s\n"),
           ff_pkt->fname, be.bstrerror(ff_pkt->ff_errno));
      jcr->JobErrors++;
      return 1;
    }
    case FT_NOFOLLOW: {
      BErrNo be;
      Jmsg(jcr, M_NOTSAVED, 0, _("     Could not follow link \"%s\": ERR=%s\n"),
           ff_pkt->fname, be.bstrerror(ff_pkt->ff_errno));
      jcr->JobErrors++;
      return 1;
    }
    case FT_NOSTAT: {
      BErrNo be;
      Jmsg(jcr, M_NOTSAVED, 0, _("     Could not stat \"%s\": ERR=%s\n"),
           ff_pkt->fname, be.bstrerror(ff_pkt->ff_errno));
      jcr->JobErrors++;
      return 1;
    }
    case FT_DIRNOCHG:
    case FT_NOCHG:
      Jmsg(jcr, M_SKIPPED, 1, _("     Unchanged file skipped: %s\n"),
           ff_pkt->fname);
      return 1;
    case FT_ISARCH:
      Jmsg(jcr, M_NOTSAVED, 0, _("     Archive file not saved: %s\n"),
           ff_pkt->fname);
      return 1;
    case FT_NOOPEN: {
      BErrNo be;
      Jmsg(jcr, M_NOTSAVED, 0,
           _("     Could not open directory \"%s\": ERR=%s\n"), ff_pkt->fname,
           be.bstrerror(ff_pkt->ff_errno));
      jcr->JobErrors++;
      return 1;
    }
    case FT_DELETED:
      Dmsg1(130, "FT_DELETED: %s\n", ff_pkt->fname);
      break;
    default:
      Jmsg(jcr, M_NOTSAVED, 0, _("     Unknown file type %d; not saved: %s\n"),
           ff_pkt->type, ff_pkt->fname);
      jcr->JobErrors++;
      return 1;
  }

  Dmsg1(130, "filed: sending %s to stored\n", ff_pkt->fname);

  // Setup backup signing context.
  memset(&bsctx, 0, sizeof(b_save_ctx));
  bsctx.digest_stream = STREAM_NONE;
  bsctx.jcr = jcr;
  bsctx.ff_pkt = ff_pkt;

  // Digests and encryption are only useful if there's file data
  if (has_file_data) {
    if (!SetupEncryptionDigests(bsctx)) { goto good_rtn; }
  }

  // Initialize the file descriptor we use for data and other streams.
  binit(&ff_pkt->bfd);
  if (BitIsSet(FO_PORTABLE, ff_pkt->flags)) {
    SetPortableBackup(&ff_pkt->bfd); /* disable Win32 BackupRead() */
  }

  // Option and cmd plugin are not compatible together
  if (ff_pkt->cmd_plugin) {
    do_plugin_set = true;
  } else if (ff_pkt->opt_plugin) {
    // Ask the option plugin what to do with this file
    switch (PluginOptionHandleFile(jcr, ff_pkt, &sp)) {
      case bRC_OK:
        Dmsg2(10, "Option plugin %s will be used to backup %s\n",
              ff_pkt->plugin, ff_pkt->fname);
        jcr->opt_plugin = true;
        jcr->fd_impl->plugin_sp = &sp;
        PluginUpdateFfPkt(ff_pkt, &sp);
        do_plugin_set = true;
        break;
      case bRC_Skip:
        Dmsg2(10, "Option plugin %s decided to skip %s\n", ff_pkt->plugin,
              ff_pkt->fname);
        goto good_rtn;
      case bRC_Core:
        Dmsg2(10, "Option plugin %s decided to let bareos handle %s\n",
              ff_pkt->plugin, ff_pkt->fname);
        break;
      default:
        goto bail_out;
    }
  }

  if (do_plugin_set) {
    // Tell bfile that it needs to call plugin
    if (!SetCmdPlugin(&ff_pkt->bfd, jcr)) { goto bail_out; }
    SendPluginName(jcr, sd, true); /* signal start of plugin data */
    plugin_started = true;
  }

  // Send attributes -- must be done after binit()
  if (!EncodeAndSendAttributes(jcr, ff_pkt, data_stream)) { goto bail_out; }

  // Meta data only for restore object
  if (IS_FT_OBJECT(ff_pkt->type)) { goto good_rtn; }

  // Meta data only for deleted files
  if (ff_pkt->type == FT_DELETED) { goto good_rtn; }

  // Set up the encryption context and send the session data to the SD
  if (has_file_data && jcr->fd_impl->crypto.pki_encrypt) {
    if (!CryptoSessionSend(jcr, sd)) { goto bail_out; }
  }

  /* For a command plugin use the setting from the plugins savepkt no_read field
   * which is saved in the ff_pkt->no_read variable. do_read is the inverted
   * value of this variable as no_read == TRUE means do_read == FALSE */
  if (ff_pkt->cmd_plugin) {
    do_read = !ff_pkt->no_read;
  } else {
    /* Open any file with data that we intend to save, then save it.
     *
     * Note, if is_win32_backup, we must open the Directory so that
     * the BackupRead will save its permissions and ownership streams. */
    if (ff_pkt->type != FT_LNKSAVED && S_ISREG(ff_pkt->statp.st_mode)) {
#  ifdef HAVE_WIN32
      do_read = !IsPortableBackup(&ff_pkt->bfd) || ff_pkt->statp.st_size > 0;
#  else
      do_read = ff_pkt->statp.st_size > 0;
#  endif
    } else if (ff_pkt->type == FT_RAW || ff_pkt->type == FT_FIFO
               || ff_pkt->type == FT_REPARSE || ff_pkt->type == FT_JUNCTION
               || (!IsPortableBackup(&ff_pkt->bfd)
                   && ff_pkt->type == FT_DIREND)) {
      do_read = true;
    }
  }

  Dmsg2(150, "type=%d do_read=%d\n", ff_pkt->type, do_read);
  if (do_read) {
    btimer_t* tid;
    int noatime;

    if (ff_pkt->type == FT_FIFO) {
      tid = start_thread_timer(jcr, pthread_self(), 60);
    } else {
      tid = NULL;
    }

    noatime = BitIsSet(FO_NOATIME, ff_pkt->flags) ? O_NOATIME : 0;
    ff_pkt->bfd.reparse_point
        = (ff_pkt->type == FT_REPARSE || ff_pkt->type == FT_JUNCTION);

    if (bopen(&ff_pkt->bfd, ff_pkt->fname, O_RDONLY | O_BINARY | noatime, 0,
              ff_pkt->statp.st_rdev)
        < 0) {
      ff_pkt->ff_errno = errno;
      BErrNo be;
      Jmsg(jcr, M_NOTSAVED, 0, _("     Cannot open \"%s\": ERR=%s.\n"),
           ff_pkt->fname, be.bstrerror());
      jcr->JobErrors++;
      if (tid) {
        StopThreadTimer(tid);
        tid = NULL;
      }
      goto good_rtn;
    }

    if (tid) {
      StopThreadTimer(tid);
      tid = NULL;
    }

    status = send_data(jcr, data_stream, ff_pkt, bsctx.digest,
                       bsctx.signing_digest);

    if (BitIsSet(FO_CHKCHANGES, ff_pkt->flags)) { HasFileChanged(jcr, ff_pkt); }

    bclose(&ff_pkt->bfd);

    if (!status) { goto bail_out; }
  }

  if (have_darwin_os) {
    // Regular files can have resource forks and Finder Info
    if (ff_pkt->type != FT_LNKSAVED
        && (S_ISREG(ff_pkt->statp.st_mode)
            && BitIsSet(FO_HFSPLUS, ff_pkt->flags))) {
      if (!SaveRsrcAndFinder(bsctx)) { goto bail_out; }
    }
  }

  // Save ACLs when requested and available for anything not being a symlink.
  if (have_acl) {
    if (BitIsSet(FO_ACL, ff_pkt->flags) && ff_pkt->type != FT_LNK) {
      if (!DoBackupAcl(jcr, ff_pkt)) { goto bail_out; }
    }
  }

  // Save Extended Attributes when requested and available for all files.
  if (have_xattr) {
    if (BitIsSet(FO_XATTR, ff_pkt->flags)) {
      if (!DoBackupXattr(jcr, ff_pkt)) { goto bail_out; }
    }
  }

  // Terminate the signing digest and send it to the Storage daemon
  if (bsctx.signing_digest) {
    if (!TerminateSigningDigest(bsctx)) { goto bail_out; }
  }

  // Terminate any digest and send it to Storage daemon
  if (bsctx.digest) {
    if (!TerminateDigest(bsctx)) { goto bail_out; }
  }

  // Check if original file has a digest, and send it
  if (ff_pkt->type == FT_LNKSAVED && ff_pkt->digest) {
    Dmsg2(300, "Link %s digest %d\n", ff_pkt->fname, ff_pkt->digest_len);
    sd->fsend("%ld %d 0", jcr->JobFiles, ff_pkt->digest_stream);

    sd->msg = CheckPoolMemorySize(sd->msg, ff_pkt->digest_len);
    memcpy(sd->msg, ff_pkt->digest, ff_pkt->digest_len);
    sd->message_length = ff_pkt->digest_len;
    sd->send();

    sd->signal(BNET_EOD); /* end of hardlink record */
  }

good_rtn:
  rtnstat = jcr->IsJobCanceled() ? 0 : 1; /* good return if not canceled */

bail_out:
  if (jcr->IsIncomplete() || jcr->IsJobCanceled()) { rtnstat = 0; }
  if (plugin_started) {
    SendPluginName(jcr, sd, false); /* signal end of plugin data */
  }
  if (ff_pkt->opt_plugin) {
    jcr->fd_impl->plugin_sp = NULL; /* sp is local to this function */
    jcr->opt_plugin = false;
  }
  if (bsctx.digest) { CryptoDigestFree(bsctx.digest); }
  if (bsctx.signing_digest) { CryptoDigestFree(bsctx.signing_digest); }

  return rtnstat;
#endif
}

#if 0
/**
 * Handle the data just read and send it to the SD after doing any
 * postprocessing needed.
 */
static inline bool SendDataToSd(b_ctx* bctx)
{
  BareosSocket* sd = bctx->jcr->store_bsock;
  bool need_more_data;

  // Check for sparse blocks
  if (BitIsSet(FO_SPARSE, bctx->ff_pkt->flags)) {
    bool allZeros;
    ser_declare;

    allZeros = false;
    if ((sd->message_length == bctx->rsize
         && (bctx->fileAddr + sd->message_length
             < (uint64_t)bctx->ff_pkt->statp.st_size))
        || ((bctx->ff_pkt->type == FT_RAW || bctx->ff_pkt->type == FT_FIFO)
            && ((uint64_t)bctx->ff_pkt->statp.st_size == 0))) {
      allZeros = IsBufZero(bctx->rbuf, bctx->rsize);
    }

    if (!allZeros) {
      // Put file address as first data in buffer
      SerBegin(bctx->wbuf, OFFSET_FADDR_SIZE);
      ser_uint64(bctx->fileAddr); /* store fileAddr in begin of buffer */
    }

    bctx->fileAddr += sd->message_length; /* update file address */

    // Skip block of all zeros
    if (allZeros) { return true; }
  } else if (BitIsSet(FO_OFFSETS, bctx->ff_pkt->flags)) {
    ser_declare;
    SerBegin(bctx->wbuf, OFFSET_FADDR_SIZE);
    ser_uint64(bctx->ff_pkt->bfd.offset); /* store offset in begin of buffer */
    SerEnd(bctx->wbuf, OFFSET_FADDR_SIZE);
  }

  bctx->jcr->ReadBytes += sd->message_length; /* count bytes read */

  // Uncompressed cipher input length
  bctx->cipher_input_len = sd->message_length;

  // Update checksum if requested
  if (bctx->digest) {
    CryptoDigestUpdate(bctx->digest, (uint8_t*)bctx->rbuf, sd->message_length);
  }

  // Update signing digest if requested
  if (bctx->signing_digest) {
    CryptoDigestUpdate(bctx->signing_digest, (uint8_t*)bctx->rbuf,
                       sd->message_length);
  }

  // Compress the data.
  if (BitIsSet(FO_COMPRESS, bctx->ff_pkt->flags)) {
    if (!CompressData(bctx->jcr, bctx->ff_pkt->Compress_algo, bctx->rbuf,
                      bctx->jcr->store_bsock->message_length, bctx->cbuf,
                      bctx->max_compress_len, &bctx->compress_len)) {
      return false;
    }

    // See if we need to generate a compression header.
    if (bctx->chead) {
      ser_declare;

      // Complete header
      SerBegin(bctx->chead, sizeof(comp_stream_header));
      ser_uint32(bctx->ch.magic);
      ser_uint32(bctx->compress_len);
      ser_uint16(bctx->ch.level);
      ser_uint16(bctx->ch.version);
      SerEnd(bctx->chead, sizeof(comp_stream_header));

      bctx->compress_len += sizeof(comp_stream_header); /* add size of header */
    }

    bctx->jcr->store_bsock->message_length
        = bctx->compress_len; /* set compressed length */
    bctx->cipher_input_len = bctx->compress_len;
  }

  // Encrypt the data.
  need_more_data = false;
  if (BitIsSet(FO_ENCRYPT, bctx->ff_pkt->flags)
      && !EncryptData(bctx, &need_more_data)) {
    if (need_more_data) { return true; }
    return false;
  }

  // Send the buffer to the Storage daemon
  if (BitIsSet(FO_SPARSE, bctx->ff_pkt->flags)
      || BitIsSet(FO_OFFSETS, bctx->ff_pkt->flags)) {
    sd->message_length += OFFSET_FADDR_SIZE; /* include fileAddr in size */
  }
  sd->msg = bctx->wbuf; /* set correct write buffer */

  if (!sd->send()) {
    if (!bctx->jcr->IsJobCanceled()) {
      Jmsg1(bctx->jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
    }
    return false;
  }

  Dmsg1(130, "Send data to SD len=%d\n", sd->message_length);
  bctx->jcr->JobBytes += sd->message_length; /* count bytes saved possibly
                                                compressed/encrypted */
  sd->msg = bctx->msgsave;                   /* restore read buffer */

  return true;
}

#endif

#ifdef HAVE_WIN32
// Callback method for ReadEncryptedFileRaw()
static DWORD WINAPI send_efs_data(PBYTE pbData,
                                  PVOID pvCallbackContext,
                                  ULONG ulLength)
{
  b_ctx* bctx = (b_ctx*)pvCallbackContext;
  BareosSocket* sd = bctx->jcr->store_bsock;

  if (ulLength == 0) { return ERROR_SUCCESS; }

  /* See if we can fit the data into the current bctx->rbuf which can hold
   * bctx->rsize bytes. */
  if (ulLength <= (ULONG)bctx->rsize) {
    sd->message_length = ulLength;
    memcpy(bctx->rbuf, pbData, ulLength);
    if (!SendDataToSd(bctx)) { return ERROR_NET_WRITE_FAULT; }
  } else {
    // Need to chunk the data into pieces.
    ULONG offset = 0;

    while (ulLength > 0) {
      sd->message_length = MIN((ULONG)bctx->rsize, ulLength);
      memcpy(bctx->rbuf, pbData + offset, sd->message_length);
      if (!SendDataToSd(bctx)) { return ERROR_NET_WRITE_FAULT; }

      offset += sd->message_length;
      ulLength -= sd->message_length;
    }
  }

  return ERROR_SUCCESS;
}

// Send the content of an Encrypted file on an EFS filesystem.
static inline bool SendEncryptedData(b_ctx& bctx)
{
  bool retval = false;

  if (!p_ReadEncryptedFileRaw) {
    Jmsg0(bctx.jcr, M_FATAL, 0,
          _("Encrypted file but no EFS support functions\n"));
  }

  /* The EFS read function, ReadEncryptedFileRaw(), works in a specific way.
   * You have to give it a function that it calls repeatedly every time the
   * read buffer is filled.
   *
   * So ReadEncryptedFileRaw() will not return until it has read the whole file.
   */
  if (p_ReadEncryptedFileRaw((PFE_EXPORT_FUNC)send_efs_data, &bctx,
                             bctx.ff_pkt->bfd.pvContext)) {
    goto bail_out;
  }
  retval = true;

bail_out:
  return retval;
}
#endif

#if 0
// Send the content of a file on anything but an EFS filesystem.
static inline bool SendPlainData(b_ctx& bctx)
{
  bool retval = false;
  BareosSocket* sd = bctx.jcr->store_bsock;

  // Read the file data
  while ((sd->message_length
          = (uint32_t)bread(&bctx.ff_pkt->bfd, bctx.rbuf, bctx.rsize))
         > 0) {
    if (!SendDataToSd(&bctx)) { goto bail_out; }
  }
  retval = true;

bail_out:
  return retval;
}
#endif

#if 0
/**
 * Send data read from an already open file descriptor.
 *
 * We return 1 on sucess and 0 on errors.
 *
 * ***FIXME***
 * We use ff_pkt->statp.st_size when FO_SPARSE to know when to stop reading.
 * Currently this is not a problem as the only other stream, resource forks,
 * are not handled as sparse files.
 */
static int send_data(JobControlRecord* jcr,
                     int stream,
                     FindFilesPacket* ff_pkt,
                     DIGEST* digest,
                     DIGEST* signing_digest)
{
  b_ctx bctx;
  BareosSocket* sd = jcr->store_bsock;
#  ifdef FD_NO_SEND_TEST
  return 1;
#  endif

  // Setup backup context.
  memset(&bctx, 0, sizeof(b_ctx));
  bctx.jcr = jcr;
  bctx.ff_pkt = ff_pkt;
  bctx.msgsave = sd->msg;                  /* save the original sd buffer */
  bctx.rbuf = sd->msg;                     /* read buffer */
  bctx.wbuf = sd->msg;                     /* write buffer */
  bctx.rsize = jcr->buf_size;              /* read buffer size */
  bctx.cipher_input = (uint8_t*)bctx.rbuf; /* encrypt uncompressed data */
  bctx.digest = digest;                    /* encryption digest */
  bctx.signing_digest = signing_digest;    /* signing digest */

  Dmsg1(300, "Saving data, type=%d\n", ff_pkt->type);

  if (!SetupCompressionContext(bctx)) { goto bail_out; }

  if (!SetupEncryptionContext(bctx)) { goto bail_out; }

  /* Send Data header to Storage daemon
   *    <file-index> <stream> <info> */
  if (!sd->fsend("%ld %d 0", jcr->JobFiles, stream)) {
    if (!jcr->IsJobCanceled()) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
    }
    goto bail_out;
  }
  Dmsg1(300, ">stored: datahdr %s", sd->msg);

  /* Make space at beginning of buffer for fileAddr because this
   *   same buffer will be used for writing if compression is off. */
  if (BitIsSet(FO_SPARSE, ff_pkt->flags)
      || BitIsSet(FO_OFFSETS, ff_pkt->flags)) {
    bctx.rbuf += OFFSET_FADDR_SIZE;
    bctx.rsize -= OFFSET_FADDR_SIZE;
#  ifdef HAVE_FREEBSD_OS
    // To read FreeBSD partitions, the read size must be a multiple of 512.
    bctx.rsize = (bctx.rsize / 512) * 512;
#  endif
  }

  // A RAW device read on win32 only works if the buffer is a multiple of 512
#  ifdef HAVE_WIN32
  if (S_ISBLK(ff_pkt->statp.st_mode)) { bctx.rsize = (bctx.rsize / 512) * 512; }

  if (ff_pkt->statp.st_rdev & FILE_ATTRIBUTE_ENCRYPTED) {
    if (!SendEncryptedData(bctx)) { goto bail_out; }
  } else {
    if (!SendPlainData(bctx)) { goto bail_out; }
  }
#  else
  if (!SendPlainData(bctx)) { goto bail_out; }
#  endif

  if (sd->message_length < 0) { /* error */
    BErrNo be;
    Jmsg(jcr, M_ERROR, 0, _("Read error on file %s. ERR=%s\n"), ff_pkt->fname,
         be.bstrerror(ff_pkt->bfd.BErrNo));
    if (jcr->JobErrors++ > 1000) { /* insanity check */
      Jmsg(jcr, M_FATAL, 0, _("Too many errors. JobErrors=%d.\n"),
           jcr->JobErrors);
    }
  } else if (BitIsSet(FO_ENCRYPT, ff_pkt->flags)) {
    // For encryption, we must call finalize to push out any buffered data.
    if (!CryptoCipherFinalize(bctx.cipher_ctx,
                              (uint8_t*)jcr->fd_impl->crypto.crypto_buf,
                              &bctx.encrypted_len)) {
      // Padding failed. Shouldn't happen.
      Jmsg(jcr, M_FATAL, 0, _("Encryption padding error\n"));
      goto bail_out;
    }

    // Note, on SSL pre-0.9.7, there is always some output
    if (bctx.encrypted_len > 0) {
      sd->message_length = bctx.encrypted_len;   /* set encrypted length */
      sd->msg = jcr->fd_impl->crypto.crypto_buf; /* set correct write buffer */
      if (!sd->send()) {
        if (!jcr->IsJobCanceled()) {
          Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
                sd->bstrerror());
        }
        goto bail_out;
      }
      Dmsg1(130, "Send data to SD len=%d\n", sd->message_length);
      jcr->JobBytes += sd->message_length; /* count bytes saved possibly
                                              compressed/encrypted */
      sd->msg = bctx.msgsave;              /* restore bnet buffer */
    }
  }

  if (!sd->signal(BNET_EOD)) { /* indicate end of file data */
    if (!jcr->IsJobCanceled()) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
    }
    goto bail_out;
  }

  // Free the cipher context
  if (bctx.cipher_ctx) { CryptoCipherFree(bctx.cipher_ctx); }

  return 1;

bail_out:
  // Free the cipher context
  if (bctx.cipher_ctx) { CryptoCipherFree(bctx.cipher_ctx); }

  sd->msg = bctx.msgsave; /* restore bnet buffer */
  sd->message_length = 0;

  return 0;
}
#endif

bool EncodeAndSendAttributes(JobControlRecord* jcr,
                             FindFilesPacket* ff_pkt,
                             int& data_stream)
{
  BareosSocket* sd = jcr->store_bsock;
  PoolMem attribs(PM_NAME), attribsExBuf(PM_NAME);
  char* attribsEx = NULL;
  int attr_stream;
  int comp_len;
  bool status;
  int hangup = GetHangup();
#ifdef FD_NO_SEND_TEST
  return true;
#endif

  Dmsg1(300, "encode_and_send_attrs fname=%s\n", ff_pkt->fname);
  /** Find what data stream we will use, then encode the attributes */
  if ((data_stream = SelectDataStream(ff_pkt)) == STREAM_NONE) {
    /* This should not happen */
    Jmsg0(jcr, M_FATAL, 0,
          _("Invalid file flags, no supported data stream type.\n"));
    return false;
  }
  EncodeStat(attribs.c_str(), &ff_pkt->statp, sizeof(ff_pkt->statp),
             ff_pkt->LinkFI, data_stream);

  /** Now possibly extend the attributes */
  if (IS_FT_OBJECT(ff_pkt->type)) {
    attr_stream = STREAM_RESTORE_OBJECT;
  } else {
    attribsEx = attribsExBuf.c_str();
    attr_stream = encode_attribsEx(jcr, attribsEx, ff_pkt);
  }

  Dmsg3(300, "File %s\nattribs=%s\nattribsEx=%s\n", ff_pkt->fname,
        attribs.c_str(), attribsEx);

  jcr->lock();
  jcr->JobFiles++;                   /* increment number of files sent */
  ff_pkt->FileIndex = jcr->JobFiles; /* return FileIndex */
  PmStrcpy(jcr->fd_impl->last_fname, ff_pkt->fname);
  jcr->unlock();

  // Debug code: check if we must hangup
  if (hangup && (jcr->JobFiles > (uint32_t)hangup)) {
    jcr->setJobStatusWithPriorityCheck(JS_Incomplete);
    Jmsg1(jcr, M_FATAL, 0, "Debug hangup requested after %d files.\n", hangup);
    SetHangup(0);
    return false;
  }

  /* Send Attributes header to Storage daemon
   *    <file-index> <stream> <info> */
  if (!sd->fsend("%ld %d 0", jcr->JobFiles, attr_stream)) {
    if (!jcr->IsJobCanceled() && !jcr->IsIncomplete()) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
    }
    return false;
  }
  Dmsg1(300, ">stored: attrhdr %s", sd->msg);

  /* Send file attributes to Storage daemon
   *   File_index
   *   File type
   *   Filename (full path)
   *   Encoded attributes
   *   Link name (if type==FT_LNK or FT_LNKSAVED)
   *   Encoded extended-attributes (for Win32)
   *   Delta Sequence Number
   *
   * or send Restore Object to Storage daemon
   *   File_index
   *   File_type
   *   Object_index
   *   Object_len  (possibly compressed)
   *   Object_full_len (not compressed)
   *   Object_compression
   *   Plugin_name
   *   Object_name
   *   Binary Object data
   *
   * For a directory, link is the same as fname, but with trailing
   * slash. For a linked file, link is the link. */
  if (!IS_FT_OBJECT(ff_pkt->type)
      && ff_pkt->type != FT_DELETED) { /* already stripped */
    StripPath(ff_pkt);
  }
  switch (ff_pkt->type) {
    case FT_JUNCTION:
    case FT_LNK:
    case FT_LNKSAVED:
      Dmsg3(300, "Link %d %s to %s\n", jcr->JobFiles, ff_pkt->fname,
            ff_pkt->link);
      status = sd->fsend("%ld %d %s%c%s%c%s%c%s%c%u%c", jcr->JobFiles,
                         ff_pkt->type, ff_pkt->fname, 0, attribs.c_str(), 0,
                         ff_pkt->link, 0, attribsEx, 0, ff_pkt->delta_seq, 0);
      break;
    case FT_DIREND:
    case FT_REPARSE:
      /* Here link is the canonical filename (i.e. with trailing slash) */
      status = sd->fsend("%ld %d %s%c%s%c%c%s%c%u%c", jcr->JobFiles,
                         ff_pkt->type, ff_pkt->link, 0, attribs.c_str(), 0, 0,
                         attribsEx, 0, ff_pkt->delta_seq, 0);
      break;
    case FT_PLUGIN_CONFIG:
    case FT_RESTORE_FIRST:
      comp_len = ff_pkt->object_len;
      ff_pkt->object_compression = 0;

      if (ff_pkt->object_len > 1000) {
        // Big object, compress it
        comp_len = compressBound(ff_pkt->object_len);
        POOLMEM* comp_obj = GetMemory(comp_len);
        // FIXME: check Zdeflate error
        Zdeflate(ff_pkt->object, ff_pkt->object_len, comp_obj, comp_len);
        if (comp_len < ff_pkt->object_len) {
          ff_pkt->object = comp_obj;
          ff_pkt->object_compression = 1; /* zlib level 9 compression */
        } else {
          // Uncompressed object smaller, use it
          comp_len = ff_pkt->object_len;
        }
        Dmsg2(100, "Object compressed from %d to %d bytes\n",
              ff_pkt->object_len, comp_len);
      }

      sd->message_length = Mmsg(
          sd->msg, "%d %d %d %d %d %d %s%c%s%c", jcr->JobFiles, ff_pkt->type,
          ff_pkt->object_index, comp_len, ff_pkt->object_len,
          ff_pkt->object_compression, ff_pkt->fname, 0, ff_pkt->object_name, 0);
      sd->msg = CheckPoolMemorySize(sd->msg, sd->message_length + comp_len + 2);
      memcpy(sd->msg + sd->message_length, ff_pkt->object, comp_len);

      // Note we send one extra byte so Dir can store zero after object
      sd->message_length += comp_len + 1;
      status = sd->send();
      if (ff_pkt->object_compression) { FreeAndNullPoolMemory(ff_pkt->object); }
      break;
    case FT_REG:
      status = sd->fsend("%ld %d %s%c%s%c%c%s%c%d%c", jcr->JobFiles,
                         ff_pkt->type, ff_pkt->fname, 0, attribs.c_str(), 0, 0,
                         attribsEx, 0, ff_pkt->delta_seq, 0);
      break;
    default:
      status = sd->fsend("%ld %d %s%c%s%c%c%s%c%u%c", jcr->JobFiles,
                         ff_pkt->type, ff_pkt->fname, 0, attribs.c_str(), 0, 0,
                         attribsEx, 0, ff_pkt->delta_seq, 0);
      break;
  }

  if (!IS_FT_OBJECT(ff_pkt->type) && ff_pkt->type != FT_DELETED) {
    UnstripPath(ff_pkt);
  }

  Dmsg2(300, ">stored: attr len=%d: %s\n", sd->message_length, sd->msg);
  if (!status && !jcr->IsJobCanceled()) {
    Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
          sd->bstrerror());
  }

  sd->signal(BNET_EOD); /* indicate end of attributes data */

  return status;
}

// Do in place strip of path
static bool do_strip(int count, char* in)
{
  char* out = in;
  int stripped;
  int numsep = 0;

  // Copy to first path separator -- Win32 might have c: ...
  while (*in && !IsPathSeparator(*in)) {
    out++;
    in++;
  }
  if (*in) { /* Not at the end of the string */
    out++;
    in++;
    numsep++; /* one separator seen */
  }
  for (stripped = 0; stripped < count && *in; stripped++) {
    while (*in && !IsPathSeparator(*in)) { in++; /* skip chars */ }
    if (*in) {
      numsep++; /* count separators seen */
      in++;     /* skip separator */
    }
  }

  // Copy to end
  while (*in) { /* copy to end */
    if (IsPathSeparator(*in)) { numsep++; }
    *out++ = *in++;
  }
  *out = 0;
  Dmsg4(500, "stripped=%d count=%d numsep=%d sep>count=%d\n", stripped, count,
        numsep, numsep > count);
  return stripped == count && numsep > count;
}

/**
 * If requested strip leading components of the path so that we can
 * save file as if it came from a subdirectory.  This is most useful
 * for dealing with snapshots, by removing the snapshot directory, or
 * in handling vendor migrations where files have been restored with
 * a vendor product into a subdirectory.
 */
void StripPath(FindFilesPacket* ff_pkt)
{
  if (!BitIsSet(FO_STRIPPATH, ff_pkt->flags) || ff_pkt->StripPath <= 0) {
    Dmsg1(200, "No strip for %s\n", ff_pkt->fname);
    return;
  }

  if (!ff_pkt->fname_save) {
    ff_pkt->fname_save = GetPoolMemory(PM_FNAME);
    ff_pkt->link_save = GetPoolMemory(PM_FNAME);
  }

  PmStrcpy(ff_pkt->fname_save, ff_pkt->fname);
  if (ff_pkt->type != FT_LNK && ff_pkt->fname != ff_pkt->link) {
    PmStrcpy(ff_pkt->link_save, ff_pkt->link);
    Dmsg2(500, "strcpy link_save=%d link=%d\n", strlen(ff_pkt->link_save),
          strlen(ff_pkt->link));
  }

  /* Strip path. If it doesn't succeed put it back. If it does, and there
   * is a different link string, attempt to strip the link. If it fails,
   * back them both back. Do not strip symlinks. I.e. if either stripping
   * fails don't strip anything. */
  if (!do_strip(ff_pkt->StripPath, ff_pkt->fname)) {
    UnstripPath(ff_pkt);
    goto rtn;
  }

  // Strip links but not symlinks
  if (ff_pkt->type != FT_LNK && ff_pkt->fname != ff_pkt->link) {
    if (!do_strip(ff_pkt->StripPath, ff_pkt->link)) { UnstripPath(ff_pkt); }
  }

rtn:
  Dmsg3(100, "fname=%s stripped=%s link=%s\n", ff_pkt->fname_save,
        ff_pkt->fname, ff_pkt->link);
}

void UnstripPath(FindFilesPacket* ff_pkt)
{
  if (!BitIsSet(FO_STRIPPATH, ff_pkt->flags) || ff_pkt->StripPath <= 0) {
    return;
  }

  strcpy(ff_pkt->fname, ff_pkt->fname_save);
  if (ff_pkt->type != FT_LNK && ff_pkt->fname != ff_pkt->link) {
    Dmsg2(500, "strcpy link=%s link_save=%s\n", ff_pkt->link,
          ff_pkt->link_save);
    strcpy(ff_pkt->link, ff_pkt->link_save);
    Dmsg2(500, "strcpy link=%d link_save=%d\n", strlen(ff_pkt->link),
          strlen(ff_pkt->link_save));
  }
}

#if defined(WIN32_VSS)
static void CloseVssBackupSession(JobControlRecord* jcr)
{
  /* STOP VSS ON WIN32
   * Tell vss to close the backup session */
  if (jcr->fd_impl->pVSSClient) {
    /* We are about to call the BackupComplete VSS method so let all plugins
     * know that by raising the bEventVssBackupComplete event. */
    GeneratePluginEvent(jcr, bEventVssBackupComplete);
    if (jcr->fd_impl->pVSSClient->CloseBackup()) {
      // Inform user about writer states
      for (size_t i = 0; i < jcr->fd_impl->pVSSClient->GetWriterCount(); i++) {
        int msg_type = M_INFO;
        if (jcr->fd_impl->pVSSClient->GetWriterState(i) < 1) {
          msg_type = M_WARNING;
          jcr->JobErrors++;
        }
        Jmsg(jcr, msg_type, 0, _("VSS Writer (BackupComplete): %s\n"),
             jcr->fd_impl->pVSSClient->GetWriterInfo(i));
      }
    }

    // Generate Job global writer metadata
    wchar_t* metadata = jcr->fd_impl->pVSSClient->GetMetadata();
    if (metadata) {
      FindFilesPacket* ff_pkt = jcr->fd_impl->ff;
      ff_pkt->fname = (char*)"*all*"; /* for all plugins */
      ff_pkt->type = FT_RESTORE_FIRST;
      ff_pkt->LinkFI = 0;
      ff_pkt->object_name = (char*)"job_metadata.xml";
      ff_pkt->object = BSTR_2_str(metadata);
      ff_pkt->object_len = (wcslen(metadata) + 1) * sizeof(wchar_t);
      ff_pkt->object_index = (int)time(NULL);
      SaveFile(jcr, ff_pkt, true);
    }
  }
}
#endif
} /* namespace filedaemon */
