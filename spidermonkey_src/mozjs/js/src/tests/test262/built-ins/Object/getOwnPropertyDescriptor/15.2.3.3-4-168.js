// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.2.3.3-4-168
description: >
    Object.getOwnPropertyDescriptor returns data desc for functions on
    built-ins (Error.prototype.constructor)
---*/

var desc = Object.getOwnPropertyDescriptor(Error.prototype, "constructor");

assert.sameValue(desc.value, Error.prototype.constructor, 'desc.value');
assert.sameValue(desc.writable, true, 'desc.writable');
assert.sameValue(desc.enumerable, false, 'desc.enumerable');
assert.sameValue(desc.configurable, true, 'desc.configurable');

reportCompare(0, 0);
