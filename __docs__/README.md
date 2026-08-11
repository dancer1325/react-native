# React Native Technical Documentation

* goal
  * how React Native 
    * works internally
    * is composed of
    * components interact with each other

* this repo
  * follows
    * monorepo approach
  * provides
    * React Native
      * Android version
      * iOS version
      * ⚠️if you want React Native | OTHER platforms -> maintained [outside](../ECOSYSTEM.md)⚠️

## 🚀 Usage

* [several packages](../packages) /
  * SOME 
    * are published SEPARATELY | NPM registry
      * Reason:🧠check | "package.json", `"private": true`🧠
  * ["react-native"](../packages/react-native)
    * the MOST important package /
      * contains: JS API

## 📐 Design

TODO: Explain the different components of React Native at a high level.

## 🔗 Relationship with other systems

### Part of this

- Runtime
  - Cross-platform
    - [Feature Flags](../packages/react-native/src/private/featureflags/__docs__/README.md)
    - Host / Instance / Bridgeless
    - UI / Fabric
      - Events
      - Shadow Tree Lifecycle
        - [Runtime Shadow Node Reference Update](../packages/react-native/ReactCommon/react/renderer/core/__docs__/RSNRU.md)
        - [passChildrenWhenCloningPersistedNodes](../packages/react-native/ReactCommon/react/renderer/core/__docs__/passChildrenWhenCloning.md)
      - Layout
      - Mounting
      - [Animation Backend](../packages/react-native/ReactCommon/react/renderer/animationbackend/__docs__/AnimationBackend.md)
    - Native Modules / TurboModules
    - JS Runtime
      - [Event Loop](../packages/react-native/ReactCommon/react/renderer/runtimescheduler/__docs__/README.md)
      - Globals and environment setup
      - Error handling
    - Developer Tools
      - React Native DevTools
        - Infrastructure
          - [Inspector proxy protocol](../packages/dev-middleware/src/inspector-proxy/__docs__/README.md)
      - LogBox
    - Misc
      - Web APIs
        - DOM Traversal & Layout APIs
        - [IntersectionObserver](../packages/react-native/src/private/webapis/intersectionobserver/__docs__/README.md)
        - [MutationObserver](../packages/react-native/src/private/webapis/mutationobserver/__docs__/README.md)
        - Performance & PerformanceObserver
        - Timers
  - Platform-specific
    - Host Platform Interface
  - Android
    - UI
      - [Events](../packages/react-native/ReactAndroid/src/main/java/com/facebook/react/fabric/events/__docs__/README.md)
      - Mounting
  - iOS
    - UI
      - Events
      - Mounting
- Build system
  - Android
  - iOS
  - C++
  - JavaScript
    - Metro
- Testing
  - Android
  - iOS
  - C++
  - JavaScript
    - Flow
    - TypeScript
    - Jest
    - ESLint
  - Integration / E2E
    - [Fantom](../private/react-native-fantom/__docs__/README.md)

### Used by this

This repository has many different types of dependencies: build systems,
external packages to be used during development, external packages used at
runtime, etc.

### Uses this

The main use cases for this repository are:

1. Developing React Native itself.
2. Testing and releasing React Native.
3. Synchronizing forks like `react-native-windows` and `react-native-macos`.
