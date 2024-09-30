/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Binding.h"

#include "AsyncEventBeat.h"
#include "ComponentFactory.h"
#include "EventBeatManager.h"
#include "EventEmitterWrapper.h"
#include "FabricMountingManager.h"
#include "ReactNativeConfigHolder.h"

#include <cxxreact/SystraceSection.h>
#include <fbjni/fbjni.h>
#include <glog/logging.h>
#include <jsi/JSIDynamic.h>
#include <jsi/jsi.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/animations/LayoutAnimationDriver.h>
#include <react/renderer/componentregistry/ComponentDescriptorFactory.h>
#include <react/renderer/core/EventBeat.h>
#include <react/renderer/core/EventEmitter.h>
#include <react/renderer/core/conversions.h>
#include <react/renderer/scheduler/Scheduler.h>
#include <react/renderer/scheduler/SchedulerDelegate.h>
#include <react/renderer/scheduler/SchedulerToolbox.h>
#include <react/renderer/uimanager/primitives.h>
#include <react/utils/ContextContainer.h>
#include <react/utils/CoreFeatures.h>

namespace facebook::react {

jni::local_ref<Binding::jhybriddata> Binding::initHybrid(
    jni::alias_ref<jclass>) {
  return makeCxxInstance();
}

// Thread-safe getter
std::shared_ptr<Scheduler> Binding::getScheduler() {
  std::shared_lock lock(installMutex_);
  // Need to return a copy of the shared_ptr to make sure this is safe if called
  // concurrently with uninstallFabricUIManager
  return scheduler_;
}

jni::local_ref<ReadableNativeMap::jhybridobject>
Binding::getInspectorDataForInstance(
    jni::alias_ref<EventEmitterWrapper::javaobject> eventEmitterWrapper) {
  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::startSurface: scheduler disappeared";
    return ReadableNativeMap::newObjectCxxArgs(folly::dynamic::object());
  }

  EventEmitterWrapper* cEventEmitter = cthis(eventEmitterWrapper);
  InspectorData data =
      scheduler->getInspectorDataForInstance(*cEventEmitter->eventEmitter);

  folly::dynamic result = folly::dynamic::object;
  result["fileName"] = data.fileName;
  result["lineNumber"] = data.lineNumber;
  result["columnNumber"] = data.columnNumber;
  result["selectedIndex"] = data.selectedIndex;
  result["props"] = data.props;
  auto hierarchy = folly::dynamic::array();
  for (const auto& hierarchyItem : data.hierarchy) {
    hierarchy.push_back(hierarchyItem);
  }
  result["hierarchy"] = hierarchy;
  return ReadableNativeMap::newObjectCxxArgs(result);
}

constexpr static auto kReactFeatureFlagsJavaDescriptor =
    "com/facebook/react/config/ReactFeatureFlags";

static bool getFeatureFlagValue(const char* name) {
  static const auto reactFeatureFlagsClass =
      jni::findClassStatic(kReactFeatureFlagsJavaDescriptor);
  const auto field = reactFeatureFlagsClass->getStaticField<jboolean>(name);
  return reactFeatureFlagsClass->getStaticFieldValue(field);
}

void Binding::setPixelDensity(float pointScaleFactor) {
  pointScaleFactor_ = pointScaleFactor;
}

void Binding::driveCxxAnimations() {
  getScheduler()->animationTick();
}

void Binding::drainPreallocateViewsQueue() {
  auto mountingManager = getMountingManager("drainPreallocateViewsQueue");
  if (!mountingManager) {
    return;
  }
  mountingManager->drainPreallocateViewsQueue();
}

void Binding::reportMount(SurfaceId surfaceId) {
  if (ReactNativeFeatureFlags::
          fixMountingCoordinatorReportedPendingTransactionsOnAndroid()) {
    // This is a fix for `MountingCoordinator::hasPendingTransactions` on
    // Android, which otherwise would report no pending transactions
    // incorrectly. This is due to the push model used on Android and can be
    // removed when we migrate to a pull model.
    std::shared_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator != surfaceHandlerRegistry_.end()) {
      const auto* surfaceHandler =
          std::get_if<SurfaceHandler>(&iterator->second);
      if (surfaceHandler == nullptr) {
        auto javaSurfaceHandler =
            std::get<jni::weak_ref<SurfaceHandlerBinding::jhybridobject>>(
                iterator->second)
                .lockLocal();
        if (javaSurfaceHandler) {
          surfaceHandler = &javaSurfaceHandler->cthis()->getSurfaceHandler();
        }
      }
      if (surfaceHandler != nullptr) {
        surfaceHandler->getMountingCoordinator()->didPerformAsyncTransactions();
      }
    } else {
      LOG(ERROR) << "Binding::reportMount: Surface with id " << surfaceId
                 << " is not found";
    }
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::reportMount: scheduler disappeared";
    return;
  }
  scheduler->reportMount(surfaceId);
}

#pragma mark - Surface management

// Used by bridgeless
void Binding::startSurfaceWithSurfaceHandler(
    jint surfaceId,
    jni::alias_ref<SurfaceHandlerBinding::jhybridobject> surfaceHandlerBinding,
    jboolean isMountable) {
  SystraceSection s("FabricUIManagerBinding::startSurfaceWithSurfaceHandler");
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "Binding::startSurfaceWithSurfaceHandler() was called (address: "
        << this << ", surfaceId: " << surfaceId << ").";
  }

  const auto& surfaceHandler =
      surfaceHandlerBinding->cthis()->getSurfaceHandler();
  surfaceHandler.setSurfaceId(surfaceId);
  surfaceHandler.setDisplayMode(
      isMountable != 0 ? DisplayMode::Visible : DisplayMode::Suspended);

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "Binding::startSurfaceWithSurfaceHandler: scheduler disappeared";
    return;
  }
  scheduler->registerSurface(surfaceHandler);

  surfaceHandler.start();

  surfaceHandler.getMountingCoordinator()->setMountingOverrideDelegate(
      animationDriver_);

  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    surfaceHandlerRegistry_.emplace(
        surfaceHandler.getSurfaceId(), jni::make_weak(surfaceHandlerBinding));
  }

  auto mountingManager = getMountingManager("startSurfaceWithSurfaceHandler");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStart(surfaceHandler.getSurfaceId());
}

// Used by non-bridgeless+Fabric
void Binding::startSurface(
    jint surfaceId,
    jni::alias_ref<jstring> moduleName,
    NativeMap* initialProps) {
  SystraceSection s("FabricUIManagerBinding::startSurface");

  if (enableFabricLogs_) {
    LOG(WARNING) << "Binding::startSurface() was called (address: " << this
                 << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::startSurface: scheduler disappeared";
    return;
  }

  auto layoutContext = LayoutContext{};
  layoutContext.pointScaleFactor = pointScaleFactor_;

  auto surfaceHandler = SurfaceHandler{moduleName->toStdString(), surfaceId};
  surfaceHandler.setContextContainer(scheduler->getContextContainer());
  surfaceHandler.setProps(initialProps->consume());
  surfaceHandler.constraintLayout({}, layoutContext);

  scheduler->registerSurface(surfaceHandler);

  surfaceHandler.start();

  surfaceHandler.getMountingCoordinator()->setMountingOverrideDelegate(
      animationDriver_);

  {
    SystraceSection s2("FabricUIManagerBinding::startSurface::surfaceId::lock");
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    SystraceSection s3("FabricUIManagerBinding::startSurface::surfaceId");
    surfaceHandlerRegistry_.emplace(surfaceId, std::move(surfaceHandler));
  }

  auto mountingManager = getMountingManager("startSurface");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStart(surfaceId);
}

// Used by non-bridgeless+Fabric
void Binding::startSurfaceWithConstraints(
    jint surfaceId,
    jni::alias_ref<jstring> moduleName,
    NativeMap* initialProps,
    jfloat minWidth,
    jfloat maxWidth,
    jfloat minHeight,
    jfloat maxHeight,
    jfloat offsetX,
    jfloat offsetY,
    jboolean isRTL,
    jboolean doLeftAndRightSwapInRTL) {
  SystraceSection s("FabricUIManagerBinding::startSurfaceWithConstraints");

  if (enableFabricLogs_) {
    LOG(WARNING)
        << "Binding::startSurfaceWithConstraints() was called (address: "
        << this << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::startSurfaceWithConstraints: scheduler disappeared";
    return;
  }

  auto minimumSize =
      Size{minWidth / pointScaleFactor_, minHeight / pointScaleFactor_};
  auto maximumSize =
      Size{maxWidth / pointScaleFactor_, maxHeight / pointScaleFactor_};

  LayoutContext context;
  context.viewportOffset =
      Point{offsetX / pointScaleFactor_, offsetY / pointScaleFactor_};
  context.pointScaleFactor = {pointScaleFactor_};
  context.swapLeftAndRightInRTL = doLeftAndRightSwapInRTL;
  LayoutConstraints constraints = {};
  constraints.minimumSize = minimumSize;
  constraints.maximumSize = maximumSize;
  constraints.layoutDirection =
      isRTL ? LayoutDirection::RightToLeft : LayoutDirection::LeftToRight;

  auto surfaceHandler = SurfaceHandler{moduleName->toStdString(), surfaceId};
  surfaceHandler.setContextContainer(scheduler->getContextContainer());
  surfaceHandler.setProps(initialProps->consume());
  surfaceHandler.constraintLayout(constraints, context);

  scheduler->registerSurface(surfaceHandler);

  surfaceHandler.start();

  surfaceHandler.getMountingCoordinator()->setMountingOverrideDelegate(
      animationDriver_);

  {
    SystraceSection s2(
        "FabricUIManagerBinding::startSurfaceWithConstraints::surfaceId::lock");
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    SystraceSection s3(
        "FabricUIManagerBinding::startSurfaceWithConstraints::surfaceId");
    surfaceHandlerRegistry_.emplace(surfaceId, std::move(surfaceHandler));
  }

  auto mountingManager = getMountingManager("startSurfaceWithConstraints");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStart(surfaceId);
}

// Used by non-bridgeless+Fabric
void Binding::stopSurface(jint surfaceId) {
  SystraceSection s("FabricUIManagerBinding::stopSurface");
  if (enableFabricLogs_) {
    LOG(WARNING) << "Binding::stopSurface() was called (address: " << this
                 << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::stopSurface: scheduler disappeared";
    return;
  }

  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator == surfaceHandlerRegistry_.end()) {
      LOG(ERROR) << "Binding::stopSurface: Surface with given id is not found";
      return;
    }

    auto* surfaceHandler = std::get_if<SurfaceHandler>(&iterator->second);
    if (surfaceHandler != nullptr) {
      surfaceHandler->stop();
      scheduler->unregisterSurface(*surfaceHandler);
    } else {
      LOG(ERROR) << "Java-owned SurfaceHandler found in stopSurface";
    }
    surfaceHandlerRegistry_.erase(iterator);
  }

  auto mountingManager = getMountingManager("stopSurface");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStop(surfaceId);
}

// Used by bridgeless
void Binding::stopSurfaceWithSurfaceHandler(
    jni::alias_ref<SurfaceHandlerBinding::jhybridobject>
        surfaceHandlerBinding) {
  SystraceSection s("FabricUIManagerBinding::stopSurfaceWithSurfaceHandler");
  const auto& surfaceHandler =
      surfaceHandlerBinding->cthis()->getSurfaceHandler();
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "Binding::stopSurfaceWithSurfaceHandler() was called (address: "
        << this << ", surfaceId: " << surfaceHandler.getSurfaceId() << ").";
  }

  surfaceHandler.stop();

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::unregisterSurface: scheduler disappeared";
    return;
  }
  scheduler->unregisterSurface(surfaceHandler);

  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    surfaceHandlerRegistry_.erase(surfaceHandler.getSurfaceId());
  }

  auto mountingManager = getMountingManager("unregisterSurface");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStop(surfaceHandler.getSurfaceId());
}

void Binding::setConstraints(
    jint surfaceId,
    jfloat minWidth,
    jfloat maxWidth,
    jfloat minHeight,
    jfloat maxHeight,
    jfloat offsetX,
    jfloat offsetY,
    jboolean isRTL,
    jboolean doLeftAndRightSwapInRTL) {
  SystraceSection s("FabricUIManagerBinding::setConstraints");

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "Binding::setConstraints: scheduler disappeared";
    return;
  }

  auto minimumSize =
      Size{minWidth / pointScaleFactor_, minHeight / pointScaleFactor_};
  auto maximumSize =
      Size{maxWidth / pointScaleFactor_, maxHeight / pointScaleFactor_};

  LayoutContext context;
  context.viewportOffset =
      Point{offsetX / pointScaleFactor_, offsetY / pointScaleFactor_};
  context.pointScaleFactor = {pointScaleFactor_};
  context.swapLeftAndRightInRTL = doLeftAndRightSwapInRTL;
  LayoutConstraints constraints = {};
  constraints.minimumSize = minimumSize;
  constraints.maximumSize = maximumSize;
  constraints.layoutDirection =
      isRTL ? LayoutDirection::RightToLeft : LayoutDirection::LeftToRight;

  {
    std::shared_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator == surfaceHandlerRegistry_.end()) {
      LOG(ERROR)
          << "Binding::setConstraints: Surface with given id is not found";
      return;
    }
    auto* surfaceHandler = std::get_if<SurfaceHandler>(&iterator->second);
    if (surfaceHandler != nullptr) {
      surfaceHandler->constraintLayout(constraints, context);
    }
  }
}

#pragma mark - Install/uninstall java binding

void Binding::installFabricUIManager(
    jni::alias_ref<JRuntimeExecutor::javaobject> runtimeExecutorHolder,
    jni::alias_ref<JRuntimeScheduler::javaobject> runtimeSchedulerHolder,
    jni::alias_ref<JFabricUIManager::javaobject> javaUIManager,
    EventBeatManager* eventBeatManager,
    ComponentFactory* componentsRegistry,
    jni::alias_ref<jobject> reactNativeConfig) {
  SystraceSection s("FabricUIManagerBinding::installFabricUIManager");

  std::shared_ptr<const ReactNativeConfig> config =
      std::make_shared<const ReactNativeConfigHolder>(reactNativeConfig);

  enableFabricLogs_ = ReactNativeFeatureFlags::enableFabricLogs();

  if (enableFabricLogs_) {
    LOG(WARNING) << "Binding::installFabricUIManager() was called (address: "
                 << this << ").";
  }

  std::unique_lock lock(installMutex_);

  auto globalJavaUiManager = make_global(javaUIManager);
  mountingManager_ =
      std::make_shared<FabricMountingManager>(config, globalJavaUiManager);

  ContextContainer::Shared contextContainer =
      std::make_shared<ContextContainer>();

  auto runtimeExecutor = runtimeExecutorHolder->cthis()->get();

  if (runtimeSchedulerHolder) {
    auto runtimeScheduler = runtimeSchedulerHolder->cthis()->get().lock();
    if (runtimeScheduler) {
      runtimeExecutor =
          [runtimeScheduler](
              std::function<void(jsi::Runtime & runtime)>&& callback) {
            runtimeScheduler->scheduleWork(std::move(callback));
          };
      contextContainer->insert(
          "RuntimeScheduler",
          std::weak_ptr<RuntimeScheduler>(runtimeScheduler));
    }
  }

  EventBeat::Factory asynchronousBeatFactory =
      [eventBeatManager, runtimeExecutor, globalJavaUiManager](
          const EventBeat::SharedOwnerBox& ownerBox)
      -> std::unique_ptr<EventBeat> {
    return std::make_unique<AsyncEventBeat>(
        ownerBox, eventBeatManager, runtimeExecutor, globalJavaUiManager);
  };

  contextContainer->insert("ReactNativeConfig", config);
  contextContainer->insert("FabricUIManager", globalJavaUiManager);

  // Keep reference to config object and cache some feature flags here
  reactNativeConfig_ = config;

  CoreFeatures::enablePropIteratorSetter =
      getFeatureFlagValue("enableCppPropsIteratorSetter");
  CoreFeatures::excludeYogaFromRawProps =
      ReactNativeFeatureFlags::excludeYogaFromRawProps();

  auto toolbox = SchedulerToolbox{};
  toolbox.contextContainer = contextContainer;
  toolbox.componentRegistryFactory = componentsRegistry->buildRegistryFunction;

  // TODO: (T132338609) runtimeExecutor should execute lambdas after
  // main bundle eval, and bindingsInstallExecutor should execute before.
  toolbox.bridgelessBindingsExecutor = std::nullopt;
  toolbox.runtimeExecutor = runtimeExecutor;

  toolbox.asynchronousEventBeatFactory = asynchronousBeatFactory;

  animationDriver_ = std::make_shared<LayoutAnimationDriver>(
      runtimeExecutor, contextContainer, this);
  scheduler_ =
      std::make_shared<Scheduler>(toolbox, animationDriver_.get(), this);
}

void Binding::uninstallFabricUIManager() {
  if (enableFabricLogs_) {
    LOG(WARNING) << "Binding::uninstallFabricUIManager() was called (address: "
                 << this << ").";
  }

  std::unique_lock lock(installMutex_);
  animationDriver_ = nullptr;
  scheduler_ = nullptr;
  mountingManager_ = nullptr;
  reactNativeConfig_ = nullptr;
}

std::shared_ptr<FabricMountingManager> Binding::getMountingManager(
    const char* locationHint) {
  std::shared_lock lock(installMutex_);
  if (!mountingManager_) {
    LOG(ERROR) << "FabricMountingManager::" << locationHint
               << " mounting manager disappeared";
  }
  // Need to return a copy of the shared_ptr to make sure this is safe if called
  // concurrently with uninstallFabricUIManager
  return mountingManager_;
}

void Binding::schedulerDidFinishTransaction(
    const MountingCoordinator::Shared& mountingCoordinator) {
  // We shouldn't be pulling the transaction here (which triggers diffing of
  // the trees to determine the mutations to run on the host platform),
  // but we have to due to current limitations in the Android implementation.
  auto mountingTransaction = mountingCoordinator->pullTransaction(
      // Indicate that the transaction will be performed asynchronously
      ReactNativeFeatureFlags::
          fixMountingCoordinatorReportedPendingTransactionsOnAndroid());
  if (!mountingTransaction.has_value()) {
    return;
  }

  std::unique_lock<std::mutex> lock(pendingTransactionsMutex_);
  auto pendingTransaction = std::find_if(
      pendingTransactions_.begin(),
      pendingTransactions_.end(),
      [&](const auto& transaction) {
        return transaction.getSurfaceId() ==
            mountingTransaction->getSurfaceId();
      });

  if (pendingTransaction != pendingTransactions_.end()) {
    pendingTransaction->mergeWith(std::move(*mountingTransaction));
  } else {
    pendingTransactions_.push_back(std::move(*mountingTransaction));
  }
}

void Binding::schedulerShouldRenderTransactions(
    const MountingCoordinator::Shared& mountingCoordinator) {
  auto mountingManager =
      getMountingManager("schedulerShouldRenderTransactions");
  if (!mountingManager) {
    return;
  }

  if (ReactNativeFeatureFlags::
          allowRecursiveCommitsWithSynchronousMountOnAndroid()) {
    std::vector<MountingTransaction> pendingTransactions;

    {
      // Retain the lock to access the pending transactions but not to execute
      // the mount operations because that method can call into this method
      // again.
      std::unique_lock<std::mutex> lock(pendingTransactionsMutex_);
      pendingTransactions_.swap(pendingTransactions);
    }

    for (auto& transaction : pendingTransactions) {
      mountingManager->executeMount(transaction);
    }
  } else {
    std::unique_lock<std::mutex> lock(pendingTransactionsMutex_);
    for (auto& transaction : pendingTransactions_) {
      mountingManager->executeMount(transaction);
    }
    pendingTransactions_.clear();
  }
}

void Binding::schedulerDidRequestPreliminaryViewAllocation(
    const ShadowNode& shadowNode) {
  auto mountingManager = getMountingManager("preallocateView");
  if (!mountingManager) {
    return;
  }
  mountingManager->maybePreallocateShadowNode(shadowNode);
  // Only the Views of ShadowNode that were pre-allocated (forms views) needs
  // to be destroyed if the ShadowNode is destroyed but it was never mounted
  // on the screen.
  if (ReactNativeFeatureFlags::enableDeletionOfUnmountedViews() &&
      shadowNode.getTraits().check(ShadowNodeTraits::Trait::FormsView)) {
    shadowNode.getFamily().onUnmountedFamilyDestroyed(
        [weakMountingManager =
             std::weak_ptr(mountingManager)](const ShadowNodeFamily& family) {
          if (auto mountingManager = weakMountingManager.lock()) {
            mountingManager->destroyUnmountedShadowNode(family);
          }
        });
  }
}

void Binding::schedulerDidDispatchCommand(
    const ShadowView& shadowView,
    const std::string& commandName,
    const folly::dynamic& args) {
  auto mountingManager = getMountingManager("schedulerDidDispatchCommand");
  if (!mountingManager) {
    return;
  }
  mountingManager->dispatchCommand(shadowView, commandName, args);
}

void Binding::schedulerDidSendAccessibilityEvent(
    const ShadowView& shadowView,
    const std::string& eventType) {
  auto mountingManager =
      getMountingManager("schedulerDidSendAccessibilityEvent");
  if (!mountingManager) {
    return;
  }
  mountingManager->sendAccessibilityEvent(shadowView, eventType);
}

void Binding::schedulerDidSetIsJSResponder(
    const ShadowView& shadowView,
    bool isJSResponder,
    bool blockNativeResponder) {
  auto mountingManager = getMountingManager("schedulerDidSetIsJSResponder");
  if (!mountingManager) {
    return;
  }
  mountingManager->setIsJSResponder(
      shadowView, isJSResponder, blockNativeResponder);
}

void Binding::onAnimationStarted() {
  auto mountingManager = getMountingManager("onAnimationStarted");
  if (!mountingManager) {
    return;
  }
  mountingManager->onAnimationStarted();
}

void Binding::onAllAnimationsComplete() {
  auto mountingManager = getMountingManager("onAnimationComplete");
  if (!mountingManager) {
    return;
  }
  mountingManager->onAllAnimationsComplete();
}

void Binding::registerNatives() {
  registerHybrid({
      makeNativeMethod("initHybrid", Binding::initHybrid),
      makeNativeMethod(
          "installFabricUIManager", Binding::installFabricUIManager),
      makeNativeMethod("startSurface", Binding::startSurface),
      makeNativeMethod(
          "getInspectorDataForInstance", Binding::getInspectorDataForInstance),
      makeNativeMethod(
          "startSurfaceWithConstraints", Binding::startSurfaceWithConstraints),
      makeNativeMethod("stopSurface", Binding::stopSurface),
      makeNativeMethod("setConstraints", Binding::setConstraints),
      makeNativeMethod("setPixelDensity", Binding::setPixelDensity),
      makeNativeMethod("driveCxxAnimations", Binding::driveCxxAnimations),
      makeNativeMethod(
          "drainPreallocateViewsQueue", Binding::drainPreallocateViewsQueue),
      makeNativeMethod("reportMount", Binding::reportMount),
      makeNativeMethod(
          "uninstallFabricUIManager", Binding::uninstallFabricUIManager),
      makeNativeMethod(
          "startSurfaceWithSurfaceHandler",
          Binding::startSurfaceWithSurfaceHandler),
      makeNativeMethod(
          "stopSurfaceWithSurfaceHandler",
          Binding::stopSurfaceWithSurfaceHandler),
  });
}

} // namespace facebook::react
