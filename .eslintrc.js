"use strict";

module.exports = {
<<<<<<< HEAD
  // When adding items to this file please check for effects on sub-directories.
  "plugins": [
    "mozilla"
  ],
  "rules": {
    "mozilla/avoid-removeChild": "error",
    "mozilla/avoid-nsISupportsString-preferences": "error",
    "mozilla/import-browser-window-globals": "error",
    "mozilla/import-globals": "warn",
    "mozilla/no-import-into-var-and-global": "error",
    "mozilla/no-useless-parameters": "error",
    "mozilla/no-useless-removeEventListener": "error",
    "mozilla/use-default-preference-values": "error",
    "mozilla/use-ownerGlobal": "error",

    // No (!foo in bar) or (!object instanceof Class)
    "no-unsafe-negation": "error",
  },
  "env": {
    "es6": true
  },
  "parserOptions": {
    "ecmaVersion": 8,
=======
  // New rules and configurations should generally be added in
  // tools/lint/eslint/eslint-plugin-mozilla/lib/configs/recommended.js to
  // allow external repositories that use the plugin to pick them up as well.
  "extends": [
    "plugin:mozilla/recommended"
  ],
  "plugins": [
    "mozilla"
  ],
  // The html plugin is enabled via a command line option on eslint. To avoid
  // bad interactions with the xml preprocessor in eslint-plugin-mozilla, we
  // turn off processing of the html plugin for .xml files.
  "settings": {
    "html/xml-extensions": [ ".xhtml" ]
>>>>>>> a17af05f6f9f79dd18cb2721cc8d0a0dfa1d04de
  },
};
