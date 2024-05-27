#include "plugin.h"

#include <stdio.h>
#include <stdint.h>

static const bareos_api* Bareos;

static ListClientCapability ClientCap;

static uint64_t port = 0;

static configuration_option Options[]
    = {{.name = "port", .type = ConfigurationValueType_U64, .required = true},
       {}};

bool loadPlugin(const bareos_api* Api, configuration_option const** Config)
{
  Bareos = Api;

  if (!Bareos->QueryCapability(Capability_ListClients, sizeof(ClientCap),
                               &ClientCap)) {
    return false;
  }

  return true;
}

void unloadPlugin() {}

void startListening(size_t configured_count, configured_option* opts)
{
  for (std::size_t i = 0; i < configured_count; ++i) {
    switch (opts[i].idx) {
      case 0: /* port_idx */ {
        port = opts[i].value.u64;
      } break;
      default: {
        char buf[1024];
        snprintf(buf, sizeof(buf), "Unknown config index at pos %zu: %zu\n", i,
                 opts[i].idx);
        buf[1023] = '\0';
        Bareos->Log(LogSeverity_Error, buf);
      } break;
    }
  }

  while (!Bareos->ShouldShutdown(Bareos)) {
    // read something from input

    auto* request = Bareos->StartRequest(Bareos);

    std::size_t ClientCount;
    char const* const* Clients;
    if (ClientCap.ListClients(request, &ClientCount, &Clients)) {
      for (std::size_t i = 0; i < ClientCount; ++i) {
        printf("Client %zu: %s\n", i, Clients[i]);
      }
    }

    Bareos->FinishRequest(request);
  }
}
