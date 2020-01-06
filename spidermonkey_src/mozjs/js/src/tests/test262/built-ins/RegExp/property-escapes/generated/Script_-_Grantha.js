// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2019 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script=Grantha`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v12.1.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [
    0x011350,
    0x011357
  ],
  ranges: [
    [0x011300, 0x011303],
    [0x011305, 0x01130C],
    [0x01130F, 0x011310],
    [0x011313, 0x011328],
    [0x01132A, 0x011330],
    [0x011332, 0x011333],
    [0x011335, 0x011339],
    [0x01133C, 0x011344],
    [0x011347, 0x011348],
    [0x01134B, 0x01134D],
    [0x01135D, 0x011363],
    [0x011366, 0x01136C],
    [0x011370, 0x011374]
  ]
});
testPropertyEscapes(
  /^\p{Script=Grantha}+$/u,
  matchSymbols,
  "\\p{Script=Grantha}"
);
testPropertyEscapes(
  /^\p{Script=Gran}+$/u,
  matchSymbols,
  "\\p{Script=Gran}"
);
testPropertyEscapes(
  /^\p{sc=Grantha}+$/u,
  matchSymbols,
  "\\p{sc=Grantha}"
);
testPropertyEscapes(
  /^\p{sc=Gran}+$/u,
  matchSymbols,
  "\\p{sc=Gran}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [
    0x011304,
    0x011329,
    0x011331,
    0x011334
  ],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x0112FF],
    [0x01130D, 0x01130E],
    [0x011311, 0x011312],
    [0x01133A, 0x01133B],
    [0x011345, 0x011346],
    [0x011349, 0x01134A],
    [0x01134E, 0x01134F],
    [0x011351, 0x011356],
    [0x011358, 0x01135C],
    [0x011364, 0x011365],
    [0x01136D, 0x01136F],
    [0x011375, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script=Grantha}+$/u,
  nonMatchSymbols,
  "\\P{Script=Grantha}"
);
testPropertyEscapes(
  /^\P{Script=Gran}+$/u,
  nonMatchSymbols,
  "\\P{Script=Gran}"
);
testPropertyEscapes(
  /^\P{sc=Grantha}+$/u,
  nonMatchSymbols,
  "\\P{sc=Grantha}"
);
testPropertyEscapes(
  /^\P{sc=Gran}+$/u,
  nonMatchSymbols,
  "\\P{sc=Gran}"
);

reportCompare(0, 0);
