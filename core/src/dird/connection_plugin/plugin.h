#include <cstdint>

extern "C" {

enum capability : std::uint16_t
{
  Capability_ListClients,
  Capability_ListClient,
  Capability_Restore,
};

struct request_handle;

struct ListClientCapability {
  bool (*ListClients)(request_handle* handle,
                      std::size_t* Count,
                      const char* const** clients);
};

enum log_severity
{
  LogSeverity_Debug,
  LogSeverity_Info,
  LogSeverity_Warning,
  LogSeverity_Error,
  LogSeverity_Fatal,
};

struct bareos_api {
  bool (*QueryCapability)(capability, std::size_t bufsize, void* buf);
  void (*Log)(log_severity, const char*);

  request_handle* (*StartRequest)(const bareos_api*);
  void (*FinishRequest)(request_handle*);
  bool (*ShouldShutdown)(const bareos_api*);
};

enum configuration_value_type
{
  ConfigurationValueType_CString,
  ConfigurationValueType_U64,
  ConfigurationValueType_U32,
  ConfigurationValueType_S64,
  ConfigurationValueType_S32,
};

union configuration_value {
  const char* cstring;
  std::uint64_t u64;
  std::uint32_t u32;
  std::int64_t s64;
  std::int32_t s32;
};

struct configuration_option {
  const char* name;
  configuration_value_type type;
  bool required;
};

struct configured_option {
  std::size_t idx;
  configuration_value value;
};
}
