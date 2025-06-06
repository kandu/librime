//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-07 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_REGISTRY_H_
#define RIME_REGISTRY_H_

#include <rime_api.h>
#include <rime/common.h>

namespace rime {

class ComponentBase;

class Registry {
 public:
  using ComponentMap = map<string, ComponentBase*>;

  RIME_DLL ComponentBase* Find(const string& name);
  RIME_DLL void Register(const string& name, ComponentBase* component);
  RIME_DLL void Unregister(const string& name);
  void Clear();

  RIME_DLL static Registry& instance();

 private:
  Registry() = default;

  ComponentMap map_;
};

}  // namespace rime

#endif  // RIME_REGISTRY_H_
