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

#include "dird/connection_plugin/config.h"
#include "config.pb.h"
#include "grpc.h"

#include <optional>

using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

namespace bareos::config {

namespace {


bool job_filter(const ::google::protobuf::RepeatedPtrField<
                    ::bareos::config::JobFilter>& filters,
                const Job& job)
{
  // we go through every filter regardless of whether we already know
  // that we do not accept the job, just so we can do some input checking
  auto accept = true;
  for (auto& filter : filters) {
    switch (filter.filter_type_case()) {
      case JobFilter::kType: {
        if (!filter.has_type()) {
          throw grpc_error(grpc::StatusCode::UNKNOWN, "bad protobuf contents");
        }
        if (filter.type().select() != job.type()) { accept = false; }
      } break;
      case JobFilter::FILTER_TYPE_NOT_SET: {
        throw grpc_error(grpc::StatusCode::INVALID_ARGUMENT,
                         "filter type is not set.");
      } break;
    }
  }
  return accept;
}

std::optional<JobType> bareos_to_grpc_type(bareos_job_type type)
{
  switch (type) {
    case BJT_BACKUP:
      return BACKUP;
    case BJT_COPY:
      return COPY;
    case BJT_RESTORE:
      return RESTORE;

    case BJT_VERIFY:
      return VERIFY;
    case BJT_ADMIN:
      return ADMIN;
    case BJT_MIGRATE:
      return MIGRATE;
    case BJT_CONSOLIDATE:
      return CONSOLIDATE;

    case BJT_SCAN:
      [[fallthrough]];
    case BJT_SYSTEM:
      [[fallthrough]];
    case BJT_ARCHIVE:
      [[fallthrough]];
    case BJT_JOB_COPY:
      [[fallthrough]];
    case BJT_CONSOLE:
      [[fallthrough]];
    case BJT_MIGRATED_JOB:
      return std::nullopt;
  }
  return std::nullopt;
}
};  // namespace

bareos_resource_type GrpcResourceTypeToBareosResourceType(
    ::bareos::config::ResourceType type)
{
  switch (type) {
    case DIRECTOR:
      return BRT_DIRECTOR;
    case CLIENT:
      return BRT_CLIENT;
    case JOBDEFS:
      return BRT_JOBDEFS;
    case JOB:
      return BRT_JOB;
    case STORAGE:
      return BRT_STORAGE;
    case CATALOG:
      return BRT_CATALOG;
    case SCHEDULE:
      return BRT_SCHEDULE;
    case FILESET:
      return BRT_FILESET;
    case POOL:
      return BRT_POOL;
    case MSGS:
      return BRT_MSGS;
    case COUNTER:
      return BRT_COUNTER;
    case PROFILE:
      return BRT_PROFILE;
    case CONSOLE:
      return BRT_CONSOLE;
    case USER:
      return BRT_USER;
    case GRPC:
      return BRT_GRPC;
    default:
      throw grpc_error(grpc::StatusCode::INVALID_ARGUMENT,
                       "Invalid config type");
  }
}

class ConfigImpl final : public Config::Service {
 public:
  ConfigImpl(config_capability cc) : cap{cc} {}

 private:
  Status Schema(ServerContext*,
                const SchemaRequest* request,
                SchemaResponse* response) override
  {
    try {
      bareos_resource_type type
          = GrpcResourceTypeToBareosResourceType(request->type());

      auto process = [values = response->mutable_schema()](
                         bareos_config_schema_entry entry) -> bool {
        SchemaValue sv;
        sv.set_is_required(entry.required);
        sv.set_is_deprecated(entry.deprecated);
        sv.set_name(entry.name);
        if (entry.default_value) { sv.set_default_value(entry.default_value); }
        if (entry.description) { sv.set_description(entry.description); }

        switch (entry.type.base_type) {
          case BCSBT_STRING: {
            sv.set_type(bareos::config::ConfigType::STRING);
          } break;
          case BCSBT_ENUM: {
            sv.set_type(bareos::config::ConfigType::ENUM);
          } break;
          case BCSBT_BOOL: {
            sv.set_type(bareos::config::ConfigType::BOOL);
          } break;
          case BCSBT_POS_INT: {
            sv.set_type(bareos::config::ConfigType::POS_INT);
          } break;
          case BCSBT_NAT_INT: {
            sv.set_type(bareos::config::ConfigType::NAT_INT);
          } break;
        }
        sv.set_multiple(entry.type.allow_multiple);

        if (entry.type.base_type == BCSBT_ENUM) {
          auto* values = sv.mutable_values();
          for (size_t i = 0; i < entry.type.enum_value_count; ++i) {
            values->Add(entry.type.enum_values[i]);
          }
        }


        values->Add(std::move(sv));
        return true;
      };

      if (!cap.config_schema(type, c_callback<decltype(process)>, &process)) {
        throw grpc_error(grpc::StatusCode::UNKNOWN, "Internal error occured");
      }
      return Status::OK;
    } catch (const grpc_error& err) {
      return err.status;
    }
  }

  Status ListClients(ServerContext*,
                     const ListClientsRequest*,
                     ListClientsResponse* response) override
  {
    try {
      auto* clients = response->mutable_clients();
      auto lambda = [clients](const bareos_config_client* data) {
        Client c;
        c.mutable_id()->mutable_name()->assign(data->name);
        c.set_name(data->name);
        c.set_address(data->address);
        clients->Add(std::move(c));
        return true;
      };

      if (!cap.list_clients(c_callback<decltype(lambda)>, &lambda)) {
        throw grpc_error(grpc::StatusCode::UNKNOWN, "Internal bareos error");
      }
    } catch (const grpc_error& err) {
      return err.status;
    }

    return Status::OK;
  }
  Status ListJobs(ServerContext*,
                  const ListJobsRequest* request,
                  ListJobsResponse* response) override
  {
    try {
      /* if no filter is set, then we accept everything by default,
       * otherwise we accept nothing by default */
      auto* jobs = response->mutable_jobs();
      auto lambda = [&filters = request->filters(),
                     jobs](const bareos_config_job* data) {
        Job j;
        j.mutable_id()->mutable_name()->assign(data->name);
        j.set_name(data->name);

        if (std::optional type = bareos_to_grpc_type(data->type)) {
          j.set_type(type.value());
        } else {
          return false;
        }

        switch (data->level) {
          case BJL_NONE: {
            // do nothing
          } break;
          case BJL_FULL: {
            j.set_default_level(FULL);
          } break;
          case BJL_DIFFERENTIAL: {
            j.set_default_level(DIFFERENTIAL);
          } break;
          case BJL_INCREMENTAL: {
            j.set_default_level(INCREMENTAL);
          } break;
          default: {
            return false;
          }
        }

        if (job_filter(filters, j)) { jobs->Add(std::move(j)); }
        return true;
      };

      if (!cap.list_jobs(c_callback<decltype(lambda)>, &lambda)) {
        throw grpc_error(grpc::StatusCode::UNKNOWN, "Internal bareos error");
      }
    } catch (const grpc_error& err) {
      return err.status;
    }

    return Status::OK;
  }
  Status ListCatalogs(ServerContext*,
                      const ListCatalogsRequest*,
                      ListCatalogsResponse* response) override
  {
    try {
      auto* catalogs = response->mutable_catalogs();
      auto lambda = [catalogs](const bareos_config_catalog* data) {
        Catalog c;
        c.mutable_id()->mutable_name()->assign(data->name);
        c.set_name(data->name);
        c.set_dbname(data->db_name);
        catalogs->Add(std::move(c));
        return true;
      };

      if (!cap.list_catalogs(c_callback<decltype(lambda)>, &lambda)) {
        throw grpc_error(grpc::StatusCode::UNKNOWN, "Internal bareos error");
      }
    } catch (const grpc_error& err) {
      return err.status;
    }

    return Status::OK;
  }

  config_capability cap;
};
}  // namespace bareos::config

std::unique_ptr<bareos::config::Config::Service> MakeConfigService(
    config_capability cap)
{
  return std::make_unique<bareos::config::ConfigImpl>(cap);
}
