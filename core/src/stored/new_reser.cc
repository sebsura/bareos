/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2025-2025 Bareos GmbH & Co. KG

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

#include "lib/parse_conf.h"
#include "stored/stored_conf.h"
#include "lib/alist.h"

namespace storagedaemon {

struct request_queue {};

struct dev {
  DeviceResource* res_;
  Device* underlying_;
  request_queue requests;

  dev(DeviceResource* res, Device* underlying)
      : res_{res}, underlying_{underlying}
  {
  }
  dev(const dev&) = delete;
  dev& operator=(const dev&) = delete;
  dev(dev&&) = default;
  dev& operator=(dev&&) = default;
};

namespace {
std::unique_ptr<dev> make_device_from_resource(DeviceResource* res)
{
  auto* underlying = FactoryCreateDevice(nullptr, res);

  if (!underlying) { return nullptr; }
  return std::make_unique<dev>(res, underlying);
}
}  // namespace


struct autochanger {
  std::vector<dev*> attached_devices;
};
struct dev_store {
  std::vector<std::unique_ptr<dev>> devices;

  std::unordered_map<DeviceResource*, dev*> dev_by_res;

  std::vector<autochanger> changer;

  dev_store() = default;
  dev_store(const dev_store&) = delete;
  dev_store& operator=(const dev_store&) = delete;
  dev_store(dev_store&&) = default;
  dev_store& operator=(dev_store&&) = default;
};

dev_store InitDevices(ConfigurationParser* config)
{
  ResLocker _{config};

  dev_store store;
  auto& devices = store.devices;
  for (auto* res
       = dynamic_cast<DeviceResource*>(config->GetNextRes(R_DEVICE, nullptr));
       res;
       res = dynamic_cast<DeviceResource*>(config->GetNextRes(R_DEVICE, res))) {
    if (auto dev = make_device_from_resource(res)) {
      devices.emplace_back(std::move(dev));
    }
  }

  auto& dev_by_res = store.dev_by_res;
  for (auto& dev : devices) { dev_by_res.emplace(dev->res_, dev.get()); }

  for (auto* res = dynamic_cast<AutochangerResource*>(
           config->GetNextRes(R_AUTOCHANGER, nullptr));
       res; res = dynamic_cast<AutochangerResource*>(
                config->GetNextRes(R_AUTOCHANGER, res))) {
    auto& changer = store.changer.emplace_back();

    for (auto* dev_res : res->device_resources) {
      if (auto found = dev_by_res.find(dev_res); found != dev_by_res.end()) {
        changer.attached_devices.push_back(found->second);
      } else if (auto dev = make_device_from_resource(dev_res)) {
        dev_by_res.emplace(dev->res_, dev.get());
        changer.attached_devices.push_back(dev.get());
        devices.emplace_back(std::move(dev));
      }
    }
  }

  return store;
}

struct dev_ref {
  dev* ptr;
};

dev_ref FindDevice() { return {nullptr}; }

};  // namespace storagedaemon
