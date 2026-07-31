# @react-native/babel-preset

* == Babel preset -- for -- React Native applications
  * 👀default one👀
    * == ❌if you do NOT want to customize "babel.config.*" -> NOT appear | your project's directory❌
  * use cases
    * | transform your app's source code

## Installation

```sh
# 1. npm
npm i @react-native/babel-preset --save-dev

# 2. via yarn
yarn add -D @react-native/babel-preset
```

## how to configure it?

* | your project's root directory,
  * create ["babel.config.js"](https://babeljs.io/docs/en/config-files/) /
    * Reason:🧠OTHERWISE, React Native use its own🧠
    * contains

      ```json
      {
        "presets": ["module:@react-native/babel-preset"]
      }
      ```
