// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2019 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=Lydian`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v12.1.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [
    0x01093F
  ],
  ranges: [
    [0x010920, 0x010939]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=Lydian}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Lydian}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Lydi}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Lydi}"
);
testPropertyEscapes(
  /^\p{scx=Lydian}+$/u,
  matchSymbols,
  "\\p{scx=Lydian}"
);
testPropertyEscapes(
  /^\p{scx=Lydi}+$/u,
  matchSymbols,
  "\\p{scx=Lydi}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x01091F],
    [0x01093A, 0x01093E],
    [0x010940, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=Lydian}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Lydian}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Lydi}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Lydi}"
);
testPropertyEscapes(
  /^\P{scx=Lydian}+$/u,
  nonMatchSymbols,
  "\\P{scx=Lydian}"
);
testPropertyEscapes(
  /^\P{scx=Lydi}+$/u,
  nonMatchSymbols,
  "\\P{scx=Lydi}"
);

reportCompare(0, 0);
