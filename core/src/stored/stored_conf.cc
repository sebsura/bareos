/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

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
 * Configuration file parser for Bareos Storage daemon
 */

#include "include/bareos.h"
#include "stored/stored_conf.h"
#include "stored/autochanger_resource.h"
#include "stored/device_resource.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/sd_backends.h"
#include "lib/address_conf.h"
#include "lib/bareos_resource.h"
#include "lib/berrno.h"
#include "lib/messages_resource.h"
#include "lib/resource_item.h"
#include "lib/parse_conf.h"
#include "lib/tls_resource_items.h"
#define NEED_JANSSON_NAMESPACE 1
#include "lib/output_formatter.h"
#include "lib/output_formatter_resource.h"
#include "lib/implementation_factory.h"
#include "lib/version.h"
#include "include/auth_types.h"
#include "include/jcr.h"

namespace storagedaemon {

static void FreeResource(BareosResource* sres, int type);
static void DumpResource(int type,
                         BareosResource* reshdr,
                         bool sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose);

#include "lib/messages_resource_items.h"

/* clang-format off */

static ResourceItem store_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(StorageResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(StorageResource, description_), 0, 0, NULL, NULL, NULL},
  {"SdPort", CFG_TYPE_ADDRESSES_PORT, ITEM(StorageResource, SDaddrs), 0, CFG_ITEM_DEFAULT, SD_DEFAULT_PORT, NULL, NULL},
  {"SdAddress", CFG_TYPE_ADDRESSES_ADDRESS, ITEM(StorageResource, SDaddrs), 0, CFG_ITEM_DEFAULT, SD_DEFAULT_PORT, NULL, NULL},
  {"SdAddresses", CFG_TYPE_ADDRESSES, ITEM(StorageResource, SDaddrs), 0, CFG_ITEM_DEFAULT, SD_DEFAULT_PORT, NULL, NULL},
  {"SdSourceAddress", CFG_TYPE_ADDRESSES_ADDRESS, ITEM(StorageResource, SDsrc_addr), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL},
  {"WorkingDirectory", CFG_TYPE_DIR, ITEM(StorageResource, working_directory), 0,
      CFG_ITEM_DEFAULT | CFG_ITEM_PLATFORM_SPECIFIC, PATH_BAREOS_WORKINGDIR, NULL, NULL},
#if defined(HAVE_DYNAMIC_SD_BACKENDS)
  {"BackendDirectory", CFG_TYPE_STR_VECTOR_OF_DIRS, ITEM(StorageResource, backend_directories), 0,
      CFG_ITEM_DEFAULT | CFG_ITEM_PLATFORM_SPECIFIC, PATH_BAREOS_BACKENDDIR, NULL, NULL},
#endif
  {"PluginDirectory", CFG_TYPE_DIR, ITEM(StorageResource, plugin_directory), 0, 0, NULL, NULL, NULL},
  {"PluginNames", CFG_TYPE_PLUGIN_NAMES, ITEM(StorageResource, plugin_names), 0, 0, NULL, NULL, NULL},
  {"ScriptsDirectory", CFG_TYPE_DIR, ITEM(StorageResource, scripts_directory), 0, 0, NULL, NULL, NULL},
  {"MaximumConcurrentJobs", CFG_TYPE_PINT32, ITEM(StorageResource, MaxConcurrentJobs), 0, CFG_ITEM_DEFAULT, "20", NULL, NULL},
  {"Messages", CFG_TYPE_RES, ITEM(StorageResource, messages), R_MSGS, 0, NULL, NULL, NULL},
  {"SdConnectTimeout", CFG_TYPE_TIME, ITEM(StorageResource, SDConnectTimeout), 0, CFG_ITEM_DEFAULT, "1800" /* 30 minutes */, NULL, NULL},
  {"FdConnectTimeout", CFG_TYPE_TIME, ITEM(StorageResource, FDConnectTimeout), 0, CFG_ITEM_DEFAULT, "1800" /* 30 minutes */, NULL, NULL},
  {"HeartbeatInterval", CFG_TYPE_TIME, ITEM(StorageResource, heartbeat_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL},
  {"CheckpointInterval", CFG_TYPE_TIME, ITEM(StorageResource, checkpoint_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL},
  {"MaximumNetworkBufferSize", CFG_TYPE_PINT32, ITEM(StorageResource, max_network_buffer_size), 0, 0, NULL, NULL, NULL},
  {"ClientConnectWait", CFG_TYPE_TIME, ITEM(StorageResource, client_wait), 0, CFG_ITEM_DEFAULT, "1800" /* 30 minutes */, NULL, NULL},
  {"VerId", CFG_TYPE_STR, ITEM(StorageResource, verid), 0, 0, NULL, NULL, NULL},
  {"MaximumBandwidthPerJob", CFG_TYPE_SPEED, ITEM(StorageResource, max_bandwidth_per_job), 0, 0, NULL, NULL, NULL},
  {"AllowBandwidthBursting", CFG_TYPE_BOOL, ITEM(StorageResource, allow_bw_bursting), 0, CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"NdmpEnable", CFG_TYPE_BOOL, ITEM(StorageResource, ndmp_enable), 0, CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"NdmpSnooping", CFG_TYPE_BOOL, ITEM(StorageResource, ndmp_snooping), 0, CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"NdmpLogLevel", CFG_TYPE_PINT32, ITEM(StorageResource, ndmploglevel), 0, CFG_ITEM_DEFAULT, "4", NULL, NULL},
  {"NdmpAddress", CFG_TYPE_ADDRESSES_ADDRESS, ITEM(StorageResource, NDMPaddrs), 0, CFG_ITEM_DEFAULT, "10000", NULL, NULL},
  {"NdmpAddresses", CFG_TYPE_ADDRESSES, ITEM(StorageResource, NDMPaddrs), 0, CFG_ITEM_DEFAULT, "10000", NULL, NULL},
  {"NdmpPort", CFG_TYPE_ADDRESSES_PORT, ITEM(StorageResource, NDMPaddrs), 0, CFG_ITEM_DEFAULT, "10000", NULL, NULL},
  {"AutoXFlateOnReplication", CFG_TYPE_BOOL, ITEM(StorageResource, autoxflateonreplication), 0, CFG_ITEM_DEFAULT, "false", "13.4.0-", NULL},
  {"AbsoluteJobTimeout", CFG_TYPE_PINT32, ITEM(StorageResource, jcr_watchdog_time), 0, 0, NULL, "14.2.0-", "Absolute time after which a Job gets terminated regardless of its progress" },
  {"CollectDeviceStatistics", CFG_TYPE_BOOL, ITEM(StorageResource, collect_dev_stats), 0, CFG_ITEM_DEPRECATED | CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"CollectJobStatistics", CFG_TYPE_BOOL, ITEM(StorageResource, collect_job_stats), 0, CFG_ITEM_DEPRECATED | CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"StatisticsCollectInterval", CFG_TYPE_PINT32, ITEM(StorageResource, stats_collect_interval), 0, CFG_ITEM_DEPRECATED | CFG_ITEM_DEFAULT, "0", NULL, NULL},
  {"DeviceReserveByMediaType", CFG_TYPE_BOOL, ITEM(StorageResource, device_reserve_by_mediatype), 0, CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"FileDeviceConcurrentRead", CFG_TYPE_BOOL, ITEM(StorageResource, filedevice_concurrent_read), 0, CFG_ITEM_DEFAULT, "false", NULL, NULL},
  {"SecureEraseCommand", CFG_TYPE_STR, ITEM(StorageResource, secure_erase_cmdline), 0, 0, NULL, "15.2.1-",
      "Specify command that will be called when bareos unlinks files."},
  {"LogTimestampFormat", CFG_TYPE_STR, ITEM(StorageResource, log_timestamp_format), 0, CFG_ITEM_DEFAULT, "%d-%b %H:%M", "15.2.3-", NULL},
    TLS_COMMON_CONFIG(StorageResource),
    TLS_CERT_CONFIG(StorageResource),
    {}
};

static ResourceItem dir_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(DirectorResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(DirectorResource, description_), 0, 0, NULL, NULL, NULL},
  {"Password", CFG_TYPE_AUTOPASSWORD, ITEM(DirectorResource, password_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Monitor", CFG_TYPE_BOOL, ITEM(DirectorResource, monitor), 0, 0, NULL, NULL, NULL},
  {"MaximumBandwidthPerJob", CFG_TYPE_SPEED, ITEM(DirectorResource, max_bandwidth_per_job), 0, 0, NULL, NULL, NULL},
  {"KeyEncryptionKey", CFG_TYPE_AUTOPASSWORD, ITEM(DirectorResource, keyencrkey), 1, 0, NULL, NULL, NULL},
    TLS_COMMON_CONFIG(DirectorResource),
    TLS_CERT_CONFIG(DirectorResource),
    {}
};

static ResourceItem ndmp_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(NdmpResource, resource_name_), 0, CFG_ITEM_REQUIRED, 0, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(NdmpResource, description_), 0, 0, 0, NULL, NULL},
  {"Username", CFG_TYPE_STR, ITEM(NdmpResource, username), 0, CFG_ITEM_REQUIRED, 0, NULL, NULL},
  {"Password", CFG_TYPE_AUTOPASSWORD, ITEM(NdmpResource, password), 0, CFG_ITEM_REQUIRED, 0, NULL, NULL},
  {"AuthType", CFG_TYPE_AUTHTYPE, ITEM(NdmpResource, AuthType), 0, CFG_ITEM_DEFAULT, "None", NULL, NULL},
  {"LogLevel", CFG_TYPE_PINT32, ITEM(NdmpResource, LogLevel), 0, CFG_ITEM_DEFAULT, "4", NULL, NULL},
  {}
};

static ResourceItem dev_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(DeviceResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, "Unique identifier of the resource."},
  {"Description", CFG_TYPE_STR, ITEM(DeviceResource, description_), 0, 0, NULL, NULL,
      "The Description directive provides easier human recognition, but is not used by Bareos directly."},
  {"MediaType", CFG_TYPE_STRNAME, ITEM(DeviceResource, media_type), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"DeviceType", CFG_TYPE_STDSTR, ITEM(DeviceResource, device_type), 0, CFG_ITEM_DEFAULT, "", NULL, NULL},
  {"ArchiveDevice", CFG_TYPE_STRNAME, ITEM(DeviceResource, archive_device_string), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"DeviceOptions", CFG_TYPE_STR, ITEM(DeviceResource, device_options), 0, 0, NULL, "15.2.0-", NULL},
  {"DiagnosticDevice", CFG_TYPE_STRNAME, ITEM(DeviceResource, diag_device_name), 0, 0, NULL, NULL, NULL},
  {"HardwareEndOfFile", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_EOF, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"HardwareEndOfMedium", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_EOM, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"BackwardSpaceRecord", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_BSR, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"BackwardSpaceFile", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_BSF, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"BsfAtEom", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_BSFATEOM, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"TwoEof", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_TWOEOF, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"ForwardSpaceRecord", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_FSR, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"ForwardSpaceFile", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_FSF, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"FastForwardSpaceFile", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_FASTFSF, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"RemovableMedia", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_REM, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"RandomAccess", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_RACCESS, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"AutomaticMount", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_AUTOMOUNT, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"LabelMedia", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_LABEL, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"AlwaysOpen", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_ALWAYSOPEN, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"Autochanger", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_ATTACHED_TO_AUTOCHANGER, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"CloseOnPoll", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_CLOSEONPOLL, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"BlockPositioning", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_POSITIONBLOCKS, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"UseMtiocget", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_MTIOCGET, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"CheckLabels", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_CHECKLABELS, CFG_ITEM_DEPRECATED | CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"RequiresMount", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_REQMOUNT, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"OfflineOnUnmount", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_OFFLINEUNMOUNT, CFG_ITEM_DEFAULT, "off", NULL, NULL},
  {"BlockChecksum", CFG_TYPE_BIT, ITEM(DeviceResource, cap_bits), CAP_BLOCKCHECKSUM, CFG_ITEM_DEFAULT, "on", NULL, NULL},
  {"AccessMode", CFG_TYPE_IODIRECTION, ITEM(DeviceResource, access_mode), 0, CFG_ITEM_DEFAULT, "readwrite", NULL, "Access mode specifies whether "
  "this device can be reserved for reading, writing or for both modes (default)."},
  {"AutoSelect", CFG_TYPE_BOOL, ITEM(DeviceResource, autoselect), 0, CFG_ITEM_DEFAULT, "true", NULL, NULL},
  {"ChangerDevice", CFG_TYPE_STRNAME, ITEM(DeviceResource, changer_name), 0, 0, NULL, NULL, NULL},
  {"ChangerCommand", CFG_TYPE_STRNAME, ITEM(DeviceResource, changer_command), 0, 0, NULL, NULL, NULL},
  {"AlertCommand", CFG_TYPE_STRNAME, ITEM(DeviceResource, alert_command), 0, 0, NULL, NULL, NULL},
  {"MaximumChangerWait", CFG_TYPE_TIME, ITEM(DeviceResource, max_changer_wait), 0, CFG_ITEM_DEFAULT,
      "300" /* 5 minutes */, NULL, NULL},
  {"MaximumOpenWait", CFG_TYPE_TIME, ITEM(DeviceResource, max_open_wait), 0, CFG_ITEM_DEFAULT, "300" /* 5 minutes */, NULL, NULL},
  {"MaximumOpenVolumes", CFG_TYPE_PINT32, ITEM(DeviceResource, max_open_vols), 0, CFG_ITEM_DEFAULT, "1", NULL, NULL},
  {"MaximumNetworkBufferSize", CFG_TYPE_PINT32, ITEM(DeviceResource, max_network_buffer_size), 0, 0, NULL, NULL, NULL},
  {"VolumePollInterval", CFG_TYPE_TIME, ITEM(DeviceResource, vol_poll_interval), 0, CFG_ITEM_DEFAULT, "300" /* 5 minutes */, NULL, NULL},
  {"MaximumRewindWait", CFG_TYPE_TIME, ITEM(DeviceResource, max_rewind_wait), 0, CFG_ITEM_DEFAULT,
      "300" /* 5 minutes */, NULL, NULL},
  {"LabelBlockSize", CFG_TYPE_PINT32, ITEM(DeviceResource, label_block_size), 0, CFG_ITEM_DEFAULT,
      "64512" /* DEFAULT_BLOCK_SIZE */, NULL, NULL},
  {"MinimumBlockSize", CFG_TYPE_PINT32, ITEM(DeviceResource, min_block_size), 0, 0, NULL, NULL, NULL},
  {"MaximumBlockSize", CFG_TYPE_MAXBLOCKSIZE, ITEM(DeviceResource, max_block_size), 0, CFG_ITEM_DEFAULT, "1048576", NULL, NULL},
  {"MaximumFileSize", CFG_TYPE_SIZE64, ITEM(DeviceResource, max_file_size), 0, CFG_ITEM_DEFAULT, "1000000000", NULL, NULL},
  {"VolumeCapacity", CFG_TYPE_SIZE64, ITEM(DeviceResource, volume_capacity), 0, 0, NULL, NULL, NULL},
  {"MaximumConcurrentJobs", CFG_TYPE_PINT32, ITEM(DeviceResource, max_concurrent_jobs), 0, CFG_ITEM_DEFAULT, "1", NULL, NULL},
  {"SpoolDirectory", CFG_TYPE_DIR, ITEM(DeviceResource, spool_directory), 0, 0, NULL, NULL, NULL},
  {"MaximumSpoolSize", CFG_TYPE_SIZE64, ITEM(DeviceResource, max_spool_size), 0, 0, NULL, NULL, NULL},
  {"MaximumJobSpoolSize", CFG_TYPE_SIZE64, ITEM(DeviceResource, max_job_spool_size), 0, 0, NULL, NULL, NULL},
  {"DriveIndex", CFG_TYPE_PINT16, ITEM(DeviceResource, drive_index), 0, 0, NULL, NULL, NULL},
  {"MountPoint", CFG_TYPE_STRNAME, ITEM(DeviceResource, mount_point), 0, 0, NULL, NULL, NULL},
  {"MountCommand", CFG_TYPE_STRNAME, ITEM(DeviceResource, mount_command), 0, 0, NULL, NULL, NULL},
  {"UnmountCommand", CFG_TYPE_STRNAME, ITEM(DeviceResource, unmount_command), 0, 0, NULL, NULL, NULL},
  {"LabelType", CFG_TYPE_LABEL, ITEM(DeviceResource, label_type), 0, CFG_ITEM_DEPRECATED, NULL, NULL, NULL},
  {"NoRewindOnClose", CFG_TYPE_BOOL, ITEM(DeviceResource, norewindonclose), 0, CFG_ITEM_DEFAULT, "true", NULL, NULL},
  {"DriveTapeAlertEnabled", CFG_TYPE_BOOL, ITEM(DeviceResource, drive_tapealert_enabled), 0, 0, NULL, NULL, NULL},
  {"DriveCryptoEnabled", CFG_TYPE_BOOL, ITEM(DeviceResource, drive_crypto_enabled), 0, 0, NULL, NULL, NULL},
  {"QueryCryptoStatus", CFG_TYPE_BOOL, ITEM(DeviceResource, query_crypto_status), 0, 0, NULL, NULL, NULL},
  {"AutoDeflate", CFG_TYPE_IODIRECTION, ITEM(DeviceResource, autodeflate), 0, 0, NULL, "13.4.0-", NULL},
  {"AutoDeflateAlgorithm", CFG_TYPE_CMPRSALGO, ITEM(DeviceResource, autodeflate_algorithm), 0, 0, NULL, "13.4.0-", NULL},
  {"AutoDeflateLevel", CFG_TYPE_PINT16, ITEM(DeviceResource, autodeflate_level), 0, CFG_ITEM_DEFAULT, "6", "13.4.0-",NULL},
  {"AutoInflate", CFG_TYPE_IODIRECTION, ITEM(DeviceResource, autoinflate), 0, 0, NULL, "13.4.0-", NULL},
  {"CollectStatistics", CFG_TYPE_BOOL, ITEM(DeviceResource, collectstats), 0, CFG_ITEM_DEFAULT, "true", NULL, NULL},
  {"EofOnErrorIsEot", CFG_TYPE_BOOL, ITEM(DeviceResource, eof_on_error_is_eot), 0, CFG_ITEM_DEFAULT, NULL, "18.2.4-",
      "If Yes, Bareos will treat any read error at an end-of-file mark as end-of-tape. You should only set "
      "this option if your tape-drive fails to detect end-of-tape while reading."},
  {"Count", CFG_TYPE_PINT32, ITEM(DeviceResource, count), 0, CFG_ITEM_DEFAULT, "1", NULL, "If Count is set to (1 < Count < 10000), "
  "this resource will be multiplied Count times. The names of multiplied resources will have a serial number (0001, 0002, ...) attached. "
  "If set to 1 only this single resource will be used and its name will not be altered."},
  {}
};

static ResourceItem autochanger_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(AutochangerResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(AutochangerResource, description_), 0, 0, NULL, NULL, NULL},
  {"Device", CFG_TYPE_ALIST_RES, ITEM(AutochangerResource, device_resources), R_DEVICE, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"ChangerDevice", CFG_TYPE_STRNAME, ITEM(AutochangerResource, changer_name), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"ChangerCommand", CFG_TYPE_STRNAME, ITEM(AutochangerResource, changer_command), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {}
};

static ResourceTable resources[] = {
  {"Director", "Directors", dir_items, R_DIRECTOR, ResourceFactory<DirectorResource> },
  {"Ndmp", "Ndmp", ndmp_items, R_NDMP, ResourceFactory<NdmpResource> },
  {"Storage", "Storages", store_items, R_STORAGE, ResourceFactory<StorageResource> },
  {"Device", "Devices", dev_items, R_DEVICE, ResourceFactory<DeviceResource> },
  {"Messages", "Messages", msgs_items, R_MSGS, ResourceFactory<MessagesResource>},
  {"Autochanger", "Autochangers", autochanger_items, R_AUTOCHANGER, ResourceFactory<AutochangerResource> },
  {}
};

/* clang-format on */

static struct s_kw authentication_methods[]
    = {{"None", AT_NONE}, {"Clear", AT_CLEAR}, {"MD5", AT_MD5}, {NULL, 0}};

struct s_io_kw {
  const char* name;
  IODirection token;
};

static s_io_kw io_directions[] = {
    {"in", IODirection::READ},         {"read", IODirection::READ},
    {"readonly", IODirection::READ},   {"out", IODirection::WRITE},
    {"write", IODirection::WRITE},     {"writeonly", IODirection::WRITE},
    {"both", IODirection::READ_WRITE}, {"readwrite", IODirection::READ_WRITE},
    {nullptr, IODirection::READ_WRITE}};

static s_kw compression_algorithms[]
    = {{"gzip", COMPRESS_GZIP},   {"lzo", COMPRESS_LZO1X},
       {"lzfast", COMPRESS_FZFZ}, {"lz4", COMPRESS_FZ4L},
       {"lz4hc", COMPRESS_FZ4H},  {NULL, 0}};

static void StoreAuthenticationType(ConfigurationParser*,
                                    BareosResource* res,
                                    LEX* lc,
                                    ResourceItem* item,
                                    int index)
{
  int i;

  LexGetToken(lc, BCT_NAME);
  // Store the type both pass 1 and pass 2
  for (i = 0; authentication_methods[i].name; i++) {
    if (Bstrcasecmp(lc->str, authentication_methods[i].name)) {
      SetItemVariable<uint32_t>(res, *item, authentication_methods[i].token);
      i = 0;
      break;
    }
  }
  if (i != 0) {
    scan_err1(lc, T_("Expected a Authentication Type keyword, got: %s"),
              lc->str);
  }
  ScanToEol(lc);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
}

// Store password either clear if for NDMP or MD5 hashed for native.
static void StoreAutopassword(ConfigurationParser* p,
                              BareosResource* res,
                              LEX* lc,
                              ResourceItem* item,
                              int index)
{
  switch (res->rcode_) {
    case R_DIRECTOR:
      /* As we need to store both clear and MD5 hashed within the same
       * resource class we use the item->code as a hint default is 0
       * and for clear we need a code of 1. */
      switch (item->code) {
        case 1:
          p->StoreResource(res, CFG_TYPE_CLEARPASSWORD, lc, item, index);
          break;
        default:
          p->StoreResource(res, CFG_TYPE_MD5PASSWORD, lc, item, index);
          break;
      }
      break;
    case R_NDMP:
      p->StoreResource(res, CFG_TYPE_CLEARPASSWORD, lc, item, index);
      break;
    default:
      p->StoreResource(res, CFG_TYPE_MD5PASSWORD, lc, item, index);
      break;
  }
}

// Store Maximum Block Size, and check it is not greater than MAX_BLOCK_LENGTH
static void StoreMaxblocksize(ConfigurationParser* p,
                              BareosResource* res,
                              LEX* lc,
                              ResourceItem* item,
                              int index)
{
  p->StoreResource(res, CFG_TYPE_SIZE32, lc, item, index);
  if (GetItemVariable<uint32_t>(res, *item) > MAX_BLOCK_LENGTH) {
    scan_err2(lc,
              T_("Maximum Block Size configured value %u is greater than "
                 "allowed maximum: %u"),
              GetItemVariable<uint32_t>(res, *item), MAX_BLOCK_LENGTH);
  }
}

// Store the IO direction on a certain device.
static void StoreIoDirection(ConfigurationParser*,
                             BareosResource* res,
                             LEX* lc,
                             ResourceItem* item,
                             int index)
{
  int i;

  LexGetToken(lc, BCT_NAME);
  for (i = 0; io_directions[i].name; i++) {
    if (Bstrcasecmp(lc->str, io_directions[i].name)) {
      SetItemVariable<IODirection>(res, *item, io_directions[i].token);
      i = 0;
      break;
    }
  }
  if (i != 0) {
    scan_err1(lc, T_("Expected a IO direction keyword, got: %s"), lc->str);
  }
  ScanToEol(lc);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
}

// Store the compression algorithm to use on a certain device.
static void StoreCompressionalgorithm(ConfigurationParser*,
                                      BareosResource* res,
                                      LEX* lc,
                                      ResourceItem* item,
                                      int index)
{
  int i;

  LexGetToken(lc, BCT_NAME);
  for (i = 0; compression_algorithms[i].name; i++) {
    if (Bstrcasecmp(lc->str, compression_algorithms[i].name)) {
      SetItemVariable<uint32_t>(res, *item,
                                compression_algorithms[i].token & 0xffffffff);
      i = 0;
      break;
    }
  }
  if (i != 0) {
    scan_err1(lc, T_("Expected a Compression algorithm keyword, got: %s"),
              lc->str);
  }
  ScanToEol(lc);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
}

/**
 * callback function for init_resource
 * See ../lib/parse_conf.c, function InitResource, for more generic handling.
 */
static void InitResourceCb(BareosResource* res, ResourceItem* item)
{
  switch (item->type) {
    case CFG_TYPE_AUTHTYPE:
      for (int i = 0; authentication_methods[i].name; i++) {
        if (Bstrcasecmp(item->default_value, authentication_methods[i].name)) {
          SetItemVariable<uint32_t>(res, *item,
                                    authentication_methods[i].token);
        }
      }
      break;
    default:
      break;
  }
}

static void ParseConfigCb(ConfigurationParser* p,
                          BareosResource* res,
                          LEX* lc,
                          ResourceItem* item,
                          int index)
{
  /* MARKER */
  switch (item->type) {
    case CFG_TYPE_AUTOPASSWORD:
      StoreAutopassword(p, res, lc, item, index);
      break;
    case CFG_TYPE_AUTHTYPE:
      StoreAuthenticationType(p, res, lc, item, index);
      break;
    case CFG_TYPE_MAXBLOCKSIZE:
      StoreMaxblocksize(p, res, lc, item, index);
      break;
    case CFG_TYPE_IODIRECTION:
      StoreIoDirection(p, res, lc, item, index);
      break;
    case CFG_TYPE_CMPRSALGO:
      StoreCompressionalgorithm(p, res, lc, item, index);
      break;
    default:
      break;
  }
}

static void MultiplyDevice(DeviceResource& multiplied_device_resource)
{
  /* append 0001 to the name of the existing resource */
  multiplied_device_resource.CreateAndAssignSerialNumber(1);

  multiplied_device_resource.multiplied_device_resource
      = std::addressof(multiplied_device_resource);

  uint32_t count = multiplied_device_resource.count - 1;

  /* create the copied devices */
  for (uint32_t i = 0; i < count; i++) {
    DeviceResource* copied_device_resource
        = new DeviceResource(multiplied_device_resource);

    /* append 0002, 0003, ... */
    copied_device_resource->CreateAndAssignSerialNumber(i + 2);

    copied_device_resource->multiplied_device_resource
        = std::addressof(multiplied_device_resource);
    copied_device_resource->count = 0;

    my_config->AppendToResourcesChain(copied_device_resource,
                                      copied_device_resource->rcode_);

    if (copied_device_resource->changer_res) {
      if (copied_device_resource->changer_res->device_resources) {
        copied_device_resource->changer_res->device_resources->append(
            copied_device_resource);
      }
    }
  }
}

static void MultiplyConfiguredDevices(ConfigurationParser& config)
{
  BareosResource* p = nullptr;
  while ((p = config.GetNextRes(R_DEVICE, p))) {
    DeviceResource& d = dynamic_cast<DeviceResource&>(*p);
    if (d.count > 1) { MultiplyDevice(d); }
  }
}

static void ConfigBeforeCallback(ConfigurationParser& config)
{
  std::map<int, std::string> map{
      {R_DIRECTOR, "R_DIRECTOR"},
      {R_JOB, "R_JOB"}, /* needed for client name conversion */
      {R_NDMP, "R_NDMP"},
      {R_STORAGE, "R_STORAGE"},
      {R_MSGS, "R_MSGS"},
      {R_DEVICE, "R_DEVICE"},
      {R_AUTOCHANGER, "R_AUTOCHANGER"},
      {R_CLIENT, "R_CLIENT"}}; /* needed for network dump */
  config.InitializeQualifiedResourceNameTypeConverter(map);
}

static void CheckDropletDevices(ConfigurationParser& config)
{
  BareosResource* p = nullptr;

  while ((p = config.GetNextRes(R_DEVICE, p)) != nullptr) {
    DeviceResource* d = dynamic_cast<DeviceResource*>(p);
    if (d && d->device_type == DeviceType::B_DROPLET_DEV) {
      if (d->max_concurrent_jobs == 0) {
        /* 0 is the general default. However, for this device_type, only 1
         * works. So we set it to this value. */
        Jmsg1(nullptr, M_WARNING, 0,
              T_("device %s is set to the default 'Maximum Concurrent Jobs' = "
                 "1.\n"),
              d->archive_device_string);
        d->max_concurrent_jobs = 1;
      } else if (d->max_concurrent_jobs > 1) {
        Jmsg2(nullptr, M_ERROR_TERM, 0,
              T_("device %s is configured with 'Maximum Concurrent Jobs' = %d, "
                 "however only 1 is supported.\n"),
              d->archive_device_string, d->max_concurrent_jobs);
      }
    }
  }
}

static void GuessMissingDeviceTypes(ConfigurationParser& config)
{
  BareosResource* p = nullptr;

  while ((p = config.GetNextRes(R_DEVICE, p)) != nullptr) {
    DeviceResource* d = dynamic_cast<DeviceResource*>(p);
    if (d && d->device_type == DeviceType::B_UNKNOWN_DEV) {
      struct stat statp;
      // Check that device is available
      if (stat(d->archive_device_string, &statp) < 0) {
        BErrNo be;
        Jmsg2(nullptr, M_ERROR_TERM, 0,
              T_("Unable to stat path '%s' for device %s: ERR=%s\n"
                 "Consider setting Device Type if device is not available when "
                 "daemon starts.\n"),
              d->archive_device_string, d->resource_name_, be.bstrerror());
        return;
      }
      if (S_ISDIR(statp.st_mode)) {
        d->device_type = DeviceType::B_FILE_DEV;
      } else if (S_ISCHR(statp.st_mode)) {
        d->device_type = DeviceType::B_TAPE_DEV;
      } else if (S_ISFIFO(statp.st_mode)) {
        d->device_type = DeviceType::B_FIFO_DEV;
      } else if (!BitIsSet(CAP_REQMOUNT, d->cap_bits)) {
        Jmsg2(nullptr, M_ERROR_TERM, 0,
              "cannot deduce Device Type from '%s'. Must be tape or directory, "
              "st_mode=%04o\n",
              d->archive_device_string, (statp.st_mode & ~S_IFMT));
        return;
      }
    }
  }
}

static void CheckAndLoadDeviceBackends(ConfigurationParser& config)
{
#if defined(HAVE_DYNAMIC_SD_BACKENDS)
  auto storage_res
      = dynamic_cast<StorageResource*>(config.GetNextRes(R_STORAGE, NULL));
#endif

  BareosResource* p = nullptr;
  while ((p = config.GetNextRes(R_DEVICE, p)) != nullptr) {
    DeviceResource* d = dynamic_cast<DeviceResource*>(p);
    if (d) {
      to_lower(d->device_type);
      if (!ImplementationFactory<Device>::IsRegistered(d->device_type)) {
#if defined(HAVE_DYNAMIC_SD_BACKENDS)
        if (!storage_res || storage_res->backend_directories.empty()) {
          Jmsg2(nullptr, M_ERROR_TERM, 0,
                "Backend Directory not set. Cannot load dynamic backend %s\n",
                d->device_type.c_str());
        }
        if (!LoadStorageBackend(d->device_type,
                                storage_res->backend_directories)) {
          Jmsg2(nullptr, M_ERROR_TERM, 0,
                "Could not load storage backend %s for device %s.\n",
                d->device_type.c_str(), d->resource_name_);
        }
#else
        Jmsg2(nullptr, M_ERROR_TERM, 0,
              "Backend %s for device %s not available.\n",
              d->device_type.c_str(), d->resource_name_);
#endif
      }
    }
  }
}

static void ConfigReadyCallback(ConfigurationParser& config)
{
  MultiplyConfiguredDevices(config);
  GuessMissingDeviceTypes(config);
  CheckAndLoadDeviceBackends(config);
  CheckDropletDevices(config);
}

ConfigurationParser* InitSdConfig(const char* t_configfile, int exit_code)
{
  ConfigurationParser* config = new ConfigurationParser(
      t_configfile, nullptr, nullptr, InitResourceCb, ParseConfigCb, nullptr,
      exit_code, R_NUM, resources, default_config_filename.c_str(),
      "bareos-sd.d", ConfigBeforeCallback, ConfigReadyCallback, DumpResource,
      FreeResource);
  if (config) { config->r_own_ = R_STORAGE; }
  return config;
}

bool ParseSdConfig(const char* t_configfile, int exit_code)
{
  bool retval;

  retval = my_config->ParseConfig();

  if (retval) {
    me = (StorageResource*)my_config->GetNextRes(R_STORAGE, NULL);
    my_config->own_resource_ = me;
    if (!me) {
      Emsg1(exit_code, 0,
            T_("No Storage resource defined in %s. Cannot continue.\n"),
            t_configfile);
      return retval;
    }
  }

  return retval;
}

// Print configuration file schema in json format
#ifdef HAVE_JANSSON
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  json_t* json = json_object();
  json_object_set_new(json, "format-version", json_integer(2));
  json_object_set_new(json, "component", json_string("bareos-sd"));
  json_object_set_new(json, "version", json_string(kBareosVersionStrings.Full));

  // Resources
  json_t* resource = json_object();
  json_object_set_new(json, "resource", resource);
  json_t* bareos_sd = json_object();
  json_object_set_new(resource, "bareos-sd", bareos_sd);

  for (int r = 0; my_config->resource_definitions_[r].name; r++) {
    ResourceTable& resource_table = my_config->resource_definitions_[r];
    json_object_set_new(bareos_sd, resource_table.name,
                        json_items(resource_table.items));
  }

  char* const json_str = json_dumps(json, JSON_INDENT(2));
  PmStrcat(buffer, json_str);
  free(json_str);
  json_decref(json);

  return true;
}
#else
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  PmStrcat(buffer, "{ \"success\": false, \"message\": \"not available\" }");
  return false;
}
#endif

#include <cassert>

static bool DumpResource_(int type,
                          BareosResource* res,
                          bool sendit(void* sock, const char* fmt, ...),
                          void* sock,
                          bool hide_sensitive_data,
                          bool verbose)
{
  PoolMem buf;
  bool recurse = true;
  OutputFormatter output_formatter
      = OutputFormatter(sendit, sock, nullptr, nullptr);
  OutputFormatterResource output_formatter_resource
      = OutputFormatterResource(&output_formatter);

  if (!res) {
    sendit(sock, T_("Warning: no \"%s\" resource (%d) defined.\n"),
           my_config->ResToStr(type), type);
    return false;
  }

  if (type < 0) { /* no recursion */
    type = -type;
    recurse = false;
  }

  switch (type) {
    case R_MSGS: {
      MessagesResource* resclass = dynamic_cast<MessagesResource*>(res);
      assert(resclass);
      resclass->PrintConfig(output_formatter_resource, *my_config,
                            hide_sensitive_data, verbose);
      break;
    }
    case R_DEVICE: {
      DeviceResource* d = dynamic_cast<DeviceResource*>(res);
      assert(d);
      d->PrintConfig(output_formatter_resource, *my_config, hide_sensitive_data,
                     verbose);
      break;
    }
    case R_AUTOCHANGER: {
      AutochangerResource* autochanger
          = dynamic_cast<AutochangerResource*>(res);
      assert(autochanger);
      autochanger->PrintConfig(output_formatter_resource, *my_config,
                               hide_sensitive_data, verbose);
      break;
    }
    default:
      BareosResource* p = dynamic_cast<BareosResource*>(res);
      assert(p);
      p->PrintConfig(output_formatter_resource, *my_config, hide_sensitive_data,
                     verbose);
      break;
  }

  return recurse;
}

static void DumpResource(int type,
                         BareosResource* res,
                         bool sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose)
{
  bool recurse = true;
  BareosResource* p = res;

  while (recurse && p) {
    recurse
        = DumpResource_(type, p, sendit, sock, hide_sensitive_data, verbose);
    p = p->next_;
  }
}

static void FreeResource(BareosResource* res, int type)
{
  if (!res) return;

  if (res->resource_name_) {
    free(res->resource_name_);
    res->resource_name_ = nullptr;
  }
  if (res->description_) {
    free(res->description_);
    res->description_ = nullptr;
  }

  BareosResource* next_ressource = (BareosResource*)res->next_;

  switch (type) {
    case R_DIRECTOR: {
      DirectorResource* p = dynamic_cast<DirectorResource*>(res);
      assert(p);
      if (p->password_.value) { free(p->password_.value); }
      if (p->address) { free(p->address); }
      if (p->keyencrkey.value) { free(p->keyencrkey.value); }
      delete p;
      break;
    }
    case R_NDMP: {
      NdmpResource* p = dynamic_cast<NdmpResource*>(res);
      assert(p);
      if (p->username) { free(p->username); }
      if (p->password.value) { free(p->password.value); }
      delete p;
      break;
    }
    case R_AUTOCHANGER: {
      AutochangerResource* p = dynamic_cast<AutochangerResource*>(res);
      assert(p);
      if (p->changer_name) { free(p->changer_name); }
      if (p->changer_command) { free(p->changer_command); }
      if (p->device_resources) { delete p->device_resources; }
      RwlDestroy(&p->changer_lock);
      delete p;
      break;
    }
    case R_STORAGE: {
      StorageResource* p = dynamic_cast<StorageResource*>(res);
      assert(p);
      if (p->SDaddrs) { FreeAddresses(p->SDaddrs); }
      if (p->SDsrc_addr) { FreeAddresses(p->SDsrc_addr); }
      if (p->NDMPaddrs) { FreeAddresses(p->NDMPaddrs); }
      if (p->working_directory) { free(p->working_directory); }
      if (p->plugin_directory) { free(p->plugin_directory); }
      if (p->plugin_names) { delete p->plugin_names; }
      if (p->scripts_directory) { free(p->scripts_directory); }
      if (p->verid) { free(p->verid); }
      if (p->secure_erase_cmdline) { free(p->secure_erase_cmdline); }
      if (p->log_timestamp_format) { free(p->log_timestamp_format); }
      delete p;
      break;
    }
    case R_DEVICE: {
      DeviceResource* p = dynamic_cast<DeviceResource*>(res);
      assert(p);
      if (p->media_type) { free(p->media_type); }
      if (p->archive_device_string) { free(p->archive_device_string); }
      if (p->device_options) { free(p->device_options); }
      if (p->diag_device_name) { free(p->diag_device_name); }
      if (p->changer_name) { free(p->changer_name); }
      if (p->changer_command) { free(p->changer_command); }
      if (p->alert_command) { free(p->alert_command); }
      if (p->spool_directory) { free(p->spool_directory); }
      if (p->mount_point) { free(p->mount_point); }
      if (p->mount_command) { free(p->mount_command); }
      if (p->unmount_command) { free(p->unmount_command); }
      delete p;
      break;
    }
    case R_MSGS: {
      MessagesResource* p = dynamic_cast<MessagesResource*>(res);
      assert(p);
      delete p;
      break;
    }
    default:
      Dmsg1(0, T_("Unknown resource type %d\n"), type);
      break;
  }
  if (next_ressource) { my_config->FreeResourceCb_(next_ressource, type); }
}

} /* namespace storagedaemon  */
