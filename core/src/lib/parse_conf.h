/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2010 Free Software Foundation Europe e.V.
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
// Kern Sibbald, January MM
#ifndef BAREOS_LIB_PARSE_CONF_H_
#define BAREOS_LIB_PARSE_CONF_H_

#include "include/bareos.h"
#include "include/bc_types.h"
#include "lib/bstringlist.h"
#include "lib/parse_conf_callbacks.h"
#include "lib/s_password.h"
#include "lib/tls_conf.h"
#include "lib/keyword_table_s.h"
#include "lib/message_destination_info.h"
#include "lib/util.h"
#include "lib/lex.h"
#include "lib/bpipe.h"

#include <functional>
#include <memory>
#include <map>

struct ResourceItem;
class ConfigParserStateMachine;
class ConfigurationParser;
class ConfigResourcesContainer;
/* For storing name_addr items in res_items table */

/* using offsetof on non-standard-layout types is conditionally supported. As
 * all the compiler we're currently using support this, it should be safe to
 * use. It is at least safer to use than the undefined behaviour we previously
 * utilized.
 */
#define ITEM(c, m) offsetof(c, m)
#define ITEMC(c) 0

/*
 * Master Resource configuration structure definition
 * This is the structure that defines the resources that are available to
 * this daemon.
 */
struct ResourceTable {
  const char* name;      /* Resource name */
  const char* groupname; /* Resource name in plural form */
  ResourceItem* items;   /* List of resource keywords */
  uint32_t rcode;        /* Code if needed */

  BareosResource* (*make)();
};

struct parsable_resource {
  BareosResource* res{};
  ResourceItem* items{};
  int code;

  operator bool() const { return res != nullptr; }
};

// Common Resource definitions
#define MAX_RES_NAME_LENGTH \
  (MAX_NAME_LENGTH - 1) /* maximum resource name length */

// Config item flags.
#define CFG_ITEM_REQUIRED 0x1   /* Item required */
#define CFG_ITEM_DEFAULT 0x2    /* Default supplied */
#define CFG_ITEM_NO_EQUALS 0x4  /* Don't scan = after name */
#define CFG_ITEM_DEPRECATED 0x8 /* Deprecated config option */
#define CFG_ITEM_ALIAS 0x10     /* Item is an alias for another */

/*
 * CFG_ITEM_DEFAULT_PLATFORM_SPECIFIC: the value may differ between different
 * platforms (or configure settings). This information is used for the
 * documentation.
 */
#define CFG_ITEM_PLATFORM_SPECIFIC 0x20

enum
{
  // Standard resource types. handlers in res.c
  /* clang-format off */
  CFG_TYPE_STR = 1,                 /* String */
  CFG_TYPE_DIR = 2,                 /* Directory */
  CFG_TYPE_MD5PASSWORD = 3,         /* MD5 hashed Password */
  CFG_TYPE_CLEARPASSWORD = 4,       /* Clear text Password */
  CFG_TYPE_AUTOPASSWORD = 5,        /* Password stored in clear when needed otherwise hashed */
  CFG_TYPE_NAME = 6,                /* Name */
  CFG_TYPE_STRNAME = 7,             /* String Name */
  CFG_TYPE_RES = 8,                 /* Resource */
  CFG_TYPE_ALIST_RES = 9,           /* List of resources */
  CFG_TYPE_ALIST_STR = 10,          /* List of strings */
  CFG_TYPE_ALIST_DIR = 11,          /* List of dirs */
  CFG_TYPE_INT16 = 12,              /* 16 bits Integer */
  CFG_TYPE_PINT16 = 13,             /* Positive 16 bits Integer (unsigned) */
  CFG_TYPE_INT32 = 14,              /* 32 bits Integer */
  CFG_TYPE_PINT32 = 15,             /* Positive 32 bits Integer (unsigned) */
  CFG_TYPE_MSGS = 16,               /* Message resource */
  CFG_TYPE_INT64 = 17,              /* 64 bits Integer */
  CFG_TYPE_BIT = 18,                /* Bitfield */
  CFG_TYPE_BOOL = 19,               /* Boolean */
  CFG_TYPE_TIME = 20,               /* Time value */
  CFG_TYPE_SIZE64 = 21,             /* 64 bits file size */
  CFG_TYPE_SIZE32 = 22,             /* 32 bits file size */
  CFG_TYPE_SPEED = 23,              /* Speed limit */
  CFG_TYPE_DEFS = 24,               /* Definition */
  CFG_TYPE_LABEL = 25,              /* Label */
  CFG_TYPE_ADDRESSES = 26,          /* List of ip addresses */
  CFG_TYPE_ADDRESSES_ADDRESS = 27,  /* Ip address */
  CFG_TYPE_ADDRESSES_PORT = 28,     /* Ip port */
  CFG_TYPE_PLUGIN_NAMES = 29,       /* Plugin Name(s) */
  CFG_TYPE_STDSTR = 30,             /* String as std::string */
  CFG_TYPE_STDSTRDIR = 31,          /* Directory as std::string */
  CFG_TYPE_STR_VECTOR = 32,         /* std::vector<std::string> of any string */
  CFG_TYPE_STR_VECTOR_OF_DIRS = 33, /* std::vector<std::string> of directories */
  CFG_TYPE_DIR_OR_CMD = 34,         /* Directory or command (starting with "|") */
  /* clang-format on */

  // Director resource types. handlers in dird_conf.
  CFG_TYPE_ACL = 50,              /* User Access Control List */
  CFG_TYPE_AUDIT = 51,            /* Auditing Command List */
  CFG_TYPE_AUTHPROTOCOLTYPE = 52, /* Authentication Protocol */
  CFG_TYPE_AUTHTYPE = 53,         /* Authentication Type */
  CFG_TYPE_DEVICE = 54,           /* Device resource */
  CFG_TYPE_JOBTYPE = 55,          /* Type of Job */
  CFG_TYPE_PROTOCOLTYPE = 56,     /* Protocol */
  CFG_TYPE_LEVEL = 57,            /* Backup Level */
  CFG_TYPE_REPLACE = 58,          /* Replace option */
  CFG_TYPE_SHRTRUNSCRIPT = 59,    /* Short Runscript definition */
  CFG_TYPE_RUNSCRIPT = 60,        /* Runscript */
  CFG_TYPE_RUNSCRIPT_CMD = 61,    /* Runscript Command */
  CFG_TYPE_RUNSCRIPT_TARGET = 62, /* Runscript Target (Host) */
  CFG_TYPE_RUNSCRIPT_BOOL = 63,   /* Runscript Boolean */
  CFG_TYPE_RUNSCRIPT_WHEN = 64,   /* Runscript When expression */
  CFG_TYPE_MIGTYPE = 65,          /* Migration Type */
  CFG_TYPE_INCEXC = 66,           /* Include/Exclude item */
  CFG_TYPE_RUN = 67,              /* Schedule Run Command */
  CFG_TYPE_ACTIONONPURGE = 68,    /* Action to perform on Purge */
  CFG_TYPE_POOLTYPE = 69,         /* Pool Type */

  // Director fileset options. handlers in dird_conf.
  CFG_TYPE_FNAME = 80,      /* Filename */
  CFG_TYPE_PLUGINNAME = 81, /* Pluginname */
  CFG_TYPE_EXCLUDEDIR = 82, /* Exclude directory */
  CFG_TYPE_OPTIONS = 83,    /* Options block */
  CFG_TYPE_OPTION = 84,     /* Option of Options block */
  CFG_TYPE_REGEX = 85,      /* Regular Expression */
  CFG_TYPE_BASE = 86,       /* Basejob Expression */
  CFG_TYPE_WILD = 87,       /* Wildcard Expression */
  CFG_TYPE_PLUGIN = 88,     /* Plugin definition */
  CFG_TYPE_FSTYPE = 89,     /* FileSytem match criterium (UNIX)*/
  CFG_TYPE_DRIVETYPE = 90,  /* DriveType match criterium (Windows) */
  CFG_TYPE_META = 91,       /* Meta tag */

  // Storage daemon resource types
  // CFG_TYPE_DEVTYPE = 201,      /* Device Type */
  CFG_TYPE_MAXBLOCKSIZE = 202, /* Maximum Blocksize */
  CFG_TYPE_IODIRECTION = 203,  /* AutoXflateMode IO Direction */
  CFG_TYPE_CMPRSALGO = 204,    /* Compression Algorithm */

  // File daemon resource types
  CFG_TYPE_CIPHER = 301 /* Encryption Cipher */
};

struct DatatypeName {
  const int number;
  const char* name;
  const char* description;
};

struct ident {
  std::string name;
};

struct done {};

struct unexpected_token {
  int value;
};

struct parse_result {
  std::string errmsg;

  parse_result() = default;
  parse_result(std::string_view msg) : errmsg(msg) {}

  operator bool() const { return errmsg.size() == 0; }

  const char* strerror() const { return errmsg.c_str(); }
};


typedef void(INIT_RES_HANDLER)(BareosResource* res, ResourceItem* item);
typedef void(STORE_RES_HANDLER)(ConfigurationParser* p,
                                BareosResource* res,
                                LEX* lc,
                                ResourceItem* item,
                                int index);
typedef void(PRINT_RES_HANDLER)(BareosResource* res,
                                ResourceItem& item,
                                OutputFormatterResource& send,
                                bool hide_sensitive_data,
                                bool inherited,
                                bool verbose);


class QualifiedResourceNameTypeConverter;

enum class DependencyStorageType
{
  SINGLE,
  ALIST,
  VECTOR,
};

struct dependency_target {
  /* a depedency target is a tuple that describes where to store a certain
     resource dependency */

  BareosResource* base;
  ResourceItem* item;

  bool operator==(const dependency_target& other) const
  {
    return base == other.base && item == other.item;
  }
};

template <> struct std::hash<dependency_target> {
  size_t operator()(const dependency_target& tgt) const
  {
    auto l = std::hash<BareosResource*>{}(tgt.base);
    auto r = std::hash<ResourceItem*>{}(tgt.item);

    std::size_t seed{0};
    seed = hash_combine(seed, l);
    seed = hash_combine(seed, r);
    return seed;
  }
};

class ConfigurationParser {
  friend class ConfiguredTlsPolicyGetterPrivate;
  friend class ConfigParserStateMachine;

 public:
  std::string cf_;                             /* Config file parameter */
  LEX_ERROR_HANDLER* scan_error_{nullptr};     /* Error handler if non-null */
  LEX_WARNING_HANDLER* scan_warning_{nullptr}; /* Warning handler if non-null */
  INIT_RES_HANDLER* init_res_{
      nullptr}; /* Init resource handler for non default types if non-null */
  STORE_RES_HANDLER* store_res_{
      nullptr}; /* Store resource handler for non default types if non-null */
  PRINT_RES_HANDLER* print_res_{
      nullptr}; /* Print resource handler for non default types if non-null */

  int32_t err_type_{0};       /* The way to Terminate on failure */
  bool omit_defaults_{false}; /* Omit config variables with default values when
                          dumping the config */

  int32_t r_num_{0};                      /* number of daemon resource types */
  int32_t r_own_{0};                      /* own resource type */
  BareosResource* own_resource_{nullptr}; /* Pointer to own resource */
  ResourceTable* resource_definitions_{
      0}; /* Pointer to table of permitted resources */
  std::shared_ptr<ConfigResourcesContainer> config_resources_container_;
  mutable brwlock_t res_lock_; /* Resource lock */

  DumpResourceCb_t DumpResourceCb_{nullptr};
  FreeResourceCb_t FreeResourceCb_{nullptr};

  bool AddDependency(DependencyStorageType type,
                     BareosResource* s,
                     ResourceItem* item,
                     std::string_view referenced_name);

  ConfigurationParser();
  ConfigurationParser(const char* cf,
                      LEX_ERROR_HANDLER* ScanError,
                      LEX_WARNING_HANDLER* scan_warning,
                      INIT_RES_HANDLER* init_res,
                      STORE_RES_HANDLER* StoreRes,
                      PRINT_RES_HANDLER* print_res,
                      int32_t err_type,
                      int32_t r_num,
                      ResourceTable* resources,
                      const char* config_default_filename,
                      const char* config_include_dir,
                      void (*ParseConfigBeforeCb)(ConfigurationParser&),
                      void (*ParseConfigReadyCb)(ConfigurationParser&),
                      DumpResourceCb_t DumpResourceCb,
                      FreeResourceCb_t FreeResourceCb);

  ~ConfigurationParser();

  bool IsUsingConfigIncludeDir() const { return use_config_include_dir_; }
  void ParseConfigOrExit();
  bool ParseConfig();
  bool ParseConfigFile(const char* config_file_name,
                       void* caller_ctx,
                       LEX_ERROR_HANDLER* scan_error = nullptr,
                       LEX_WARNING_HANDLER* scan_warning = nullptr);
  const std::string& get_base_config_path() const { return used_config_path_; }
  void FreeResources();

  std::shared_ptr<ConfigResourcesContainer> GetResourcesContainer();
  std::shared_ptr<ConfigResourcesContainer> BackupResourcesContainer();
  void RestoreResourcesContainer(std::shared_ptr<ConfigResourcesContainer>&&);

  void InitResource(int rcode, ResourceItem items[], BareosResource* new_res);

  parsable_resource make_resource(const char* name);

  bool AppendToResourcesChain(BareosResource* new_resource, int rcode);
  bool RemoveResource(int rcode, const char* name);
  bool DumpResources(bool sendit(void* sock, const char* fmt, ...),
                     void* sock,
                     const std::string& res_type_name,
                     const std::string& res_name,
                     bool hide_sensitive_data = false);
  void DumpResources(bool sendit(void* sock, const char* fmt, ...),
                     void* sock,
                     bool hide_sensitive_data = false);
  int GetResourceCode(const char* resource_type);
  ResourceTable* GetResourceTable(const char* resource_type_name);
  int GetResourceItemIndex(ResourceItem* res_table, const char* item);
  ResourceItem* GetResourceItem(ResourceItem* res_table, const char* item);
  bool GetPathOfResource(PoolMem& path,
                         const char* component,
                         const char* resourcetype,
                         const char* name,
                         bool set_wildcards = false);
  bool GetPathOfNewResource(PoolMem& path,
                            PoolMem& extramsg,
                            const char* component,
                            const char* resourcetype,
                            const char* name,
                            bool error_if_exits = false,
                            bool create_directories = false);
  BareosResource* GetNextRes(int rcode, BareosResource* res) const;
  BareosResource* GetResWithName(int rcode,
                                 const char* name,
                                 bool lock = true) const;
  void b_LockRes(const char* file, int line) const;
  void b_UnlockRes(const char* file, int line) const;
  const char* ResToStr(int rcode) const;
  const char* ResGroupToStr(int rcode) const;
  void InsertResource(int resource_type, BareosResource* res);

  bool StoreResource(BareosResource* res,
                     int rcode,
                     LEX* lc,
                     ResourceItem* item,
                     int index);
  void InitializeQualifiedResourceNameTypeConverter(
      const std::map<int, std::string>&);
  QualifiedResourceNameTypeConverter* GetQualifiedResourceNameTypeConverter()
      const
  {
    return qualified_resource_name_type_converter_.get();
  }
  static bool GetTlsPskByFullyQualifiedResourceName(
      ConfigurationParser* config,
      const char* fully_qualified_name,
      std::string& psk);
  bool GetConfiguredTlsPolicyFromCleartextHello(
      const std::string& r_code,
      const std::string& name,
      TlsPolicy& tls_policy_out) const;
  std::string CreateOwnQualifiedNameForNetworkDump() const;

  void AddWarning(const std::string& warning);
  void ClearWarnings();
  bool HasWarnings() const;
  const BStringList& GetWarnings() const;

  std::variant<done, ident, unexpected_token> NextResourceIdentifier(LEX* lex);

  parse_result ParseResource(BareosResource* res,
                             ResourceItem* items,
                             LEX* lex);

 private:
  ConfigurationParser(const ConfigurationParser&) = delete;
  ConfigurationParser operator=(const ConfigurationParser&) = delete;

 private:
  std::string config_default_filename_; /* default config filename, that is
                                           used, if no filename is given */
  std::string config_dir_; /* base directory of configuration files */
  std::string
      config_include_dir_; /* rel. path to the config include directory
                               (bareos-dir.d, bareos-sd.d, bareos-fd.d, ...) */
  bool use_config_include_dir_{false}; /* Use the config include directory */
  std::string config_include_naming_format_; /* Format string for file paths of
                                                resources */
  std::string used_config_path_;             /* Config file that is used. */
  std::unique_ptr<QualifiedResourceNameTypeConverter>
      qualified_resource_name_type_converter_;
  ParseConfigBeforeCb_t ParseConfigBeforeCb_{nullptr};
  ParseConfigReadyCb_t ParseConfigReadyCb_{nullptr};
  bool parser_first_run_{true};
  BStringList warnings_;


 public:
  static const char* GetDefaultConfigDir();

 private:
  bool ParsingPass(LEX* lex);
  bool FixupPass();
  bool VerifyPass();


  bool GetConfigFile(PoolMem& full_path,
                     const char* config_dir,
                     const char* config_filename);
  bool GetConfigIncludePath(PoolMem& full_path, const char* config_dir);
  bool FindConfigPath(PoolMem& full_path);
  int GetResourceTableIndex(const char* resource_type_name);

 public:
  /* MARKER */
  void ScanTypes(LEX* lc,
                 MessagesResource* msg,
                 MessageDestinationCode dest_code,
                 const std::string& where,
                 const std::string& cmd,
                 const std::string& timestamp_format);

 private:
  void lex_error(const char* cf,
                 LEX_ERROR_HANDLER* ScanError,
                 LEX_WARNING_HANDLER* scan_warning) const;
  void SetAllResourceDefaultsIterateOverItems(
      BareosResource* res,
      int rcode,
      ResourceItem items[],
      std::function<void(ConfigurationParser&, BareosResource*, ResourceItem*)>
          SetDefaults);
  void SetResourceDefaultsParserPass1(BareosResource* res, ResourceItem* item);


 private:
  std::unordered_map<dependency_target, std::string> single_dependencies;
  std::unordered_map<dependency_target, std::vector<std::string>>
      vector_dependencies;
  std::unordered_map<dependency_target, std::vector<std::string>>
      alist_dependencies;
};


class ConfigResourcesContainer {
 private:
  std::chrono::time_point<std::chrono::system_clock> timestamp_{};
  ConfigurationParser* config_ = nullptr;

 public:
  std::unique_ptr<BareosResource*[]> configuration_resources_ = nullptr;
  ConfigResourcesContainer(ConfigurationParser* config)
  {
    config_ = config;
    int num = config_->r_num_;
    configuration_resources_ = std::make_unique<BareosResource*[]>(num);
    Dmsg1(10, "ConfigResourcesContainer: new configuration_resources_ %p\n",
          configuration_resources_.get());
  }

  ~ConfigResourcesContainer()
  {
    Dmsg1(10, "ConfigResourcesContainer freeing %p %s\n",
          configuration_resources_.get(), TPAsString(timestamp_).c_str());
    int num = config_->r_num_;
    for (int j = 0; j < num; j++) {
      config_->FreeResourceCb_(configuration_resources_[j], j);
      configuration_resources_[j] = nullptr;
    }
    configuration_resources_.reset(nullptr);
  }
  void SetTimestampToNow() { timestamp_ = std::chrono::system_clock::now(); }
  std::string TimeStampAsString() { return TPAsString(timestamp_); }
};


bool PrintMessage(void* sock, const char* fmt, ...);
bool IsTlsConfigured(TlsResource* tls_resource);

const char* GetName(BareosResource* res, ResourceItem& item, s_kw* keywords);
bool HasDefaultValue(BareosResource* res, ResourceItem& item, s_kw* keywords);

// Data type routines
DatatypeName* GetDatatype(int number);
const char* DatatypeToString(int type);
const char* DatatypeToDescription(int type);

/* this function is used as an initializer in foreach_res, so we can null
 * the pointer passed into and also get a reference to the configuration that
 * we then keep for the lifetime of the loop.
 */
inline std::shared_ptr<ConfigResourcesContainer> _init_foreach_res_(
    ConfigurationParser* config,
    void* var)
{
  memset(var, 0, sizeof(void*));
  return config->GetResourcesContainer();
}

// Loop through each resource of type, returning in var
#define _foreach_res_join_name(sym1, sym2) _foreach_res_join_impl(sym1, sym2)
#define _foreach_res_join_impl(sym1, sym2) sym1##sym2
#define foreach_res(var, type)                                              \
  for (auto _foreach_res_join_name(_foreach_res_config_table_, __COUNTER__) \
       = _init_foreach_res_(my_config, &var);                               \
       ((var)                                                               \
        = static_cast<decltype(var)>(my_config->GetNextRes((type), var)));)

class ResLocker {
  const ConfigurationParser* config_res_;

 public:
  ResLocker(const ConfigurationParser* config) : config_res_(config)
  {
    config_res_->b_LockRes(__FILE__, __LINE__);
  }
  ~ResLocker() { config_res_->b_UnlockRes(__FILE__, __LINE__); }
  ResLocker(const ResLocker&) = delete;
  ResLocker& operator=(const ResLocker&) = delete;
  ResLocker(ResLocker&&) = delete;
  ResLocker& operator=(ResLocker&&) = delete;
};

template <typename Resource> BareosResource* ResourceFactory()
{
  static_assert(std::is_base_of_v<BareosResource, Resource>);
  return new Resource();
}

#endif  // BAREOS_LIB_PARSE_CONF_H_
