// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2019 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=Caucasian_Albanian`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v12.1.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [
    0x01056F
  ],
  ranges: [
    [0x010530, 0x010563]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=Caucasian_Albanian}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Caucasian_Albanian}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Aghb}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Aghb}"
);
testPropertyEscapes(
  /^\p{scx=Caucasian_Albanian}+$/u,
  matchSymbols,
  "\\p{scx=Caucasian_Albanian}"
);
testPropertyEscapes(
  /^\p{scx=Aghb}+$/u,
  matchSymbols,
  "\\p{scx=Aghb}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x01052F],
    [0x010564, 0x01056E],
    [0x010570, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=Caucasian_Albanian}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Caucasian_Albanian}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Aghb}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Aghb}"
);
testPropertyEscapes(
  /^\P{scx=Caucasian_Albanian}+$/u,
  nonMatchSymbols,
  "\\P{scx=Caucasian_Albanian}"
);
testPropertyEscapes(
  /^\P{scx=Aghb}+$/u,
  nonMatchSymbols,
  "\\P{scx=Aghb}"
);

reportCompare(0, 0);
