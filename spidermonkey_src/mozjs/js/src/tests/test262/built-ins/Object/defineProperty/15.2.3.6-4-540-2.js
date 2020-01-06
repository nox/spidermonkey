// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.2.3.6-4-540-2
description: >
    Object.defineProperty fails to update [[Get]] and [[Set]]
    attributes of an indexed property 'P' whose [[Configurable]]
    attribute is false and throws TypeError exception, 'A' is an Array
    object (8.12.9 step 11.a)
includes: [propertyHelper.js]
---*/

var obj = [];

obj.verifySetFunction = "data";
var getFunc = function() {
  return obj.verifySetFunction;
};
var setFunc = function(value) {
  obj.verifySetFunction = value;
};
Object.defineProperty(obj, "0", {
  get: getFunc,
  set: setFunc,
  configurable: false
});

var result = false;
try {
  Object.defineProperty(obj, "0", {
    get: function() {
      return 100;
    }
  });
} catch (e) {
  result = e instanceof TypeError;
  verifyEqualTo(obj, "0", getFunc());

  verifyWritable(obj, "0", "verifySetFunction");

  verifyNotEnumerable(obj, "0");

  verifyNotConfigurable(obj, "0");
}

try {
  Object.defineProperty(obj, "0", {
    set: function(value) {
      obj.verifySetFunction1 = value;
    }
  });
} catch (e) {
  if (!result) {
    $ERROR('Expected result  to be true, actually ' + result);
  }

  verifyEqualTo(obj, "0", getFunc());

  verifyWritable(obj, "0", "verifySetFunction");

  verifyNotEnumerable(obj, "0");

  verifyNotConfigurable(obj, "0");

  if (!(e instanceof TypeError)) {
    $ERROR("Expected TypeError, got " + e);
  }

}

reportCompare(0, 0);
