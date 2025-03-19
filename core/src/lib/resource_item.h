/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2010 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2025 Bareos GmbH & Co. KG

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

#ifndef BAREOS_LIB_RESOURCE_ITEM_H_
#define BAREOS_LIB_RESOURCE_ITEM_H_

#include <vector>
#include <string>
#include <optional>
#include <utility>

#include "lib/parse_conf.h"

struct s_password;
template <typename T> class alist;
template <typename T> class dlist;

namespace config {
struct DefaultValue {
  const char* value;

  constexpr explicit DefaultValue(const char* value_) : value{value_} {}
};

struct Version {
  size_t major{}, minor{}, patch{};
};

struct DeprecatedSince {
  Version version;
  constexpr explicit DeprecatedSince(Version value_) : version{value_} {}
  constexpr explicit DeprecatedSince(size_t major, size_t minor, size_t patch)
      : version{major, minor, patch}
  {
  }
};

struct IntroducedIn {
  Version version;
  constexpr explicit IntroducedIn(Version value_) : version{value_} {}
  constexpr explicit IntroducedIn(size_t major, size_t minor, size_t patch)
      : version{major, minor, patch}
  {
  }
};

struct Code {
  int32_t value;
  constexpr explicit Code(int32_t value_) : value{value_} {}
};

struct Required {};

struct Alias {
  const char* name;

  constexpr explicit Alias(const char* name_) : name{name_} {}
};

struct UsesNoEquals {};

struct Description {
  const char* text;

  constexpr explicit Description(const char* text_) : text{text_} {}
};

struct PlatformSpecific {};
};  // namespace config


template <typename What, typename... Args> struct occurances {
  static constexpr size_t value = []() {
    if constexpr (sizeof...(Args) == 0) {
      return 0;
    } else {
      return (std::is_same_v<What, Args> + ...);
    }
  }();
};

template <typename What, typename... Args> struct is_present {
  static constexpr bool value{occurances<What, Args...>::value > 0};
};


template <typename T, typename... Ts>
constexpr T* get_if(std::tuple<Ts...>& tuple)
{
  if constexpr (is_present<T, Ts...>::value) {
    return &std::get<T>(tuple);
  } else {
    return nullptr;
  }
}

struct ResourceItemFlags {
  std::optional<config::Version> introduced_in{};
  std::optional<config::Version> deprecated_since{};
  const char* default_value{};
  std::optional<int32_t> extra{};
  bool required{};
  const char* alt_name{};
  bool platform_specific{};
  bool no_equals{};
  const char* description{};

  template <typename... Types> constexpr ResourceItemFlags(Types&&... values)
  {
    static_assert(
        (is_present<Types, config::DefaultValue, config::IntroducedIn,
                    config::DeprecatedSince, config::Code, config::Required,
                    config::Alias, config::Description,
                    config::PlatformSpecific, config::UsesNoEquals>::value
         && ...),
        "only allowed flags may be used");

    static_assert(occurances<config::DefaultValue, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::IntroducedIn, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::DeprecatedSince, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::Code, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::Required, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::Alias, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::Description, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::PlatformSpecific, Types...>::value <= 1,
                  "flag may only be specified once");
    static_assert(occurances<config::UsesNoEquals, Types...>::value <= 1,
                  "flag may only be specified once");

    std::tuple<Types...> tup{std::forward<Types>(values)...};

    if (auto* defval = get_if<config::DefaultValue>(tup)) {
      default_value = defval->value;
    }
    if (auto* introduced = get_if<config::IntroducedIn>(tup)) {
      introduced_in = introduced->version;
    }
    if (auto* deprecated = get_if<config::DeprecatedSince>(tup)) {
      deprecated_since = deprecated->version;
    }
    if (auto* code = get_if<config::Code>(tup)) { extra = code->value; }
    if (auto* _ = get_if<config::Required>(tup)) { required = true; }
    if (auto* alias = get_if<config::Alias>(tup)) { alt_name = alias->name; }
    if (auto* _ = get_if<config::UsesNoEquals>(tup)) { no_equals = true; }
    if (auto* _ = get_if<config::PlatformSpecific>(tup)) {
      platform_specific = true;
    }
    if (auto* desc = get_if<config::Description>(tup)) {
      description = desc->text;
    }
  }
};

/*
 * This is the structure that defines the record types (items) permitted within
 * each resource. It is used to define the configuration tables.
 */
struct ResourceItem {
  using resource_fun = BareosResource*();
  using address_fun = char*(BareosResource*);

  constexpr ResourceItem(const char* name_,
                         const int type_,
                         resource_fun* res_fun_,
                         address_fun* addr_fun_,
                         ResourceItemFlags&& resource_flags)
      : name{name_}
      , type{type_}
      , res_fun{res_fun_}
      , addr_fun{addr_fun_}
      , code{resource_flags.extra.value_or(int32_t{})}
      , alias{resource_flags.alt_name}
      , required{resource_flags.required}
      , deprecated{resource_flags.deprecated_since.has_value()}
      , platform_specific{resource_flags.platform_specific}
      , no_equal{resource_flags.no_equals}
      , default_value{resource_flags.default_value}
      , introduced_in{resource_flags.introduced_in}
      , deprecated_since{resource_flags.deprecated_since}
      , description{resource_flags.description}
  {
  }

  ResourceItem() = default;

  const char* name{}; /* Resource name i.e. Director, ... */
  int type{};
  resource_fun* res_fun{};
  address_fun* addr_fun{};
  int32_t code{}; /* Item code/additional info */
  const char* alias{};
  bool required{};
  bool deprecated{};
  bool platform_specific{};
  bool no_equal{};
  const char* default_value{}; /* Default value */

  std::optional<config::Version> introduced_in{};
  std::optional<config::Version> deprecated_since{};

  /* short description of the directive, in plain text,
   * used for the documentation.
   * Full sentence.
   * Every new directive should have a description. */
  const char* description{};

  void SetPresent() const { allocated_resource()->SetMemberPresent(name); }

  bool IsPresent() const { return allocated_resource()->IsMemberPresent(name); }

  bool is_required() const { return required; }
  bool is_platform_specific() const { return platform_specific; }
  bool is_deprecated() const { return deprecated; }
  bool has_no_eq() const { return no_equal; }

  BareosResource* allocated_resource() const { return (*res_fun)(); }

  char* member_address(BareosResource* res) const { return (*addr_fun)(res); }
  const char* member_address(const BareosResource* res) const
  {
    return (*addr_fun)(const_cast<BareosResource*>(res));
  }
};

static inline void* CalculateAddressOfMemberVariable(BareosResource* res,
                                                     const ResourceItem& item)
{
  return item.member_address(res);
}

static inline const void* CalculateAddressOfMemberVariable(
    const BareosResource* res,
    const ResourceItem& item)
{
  return item.member_address(res);
}

template <typename P>
P GetItemVariable(const BareosResource* res, const ResourceItem& item)
{
  const void* p = CalculateAddressOfMemberVariable(res, item);

  return *(static_cast<typename std::remove_reference<P>::type const*>(p));
}

template <typename P>
P GetItemVariablePointer(BareosResource* res, const ResourceItem& item)
{
  void* p = CalculateAddressOfMemberVariable(res, item);
  return static_cast<P>(p);
}

template <typename P>
auto GetItemVariablePointer(const BareosResource* res, const ResourceItem& item)
{
  const void* p = CalculateAddressOfMemberVariable(res, item);

  using T = std::remove_pointer_t<P>;

  return static_cast<T const*>(p);
}

template <typename P> P GetItemVariable(const ResourceItem& item)
{
  return GetItemVariable<P>(item.allocated_resource(), item);
}

template <typename P> P GetItemVariablePointer(const ResourceItem& item)
{
  return GetItemVariablePointer<P>(item.allocated_resource(), item);
}

template <typename P, typename V>
void SetItemVariable(const ResourceItem& item, const V& value)
{
  P* p = GetItemVariablePointer<P*>(item);
  *p = value;
}

template <typename P, typename V>
void SetItemVariableFreeMemory(const ResourceItem& item, const V& value)
{
  void* p = GetItemVariablePointer<void*>(item);
  P** q = (P**)p;
  if (*q) free(*q);
  (*(P**)p) = (P*)value;
}

namespace config {

template <typename T, typename U> using MemberFn = U*(T*);
template <typename T, MemberFn<T, char*>* member_get>
constexpr ResourceItem String(const char* name)
{
  return ResourceItem{name,
                      CFG_TYPE_STR,
                      +[]() -> BareosResource* { return nullptr; },
                      +[](BareosResource* res) {
                        auto actual_res = dynamic_cast<T*>(res);
                        return (char*)member_get(actual_res);
                      },
                      {}};
}
#if 0
constexpr ResourceItem Directory() { return {}; }
constexpr ResourceItem Md5Password() { return {}; }
constexpr ResourceItem ClearPassword() { return {}; }
constexpr ResourceItem AutoPassword() { return {}; }
constexpr ResourceItem Name() { return {}; }
constexpr ResourceItem StrName() { return {}; }
constexpr ResourceItem Resource() { return {}; }
constexpr ResourceItem AlistResource() { return {}; }
constexpr ResourceItem AlistString() { return {}; }
constexpr ResourceItem AlistDirectory() { return {}; }
constexpr ResourceItem Int16() { return {}; }
constexpr ResourceItem PositiveInt16() { return {}; }
constexpr ResourceItem Int32() { return {}; }
constexpr ResourceItem PositiveInt32() { return {}; }
constexpr ResourceItem Messages() { return {}; }
constexpr ResourceItem Int64() { return {}; }
constexpr ResourceItem Bit() { return {}; }
constexpr ResourceItem Bool() { return {}; }
constexpr ResourceItem Time() { return {}; }
constexpr ResourceItem Size64() { return {}; }
constexpr ResourceItem Size32() { return {}; }
constexpr ResourceItem Speed() { return {}; }
constexpr ResourceItem Defs() { return {}; }
constexpr ResourceItem Label() { return {}; }
constexpr ResourceItem Addresses() { return {}; }
constexpr ResourceItem Addresses_Address() { return {}; }
constexpr ResourceItem Addresses_Port() { return {}; }
constexpr ResourceItem PluginNames() { return {}; }
constexpr ResourceItem StdString() { return {}; }
constexpr ResourceItem StdString_Directory() { return {}; }
constexpr ResourceItem VectorString() { return {}; }
constexpr ResourceItem VectorString_Directory() { return {}; }
constexpr ResourceItem DirectoryOrCmd() { return {}; }
// Director resource types. handlers in dird_conf.
constexpr ResourceItem Acl() { return {}; }
constexpr ResourceItem Audit() { return {}; }
constexpr ResourceItem AuthProtocolType() { return {}; }
constexpr ResourceItem AuthType() { return {}; }
constexpr ResourceItem Device() { return {}; }
constexpr ResourceItem JobType() { return {}; }
constexpr ResourceItem ProtocolType() { return {}; }
constexpr ResourceItem Level() { return {}; }
constexpr ResourceItem Replace() { return {}; }
constexpr ResourceItem ShortRunscript() { return {}; }
constexpr ResourceItem Runscript() { return {}; }
constexpr ResourceItem Runscript_Cmd() { return {}; }
constexpr ResourceItem Runscript_Target() { return {}; }
constexpr ResourceItem Runscript_Bool() { return {}; }
constexpr ResourceItem Runscript_When() { return {}; }
constexpr ResourceItem MigType() { return {}; }
constexpr ResourceItem IncExc() { return {}; }
constexpr ResourceItem Run() { return {}; }
constexpr ResourceItem ActionOnPurge() { return {}; }
constexpr ResourceItem PoolType() { return {}; }

// Director fileset options. handlers in dird_conf.
constexpr ResourceItem FName() { return {}; }
constexpr ResourceItem PluginName() { return {}; }
constexpr ResourceItem ExcludeDir() { return {}; }
constexpr ResourceItem Options() { return {}; }
constexpr ResourceItem Option() { return {}; }
constexpr ResourceItem Regex() { return {}; }
constexpr ResourceItem Base() { return {}; }
constexpr ResourceItem Wild() { return {}; }
constexpr ResourceItem Plugin() { return {}; }
constexpr ResourceItem FsType() { return {}; }
constexpr ResourceItem Drivetype() { return {}; }
constexpr ResourceItem Meta() { return {}; }

// Storage daemon resource types
constexpr ResourceItem MaxBlocksize() { return {}; }
constexpr ResourceItem IoDirection() { return {}; }
constexpr ResourceItem CmprsAlgo() { return {}; }

// File daemon resource types
constexpr ResourceItem Cipher() { return {}; }
#endif
}  // namespace config

#endif  // BAREOS_LIB_RESOURCE_ITEM_H_
