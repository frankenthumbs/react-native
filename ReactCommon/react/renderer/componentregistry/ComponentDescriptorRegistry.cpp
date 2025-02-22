/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ComponentDescriptorRegistry.h"

#include "componentNameByReactViewName.h"

#include <react/debug/react_native_assert.h>
#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/ShadowNodeFragment.h>

namespace facebook {
namespace react {

ComponentDescriptorRegistry::ComponentDescriptorRegistry(
    ComponentDescriptorParameters const &parameters,
    ComponentDescriptorProviderRegistry const &providerRegistry,
    ContextContainer::Shared contextContainer)
    : parameters_(parameters),
      providerRegistry_(providerRegistry),
      contextContainer_(contextContainer) {}

void ComponentDescriptorRegistry::add(
    ComponentDescriptorProvider componentDescriptorProvider) const {
  std::unique_lock<butter::shared_mutex> lock(mutex_);

  auto componentDescriptor = componentDescriptorProvider.constructor(
      {parameters_.eventDispatcher,
       parameters_.contextContainer,
       componentDescriptorProvider.flavor});
  react_native_assert(
      componentDescriptor->getComponentHandle() ==
      componentDescriptorProvider.handle);
  react_native_assert(
      componentDescriptor->getComponentName() ==
      componentDescriptorProvider.name);

  auto sharedComponentDescriptor = std::shared_ptr<ComponentDescriptor const>(
      std::move(componentDescriptor));
  _registryByHandle[componentDescriptorProvider.handle] =
      sharedComponentDescriptor;
  _registryByName[componentDescriptorProvider.name] = sharedComponentDescriptor;
}

void ComponentDescriptorRegistry::registerComponentDescriptor(
    SharedComponentDescriptor componentDescriptor) const {
  ComponentHandle componentHandle = componentDescriptor->getComponentHandle();
  _registryByHandle[componentHandle] = componentDescriptor;

  ComponentName componentName = componentDescriptor->getComponentName();
  _registryByName[componentName] = componentDescriptor;
}

ComponentDescriptor const &ComponentDescriptorRegistry::at(
    std::string const &componentName) const {
  std::shared_lock<butter::shared_mutex> lock(mutex_);

  auto unifiedComponentName = componentNameByReactViewName(componentName);

  auto it = _registryByName.find(unifiedComponentName);
  if (it == _registryByName.end()) {
    lock.unlock();
    providerRegistry_.request(unifiedComponentName.c_str());
    lock.lock();

    it = _registryByName.find(unifiedComponentName);

    /*
     * TODO: T54849676
     * Uncomment the `assert` after the following block that checks
     * `_fallbackComponentDescriptor` is no longer needed. The assert assumes
     * that `componentDescriptorProviderRequest` is always not null and register
     * some component on every single request.
     */
    // assert(it != _registryByName.end());
  }

  if (it == _registryByName.end()) {
    if (_fallbackComponentDescriptor == nullptr) {
      throw std::invalid_argument(
          ("Unable to find componentDescriptor for " + unifiedComponentName)
              .c_str());
    }
    return *_fallbackComponentDescriptor.get();
  }

  return *it->second;
}

ComponentDescriptor const *ComponentDescriptorRegistry::
    findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN(
        ComponentHandle componentHandle) const {
  std::shared_lock<butter::shared_mutex> lock(mutex_);

  auto iterator = _registryByHandle.find(componentHandle);
  if (iterator == _registryByHandle.end()) {
    return nullptr;
  }

  return iterator->second.get();
}

ComponentDescriptor const &ComponentDescriptorRegistry::at(
    ComponentHandle componentHandle) const {
  std::shared_lock<butter::shared_mutex> lock(mutex_);

  return *_registryByHandle.at(componentHandle);
}

bool ComponentDescriptorRegistry::hasComponentDescriptorAt(
    ComponentHandle componentHandle) const {
  std::shared_lock<butter::shared_mutex> lock(mutex_);

  auto iterator = _registryByHandle.find(componentHandle);
  if (iterator == _registryByHandle.end()) {
    return false;
  }

  return true;
}

SharedShadowNode ComponentDescriptorRegistry::createNode(
    Tag tag,
    std::string const &viewName,
    SurfaceId surfaceId,
    folly::dynamic const &propsDynamic,
    SharedEventTarget const &eventTarget) const {
  auto unifiedComponentName = componentNameByReactViewName(viewName);
  auto const &componentDescriptor = this->at(unifiedComponentName);

  auto const fragment = ShadowNodeFamilyFragment{tag, surfaceId, nullptr};
  auto family =
      componentDescriptor.createFamily(fragment, std::move(eventTarget));
  auto const props = componentDescriptor.cloneProps(
      PropsParserContext{surfaceId, *contextContainer_.get()},
      nullptr,
      RawProps(propsDynamic));
  auto const state =
      componentDescriptor.createInitialState(ShadowNodeFragment{props}, family);

  return componentDescriptor.createShadowNode(
      {
          /* .props = */ props,
          /* .children = */ ShadowNodeFragment::childrenPlaceholder(),
          /* .state = */ state,
      },
      family);
}

void ComponentDescriptorRegistry::setFallbackComponentDescriptor(
    SharedComponentDescriptor descriptor) {
  _fallbackComponentDescriptor = descriptor;
  registerComponentDescriptor(descriptor);
}

ComponentDescriptor::Shared
ComponentDescriptorRegistry::getFallbackComponentDescriptor() const {
  return _fallbackComponentDescriptor;
}

} // namespace react
} // namespace facebook
