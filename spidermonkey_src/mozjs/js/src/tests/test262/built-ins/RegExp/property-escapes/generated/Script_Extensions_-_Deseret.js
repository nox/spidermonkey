// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2019 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=Deseret`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v12.1.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x010400, 0x01044F]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=Deseret}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Deseret}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Dsrt}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Dsrt}"
);
testPropertyEscapes(
  /^\p{scx=Deseret}+$/u,
  matchSymbols,
  "\\p{scx=Deseret}"
);
testPropertyEscapes(
  /^\p{scx=Dsrt}+$/u,
  matchSymbols,
  "\\p{scx=Dsrt}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x0103FF],
    [0x010450, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=Deseret}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Deseret}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Dsrt}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Dsrt}"
);
testPropertyEscapes(
  /^\P{scx=Deseret}+$/u,
  nonMatchSymbols,
  "\\P{scx=Deseret}"
);
testPropertyEscapes(
  /^\P{scx=Dsrt}+$/u,
  nonMatchSymbols,
  "\\P{scx=Dsrt}"
);

reportCompare(0, 0);
