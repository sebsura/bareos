/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
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

#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "lib/edit.h"
#include "lib/util.h"
#include "lib/address_conf.h"
#include "lib/alist.h"

static void MakePathName(PoolMem& pathname, const char* str)
{
  PmStrcpy(pathname, str);
  if (pathname.c_str()[0] != '|') {
    pathname.check_size(pathname.size() + 1024);
    DoShellExpansion(pathname.c_str(), pathname.size());
  }
}

static void CheckIfItemDefaultBitIsSet(ResourceItem* item)
{
  /* Items with a default value but without the CFG_ITEM_DEFAULT flag
   * set are most of the time an indication of a programmers error. */
  if (item->default_value != nullptr && !(item->flags & CFG_ITEM_DEFAULT)) {
    Pmsg1(000,
          T_("Found config item %s which has default value but no "
             "CFG_ITEM_DEFAULT flag set\n"),
          item->name);
    item->flags |= CFG_ITEM_DEFAULT;
  }
}

void ConfigurationParser::SetResourceDefaultsParserPass1(BareosResource* res,
                                                         ResourceItem* item)
{
  Dmsg3(900, "Item=%s def=%s defval=%s\n", item->name,
        (item->flags & CFG_ITEM_DEFAULT) ? "yes" : "no",
        (item->default_value) ? item->default_value : "None");


  CheckIfItemDefaultBitIsSet(item);

  if (item->flags & CFG_ITEM_DEFAULT && item->default_value) {
    switch (item->type) {
      case CFG_TYPE_BIT:
        if (Bstrcasecmp(item->default_value, "on")) {
          char* bitfield = GetItemVariablePointer<char*>(res, *item);
          SetBit(item->code, bitfield);
        } else if (Bstrcasecmp(item->default_value, "off")) {
          char* bitfield = GetItemVariablePointer<char*>(res, *item);
          ClearBit(item->code, bitfield);
        }
        break;
      case CFG_TYPE_BOOL:
        if (Bstrcasecmp(item->default_value, "yes")
            || Bstrcasecmp(item->default_value, "true")) {
          SetItemVariable<bool>(res, *item, true);
        } else if (Bstrcasecmp(item->default_value, "no")
                   || Bstrcasecmp(item->default_value, "false")) {
          SetItemVariable<bool>(res, *item, false);
        }
        break;
      case CFG_TYPE_PINT32:
      case CFG_TYPE_INT32:
      case CFG_TYPE_SIZE32:
        SetItemVariable<uint32_t>(res, *item,
                                  str_to_uint64(item->default_value));
        break;
      case CFG_TYPE_INT64:
        SetItemVariable<uint64_t>(res, *item,
                                  str_to_int64(item->default_value));
        break;
      case CFG_TYPE_SIZE64:
        SetItemVariable<uint64_t>(res, *item,
                                  str_to_uint64(item->default_value));
        break;
      case CFG_TYPE_SPEED:
        SetItemVariable<uint64_t>(res, *item,
                                  str_to_uint64(item->default_value));
        break;
      case CFG_TYPE_TIME: {
        SetItemVariable<utime_t>(res, *item, str_to_int64(item->default_value));
        break;
      }
      case CFG_TYPE_STRNAME:
      case CFG_TYPE_STR:
        SetItemVariable<char*>(res, *item, strdup(item->default_value));
        break;
      case CFG_TYPE_STDSTR:
        SetItemVariable<std::string>(res, *item, item->default_value);
        break;
      case CFG_TYPE_DIR: {
        PoolMem pathname(PM_FNAME);
        MakePathName(pathname, item->default_value);
        SetItemVariable<char*>(res, *item, strdup(pathname.c_str()));
        break;
      }
      case CFG_TYPE_STDSTRDIR: {
        PoolMem pathname(PM_FNAME);
        MakePathName(pathname, item->default_value);
        SetItemVariable<std::string>(res, *item, std::string(pathname.c_str()));
        break;
      }
      case CFG_TYPE_ADDRESSES: {
        dlist<IPADDR>** dlistvalue
            = GetItemVariablePointer<dlist<IPADDR>**>(res, *item);
        InitDefaultAddresses(dlistvalue, item->default_value);
        break;
      }
      default:
        if (init_res_) { init_res_(res, item); }
        break;
    }
  }
}

// void ConfigurationParser::SetResourceDefaultsParserPass2(BareosResource* res,
//                                                          ResourceItem* item)
// {
//   Dmsg3(900, "Item=%s def=%s defval=%s\n", item->name,
//         (item->flags & CFG_ITEM_DEFAULT) ? "yes" : "no",
//         (item->default_value) ? item->default_value : "None");

//   if (item->flags & CFG_ITEM_DEFAULT && item->default_value) {
//     switch (item->type) {
//       case CFG_TYPE_ALIST_STR: {
//         alist<const char*>** alistvalue
//             = GetItemVariablePointer<alist<const char*>**>(res, *item);
//         if (!alistvalue) {
//           *(alistvalue) = new alist<const char*>(10, owned_by_alist);
//         }
//         (*alistvalue)->append(strdup(item->default_value));
//         break;
//       }
//       case CFG_TYPE_ALIST_DIR: {
//         PoolMem pathname(PM_FNAME);
//         alist<const char*>** alistvalue
//             = GetItemVariablePointer<alist<const char*>**>(res, *item);

//         if (!*alistvalue) {
//           *alistvalue = new alist<const char*>(10, owned_by_alist);
//         }

//         PmStrcpy(pathname, item->default_value);
//         if (*item->default_value != '|') {
//           int size;

//           // Make sure we have enough room
//           size = pathname.size() + 1024;
//           pathname.check_size(size);
//           DoShellExpansion(pathname.c_str(), pathname.size());
//         }
//         (*alistvalue)->append(strdup(pathname.c_str()));
//         break;
//       }
//       case CFG_TYPE_STR_VECTOR_OF_DIRS: {
//         std::vector<std::string>* list
//             = GetItemVariablePointer<std::vector<std::string>*>(res, *item);

//         PoolMem pathname(PM_FNAME);
//         PmStrcpy(pathname, item->default_value);
//         if (*item->default_value != '|') {
//           int size = pathname.size() + 1024;
//           pathname.check_size(size);
//           DoShellExpansion(pathname.c_str(), pathname.size());
//         }
//         list->emplace_back(pathname.c_str());
//         break;
//       }
//       default:
//         if (init_res_) { init_res_(res, item, 2); }
//         break;
//     }
//   }
// }

void ConfigurationParser::SetAllResourceDefaultsIterateOverItems(
    BareosResource* res,
    int rcode,
    ResourceItem items[],
    std::function<void(ConfigurationParser&,
                       BareosResource* res,
                       ResourceItem*)> SetDefaults)
{
  int res_item_index = 0;

  while (items[res_item_index].name) {
    SetDefaults(*this, res, &items[res_item_index]);

    if (!omit_defaults_) { SetBit(res_item_index, res->inherit_content_); }

    res_item_index++;

    if (res_item_index >= MAX_RES_ITEMS) {
      Emsg1(M_ERROR_TERM, 0, T_("Too many items in %s resource\n"),
            resource_definitions_[rcode].name);
    }
  }
}

void ConfigurationParser::InitResource(int rcode,
                                       ResourceItem items[],
                                       BareosResource* res)
{
  auto SetDefaults = [rcode](ConfigurationParser& c, BareosResource* to_init,
                             ResourceItem* item) {
    to_init->rcode_ = rcode;
    to_init->refcnt_ = 1;
    c.SetResourceDefaultsParserPass1(to_init, item);
  };
  SetAllResourceDefaultsIterateOverItems(res, rcode, items, SetDefaults);
}
